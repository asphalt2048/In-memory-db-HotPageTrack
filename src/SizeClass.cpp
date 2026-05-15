#include "SizeClass.h"

namespace imdb{
void SizeClassManager::push_to_partial_list(Page *page){
    assert( !(page->header.next != nullptr || 
        page->header.prev != nullptr || partial_list_head == page) &&
        "FATAL: Tried to push linked page!");

    assert( !(page->header.used.load() == page->header.max_slots) &&
        "FATAL: Tried to push full page!");

    page->header.prev = nullptr;
    page->header.next = partial_list_head;

    if(partial_list_head){ partial_list_head->header.prev = page; }
    partial_list_head = page;
}

void SizeClassManager::remove_from_partial_list(Page *page){
    assert( !(page->header.next == nullptr && 
        page->header.prev == nullptr && partial_list_head != page) && 
        "FATAL: Tried to remove unlinked page!");

    if (page->header.prev){
        page->header.prev->header.next = page->header.next;
    }else{
        partial_list_head = page->header.next; 
    }
    if (page->header.next){
        page->header.next->header.prev = page->header.prev;
    }
    page->header.prev = nullptr;
    page->header.next = nullptr;
}


// TODO: heavy load on a single SC will cause it to ask many pages at once, which might not be prefered.

Page* SizeClassManager::get_a_page(){
    void* raw_addr = arena.alloc_a_page();
    /* This should never be nullptr, alloc_a_page must be successful */
    if(raw_addr == nullptr){
        std::cerr<<RED<<"Arena out of memory!\n"<<RESET;
        exit(-1);
    }

    Page* page = init_page(raw_addr);

    return page;
}

void SizeClassManager::return_a_page(Page* page){
    arena.remove_from_lru(page);
    arena.free_a_page(reinterpret_cast<void *>(page));
}

/* init a page with the correct format, setup interal free slots list */
Page* SizeClassManager::init_page(void* raw_page_base){
    Page* page = reinterpret_cast<Page*>(raw_page_base);
    page->header.used.store(0, std::memory_order_relaxed);
    page->header.is_quarantined.store(false, std::memory_order_relaxed);
    page->header.next = nullptr;
    page->header.prev = nullptr;
    page->header.lru_prev = nullptr;
    page->header.lru_next = nullptr;
    // page->header.page_id = arena.get_page_id(raw_page_base);
    page->header.slot_size = this->slot_size;

    /* dynamically sets up header_reserved and max_slots */
    size_t header_size = sizeof(Page);
    size_t header_slots = (header_size + slot_size - 1) / slot_size;
    page->header.header_reserved = header_slots * slot_size;
    page->header.max_slots = (Page::PAGE_SIZE - page->header.header_reserved) / slot_size;

    /* setup is_hot array */
    for(int i=0; i < IS_HOT_ARR_LENGTH; i++){
        page->is_hot[i].store(0ULL, std::memory_order_relaxed);
    }
    /* setup is_allocated array */
    for(int i=0; i < IS_ALLOCATED_ARR_LENGHT; i++){
        page->is_allocated[i].store(0ULL, std::memory_order_relaxed);
    }

    return page;
}

uint16_t SizeClassManager::get_free_slot(Page* page){
    uint16_t free_slot_idx = SLOT_END;

    for (int i = 0; i < IS_ALLOCATED_ARR_LENGHT; i++) {
        uint64_t chunk = page->is_allocated[i].load(std::memory_order_acquire);
        
        // If chunk is not entirely full (i.e not 0xFFFFFFFFFFFFFFFF)
        if (chunk != ~0ULL) {
            uint64_t inverted = ~chunk;
            int first_free_bit = __builtin_ctzll(inverted); 
            
            uint16_t idx = (i * 64) + first_free_bit;
            
            // is_allocated array length might be larger that actual max_slot
            // do this check to ensure safety
            if (idx < page->header.max_slots) {
                free_slot_idx = idx;
                break;
            }
        }
    }

    if (free_slot_idx == SLOT_END) {
        std::cerr<<RED<< "Error: SCM: try to alloc slot on a full page\n"<<RESET; exit(-1); 
    }
    return free_slot_idx;
}

/* alloate a slot, if page becomes full, stop tracking the page */
void* SizeClassManager::alloc(){
    std::unique_lock<std::shared_mutex> write_lock(rw_lock);

    if (partial_list_head != nullptr) {
        Page *page = partial_list_head;
        uint16_t slot_idx = get_free_slot(page);
        
        page->header.used.fetch_add(1);
        set_allocated_bit(page, slot_idx);

        if(page->header.used.load() == page->header.max_slots) {
            remove_from_partial_list(page);
        }
        
        return static_cast<void*>(page->get_slot_addr(slot_idx));
    }

    write_lock.unlock(); 
    // get page might sleep, drop the lock in case sweeper need it
    Page* new_page = get_a_page();
    write_lock.lock();

    uint16_t slot_idx = get_free_slot(new_page);
    new_page->header.used.fetch_add(1);
    set_allocated_bit(new_page, slot_idx);

    if (new_page->header.used < new_page->header.max_slots) {
        push_to_partial_list(new_page);
    }
    arena.add_to_lru(new_page);

    return static_cast<void*>(new_page->get_slot_addr(slot_idx));
}

/* allocate a slot if SCM have free space, never ask new page. Might fail */
void* SizeClassManager::alloc_notrigger(){
    std::unique_lock<std::shared_mutex> write_lock(rw_lock);

    Page* page = partial_list_head;

    if(page == nullptr) return nullptr;

    uint16_t slot_idx = get_free_slot(page);
    page->header.used.fetch_add(1);
    set_allocated_bit(page, slot_idx);

    if(page->header.used == page->header.max_slots){
        remove_from_partial_list(page);
    }

    return static_cast<void*>(page->get_slot_addr(slot_idx));
}

/* free a slot, if page become free, return it to the arena*/
void SizeClassManager::free(void* slot_addr){
    std::unique_lock<std::shared_mutex> write_lock(rw_lock);

    uintptr_t slot_addr_ = reinterpret_cast<uintptr_t>(slot_addr);

    uintptr_t page_base = PAGE_ALIGN(slot_addr_);
    Page *page = reinterpret_cast<Page*>(page_base);

    if(page->header.slot_size != slot_size){
        std::cerr<<RED<<"Error: call free on wrong SCM\n"<<RESET; exit(-1);
    }

    size_t offset = slot_addr_ - page_base;
    uint16_t slot_idx = (offset - page->header.header_reserved) / page->header.slot_size;

    clear_allocated_bit(page, slot_idx);
    uint16_t old_used = page->header.used.fetch_sub(1);

    // do updates on page's pointers if sweeper isn't quarantining it
    if (page->header.is_quarantined.load(std::memory_order_acquire)) {
        return; 
    }

    if(old_used == page->header.max_slots){
        push_to_partial_list(page);
    }

    if(old_used == 1){
        remove_from_partial_list(page);
        return_a_page(page);
    }

    return;
}

bool SizeClassManager::quarantine_page(Page* page){
    std::unique_lock<std::shared_mutex> write_lock(rw_lock);

    if(page->header.used.load(std::memory_order_relaxed) == 0){
        return false;
    }
    if(page->header.slot_size != slot_size){
        return false;
    }

    if(page->header.used.load(std::memory_order_relaxed) < page->header.max_slots){
        remove_from_partial_list(page);
    }
    page->header.is_quarantined.store(true, std::memory_order_release);
    return true;
}

Status SizeClassManager::unquarantine_page(Page* page){
    std::unique_lock<std::shared_mutex> write_lock(rw_lock);

    uint16_t current_used = page->header.used.load(std::memory_order_relaxed);
    
    // Page not fully cleared. But is a partial page
    if(current_used > 0 && current_used < page->header.max_slots){
        push_to_partial_list(page);
        arena.lift_in_lru(reinterpret_cast<void*>(page));
        page->header.is_quarantined.store(false, std::memory_order_release);
        return PARTIAL_PAGE_EVICTED;
    }
    // Page not fully cleared, and is still full after eviciton loop. Unlikely to happen
    if(current_used == page->header.max_slots){
        arena.lift_in_lru(reinterpret_cast<void*>(page));
        page->header.is_quarantined.store(false, std::memory_order_release);
        return PARTIAL_PAGE_EVICTED;
    }
    // Page fully cleared.
    if(current_used == 0){
        
        page->header.is_quarantined.store(false, std::memory_order_release);
        arena.remove_from_lru(reinterpret_cast<void*>(page));
        arena.free_a_page(reinterpret_cast<void*>(page));
        return FULL_PAGE_EVICTED;
    }
    return ERROR;
}


/*-------------------------Helper functions---------------------------------------*/
/* They are not bind to a size class(not a member funtion) for flexibilty reasons */

void promote_a_slot(void* slot_addr, uint8_t inc){
    Page* page = get_struct_page(slot_addr);
    uint16_t slot_id = page->get_slot_idx_nocheck(slot_addr);

    size_t arr_idx = slot_id / 32;
    size_t bit_idx = slot_id % 32;
    size_t bit_shift = bit_idx*2;

    // use CAS loop to update is_hot
    uint64_t old_arr = page->is_hot[arr_idx].load(std::memory_order_relaxed);
    while(true){
        uint64_t current_val = (old_arr>>bit_shift) & 0b11ULL;
        // already reached the max hotness, do nothing
        if(current_val == 3) break;

        uint64_t new_val = current_val + inc;
        if(new_val > 3) new_val = 3;

        uint64_t new_arr = (old_arr & ~(0b11ULL<<bit_shift)) | (new_val << bit_shift);
        if(page->is_hot[arr_idx].compare_exchange_weak(old_arr, new_arr, std::memory_order_relaxed)){
            break;
        }
    }
};


void age_a_slot(void* slot_addr){
    Page* page = get_struct_page(slot_addr);
    uint16_t slot_id = page->get_slot_idx_nocheck(slot_addr);

    size_t arr_idx = slot_id / 32;
    size_t bit_idx = slot_id % 32;
    size_t bit_shift = bit_idx*2;

    // use CAS loop to update is_hot
    uint64_t old_arr = page->is_hot[arr_idx].load(std::memory_order_relaxed);
    while(true){
        uint64_t current_val = (old_arr>>bit_shift) & 0b11ULL;
        // already reached the min hotness, do nothing
        if(current_val == 0) break;

        uint64_t new_val = current_val - 1;
        uint64_t new_arr = (old_arr & ~(0b11ULL<<bit_shift)) | (new_val << bit_shift);

        if(page->is_hot[arr_idx].compare_exchange_weak(old_arr, new_arr, std::memory_order_relaxed)){
            break;
        }
    }
}

void mark_slot_cold(void* slot_addr){
    Page* page = get_struct_page(slot_addr);
    uint16_t slot_id = page->get_slot_idx_nocheck(slot_addr);

    size_t arr_idx = slot_id / 32;
    size_t bit_idx = slot_id % 32;
    size_t bit_shift = bit_idx*2;

    page->is_hot[arr_idx].fetch_and(~(0b11ULL<<bit_shift), std::memory_order_relaxed);
};

uint8_t get_slot_hotness(void* slot_addr){
    Page* page = get_struct_page(slot_addr);
    uint16_t slot_id = page->get_slot_idx_nocheck(slot_addr);

    size_t arr_idx = slot_id / 32;
    size_t bit_idx = slot_id % 32;
    size_t bit_shift = bit_idx * 2;

    uint64_t chunk = page->is_hot[arr_idx].load(std::memory_order_relaxed);
    uint8_t val = (chunk>>bit_shift) & 0b11ULL;
    return val;
}

// Explicitly set the 2-bit hotness value safely
void set_slot_hotness(void* slot_addr, uint8_t exact_val){
    Page* page = get_struct_page(slot_addr);
    uint16_t slot_id = page->get_slot_idx_nocheck(slot_addr);

    size_t arr_idx = slot_id / 32;
    size_t bit_shift = (slot_id % 32) * 2;

    uint64_t old_arr = page->is_hot[arr_idx].load(std::memory_order_relaxed);
    while (true){
        uint64_t new_arr = (old_arr & ~(0b11ULL << bit_shift)) | (static_cast<uint64_t>(exact_val) << bit_shift);
        if (page->is_hot[arr_idx].compare_exchange_weak(old_arr, new_arr, std::memory_order_relaxed)) {
            break;
        }
    }
}

uint16_t get_page_hot_count(Page* page) {
    uint16_t total_hot = 0;
    // no lock. 'false' value is acceptable
    for (int i = 0; i < IS_HOT_ARR_LENGTH; i++) {
        uint64_t chunk = page->is_hot[i].load(std::memory_order_relaxed);
        for(size_t bit_shift=0; bit_shift<64; bit_shift+=2){
            total_hot += ((chunk>>bit_shift) & 0b11ULL);
        }
    }
    return total_hot;
}

uint16_t age_and_get_page_hot_count(Page* page, uint8_t age_speed){
    uint16_t total_hot = 0;
    
    for(int i=0; i < IS_HOT_ARR_LENGTH; i++){
        uint64_t old_arr = page->is_hot[i].load(std::memory_order_relaxed);
        
        while(true){
            uint64_t new_arr = 0ULL;
            uint16_t chunk_hot = 0; // Temp counter for this specific CAS attempt
            
            for(size_t bit_shift=0; bit_shift<64; bit_shift+=2){
                uint8_t val = (old_arr>>bit_shift) & 0b11ULL;
                chunk_hot += val; // Count the hotness
                
                for(int j=0; j<age_speed; j++){
                    if(val > 0) val--; // Age the record
                }
                new_arr |= (static_cast<uint64_t>(val) << bit_shift);
            }
            
            if(page->is_hot[i].compare_exchange_weak(old_arr, new_arr, std::memory_order_relaxed)){
                total_hot += chunk_hot; 
                break;
            }
        }
    }
    return total_hot;
}

void clear_page_hot_bits(Page* page) {
    for (int i = 0; i < IS_HOT_ARR_LENGTH; i++) {
        page->is_hot[i].store(0ULL, std::memory_order_relaxed);
    }
}

Page* get_struct_page(void* slot_addr){
    uintptr_t slot_addr_ = reinterpret_cast<uintptr_t>(slot_addr);
    uintptr_t page_base = PAGE_ALIGN(slot_addr_);

    return reinterpret_cast<Page*>(page_base);
}

/* Mark a slot as permanently reserved. Called immediately during SCM alloc() */
void set_allocated_bit(Page* page, uint16_t slot_idx){
    size_t arr_idx = slot_idx / 64;
    size_t bit_idx = slot_idx % 64;
    
    // Use release semantics so the bit is set BEFORE the thread starts writing data
    page->is_allocated[arr_idx].fetch_or(1ULL << bit_idx, std::memory_order_release);
}

/* Clear the reservation. Called during SCM free() */
void clear_allocated_bit(Page* page, uint16_t slot_idx){
    size_t arr_idx = slot_idx / 64;
    size_t bit_idx = slot_idx % 64;
    
    page->is_allocated[arr_idx].fetch_and(~(1ULL << bit_idx), std::memory_order_release);
}

/* Check if a slot is currently held by an active thread. Called by Sweeper */
bool get_allocated_bit(Page* page, uint16_t slot_idx){
    size_t arr_idx = slot_idx / 64;
    size_t bit_idx = slot_idx % 64;
    
    // Use acquire semantics to guarantee we see the most up-to-date bit state
    return (page->is_allocated[arr_idx].load(std::memory_order_acquire) & (1ULL << bit_idx)) != 0;
}
}// namespace imdb