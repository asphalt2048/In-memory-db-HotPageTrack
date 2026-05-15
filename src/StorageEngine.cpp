#include "StorageEngine.h"

/*------------------------ctor/dtor-----------------------------*/

/* TODO: DB now boots with translation table in a static size. */

#define TABLE_SIZE 100000000

namespace imdb{
StorageEngine::StorageEngine(const DBConfig &cfg):
    config(cfg),
    arena(cfg.arena_size, config.backpressure_sleep_us),
    disk_manager(config.db_file_path),
    SCMs{{16, arena}, {32, arena}, {64, arena}, {128, arena}, {256, arena}},
    sweeper(arena, [this](){this->evict_cold_page();}),
    next_logical_id(0)
{
    translation_table.resize(TABLE_SIZE);
    for(int i = 0; i<TABLE_SIZE-1; i++){ translation_table[i].next_free_idx = i + 1; }
    translation_table[TABLE_SIZE-1].next_free_idx = TABLE_END;
}

/*---------------------------------Translationn table related----------------------------------*/

uint64_t StorageEngine::add_to_table(RecordLoc& new_loc){
    std::unique_lock<std::shared_mutex> write_lock(tt_meta_rw_lock);

    if(next_logical_id == TABLE_END){
        std::cerr<<RED<<"Fatal: TT exhausted\n"<<RESET;
        exit(-1);
    }

    uint64_t logical_id = next_logical_id;
    next_logical_id = translation_table[next_logical_id].next_free_idx;
    translation_table[logical_id] = new_loc;

    return logical_id;
}

void StorageEngine::remove_from_table(uint64_t logical_id){
    std::unique_lock<std::shared_mutex> write_lock(tt_meta_rw_lock);

    translation_table[logical_id].next_free_idx = next_logical_id;

    // don't remove this line, unless the partial in-use slot problem
    // (noted in page_hot_rescue), is solved perfectly.
    translation_table[logical_id].in_use.is_in_ram = false;

    next_logical_id = logical_id;
}

// .resize() is actually copy&paste, which requires a TT global lock to perform.
/*
void StorageEngine::grow_table(){
    uint64_t old_size = translation_table.size();
    uint64_t new_size = old_size*config.table_grow_speed;
    translation_table.resize(new_size);

    for(uint64_t i=old_size; i<new_size-1; i++){ 
        translation_table[i].next_free_idx = i+1;
    }
    translation_table[new_size-1].next_free_idx = TABLE_END;

    next_logical_id = old_size;
}
*/

/*------------------------------------------------Helper Functions------------------------------------------------------*/

uint8_t StorageEngine::get_scm_index(uint64_t total_size) {
    if (total_size <= 16) return 0;
    if (total_size <= 32) return 1;
    if (total_size <= 64) return 2;
    if (total_size <= 128) return 3;
    if (total_size <= 256) return 4;
    
    std::cerr<<RED<<"Out of Bound\n"<<RESET;
    exit(-1);
}

/* given an allocated slot, initialize it with the given logical id and payload 
 * don't check whether the size fits, nor whether the slot is allocated.     */
void StorageEngine::fill_slot_nocheck(void* slot_addr, uint64_t logical_id, const char* record, uint64_t record_size){
    RecordHeader* header = reinterpret_cast<RecordHeader*>(slot_addr);
    header->logical_id = logical_id;
    char* payload = header->get_payload();
    std::memcpy(payload, record, record_size);
}

bool StorageEngine::hashmap_recheck(const std::string &key, uint64_t expected_id){
    uint64_t check_id;
    if(!hashmap.get(key, check_id) || check_id != expected_id){
        return false;
    }
    return true;
}

/*------------------------------------------------------DB interface-----------------------------------------------------*/

/* WARNING: DB interface must pre alloc space before holding TT's lock. Otherwise there will be deadlock when OOM happens. */

bool StorageEngine::update_record(const std::string &key, uint64_t logical_id, const char* record, uint64_t record_size){
    size_t real_size = record_size + sizeof(RecordHeader);

    SizeClassManager &scm = SCMs[get_scm_index(real_size)];
    void* pre_alloc_slot = scm.alloc();

    size_t lock_idx = logical_id % TT_SHARDS;
    std::unique_lock<std::shared_mutex> write_lock(tt_locks[lock_idx]);

    if(!hashmap_recheck(key, logical_id)){
        scm.free(pre_alloc_slot);
        return false;
    }

    RecordLoc& loc = translation_table[logical_id];

    /* case 1: record is in ram */
    if(loc.in_use.is_in_ram){
        if(config.enable_monitor){
            ram_hit_count.fetch_add(1, std::memory_order_relaxed);
        }
        
        void* old_slot_addr = loc.in_use.ram_addr;
        size_t old_size = get_struct_page(old_slot_addr)->header.slot_size;
        SizeClassManager &old_scm = SCMs[get_scm_index(old_size)];

        /* case 1.1: New data fits in the existing slot. Update in-place. */
        if(scm.get_slot_size() == old_scm.get_slot_size()){
            scm.free(pre_alloc_slot);

            fill_slot_nocheck(old_slot_addr, logical_id, record, record_size);
            promote_a_slot(old_slot_addr);
            loc.in_use.size = record_size;
            return true;
        }
        /* case 1.2: New data outgrows/downgrow the current slot. Reallocate. */
        else{
            // copy data to new slot.
            fill_slot_nocheck(pre_alloc_slot, logical_id, record, record_size);
            promote_a_slot(pre_alloc_slot);

            // mark old slot cold before free.
            mark_slot_cold(old_slot_addr);
            old_scm.free(old_slot_addr);            

            loc.in_use.ram_addr = pre_alloc_slot;
            loc.in_use.size = record_size;
            return true;
        }
    }
    /* case 2: record is in disk */
    else{
        if(config.enable_monitor){
            ram_miss_count.fetch_add(1, std::memory_order_relaxed); 
        }

        fill_slot_nocheck(pre_alloc_slot, logical_id, record, record_size);
        promote_a_slot(pre_alloc_slot);

        loc.in_use.is_in_ram = true;
        loc.in_use.ram_addr = pre_alloc_slot;
        loc.in_use.size = record_size;
        return true;
    }
}


bool StorageEngine::insert_record(const std::string& key, const char* record, uint64_t record_size, uint64_t &collided_id) {
    SizeClassManager &scm = SCMs[get_scm_index(record_size + sizeof(RecordHeader))];
    void* new_slot_addr = scm.alloc();

    RecordLoc new_loc;

    promote_a_slot(new_slot_addr);
    uint64_t new_logical_id = add_to_table(new_loc);
    fill_slot_nocheck(new_slot_addr, new_logical_id, record, record_size);
    
    
    size_t lock_idx = new_logical_id % TT_SHARDS;
    std::unique_lock<std::shared_mutex> write_lock(tt_locks[lock_idx]);

    /* Beware of insert race:
     * Two insert with same key fire at the same time. The loser in the race must roll back.
     */
    bool success = hashmap.insert(key, new_logical_id, collided_id);
    if(!success){ // roll back
        mark_slot_cold(new_slot_addr);
        scm.free(new_slot_addr);
        remove_from_table(new_logical_id);
        return false;
    }

    /* must only update TT at the end. As this action will make record visible to sweeper */
    translation_table[new_logical_id].in_use.is_in_ram = true;
    translation_table[new_logical_id].in_use.size = record_size;
    translation_table[new_logical_id].in_use.ram_addr = new_slot_addr;

    return true;
}

bool StorageEngine::put(const std::string& key, const char* record, uint64_t record_size){
    size_t real_size = record_size + sizeof(RecordHeader);
    if(real_size > 256){
        std::cerr<<RED<<"Out of bound\n"<<RESET;
        return false;
    }

    uint64_t logical_id;
    

    /* the UPSERT semantic requires a put to be executed no matter what */
    while(true){
        bool exist = hashmap.get(key, logical_id);

        if (exist){
            // it's an update
            /* beware of TOCTOU: if a del got ahead, an update will become an insert 
             * update should double check whether the record exists. 
             */
            if(update_record(key, logical_id, record, record_size)){
                return true;
            }
        } else {
            // it's an insert
            /* beware of insert race: if two inserts with same key fires simultaneously,
             * the loser of the race should become an update.
             */
            uint64_t collided_id;
            if(insert_record(key, record, record_size, collided_id)){
                return true;
            }
        }
    }
}

bool StorageEngine::get(const std::string& key, char* buf, uint64_t& out_record_size){
    uint64_t logical_id;
    bool exist = hashmap.get(key, logical_id);
    if(!exist) {
        std::cerr<<RED<<"Get record with key: "<<key<<" error: record not exists\n"<<RESET; 
        return false;
    }

    size_t lock_idx = logical_id % TT_SHARDS;
    
    // 1. FAST PATH: Start with a READ lock
    std::shared_lock<std::shared_mutex> read_lock(tt_locks[lock_idx]);

    if(!hashmap_recheck(key, logical_id)){
        return false;
    }

    RecordLoc& loc = translation_table[logical_id];

    if(loc.in_use.is_in_ram){
        RecordHeader* header = reinterpret_cast<RecordHeader*>(loc.in_use.ram_addr);
        char* payload = header->get_payload();

        std::memcpy(buf, payload, loc.in_use.size);
        out_record_size = loc.in_use.size;
        promote_a_slot(loc.in_use.ram_addr);

        if(config.enable_monitor){
            ram_hit_count.fetch_add(1, std::memory_order_relaxed);
        }
        return true;
    }

    // 2. SLOW PATH (Disk -> RAM)
    uint64_t record_size = loc.in_use.size;
    
    /* Drop the read lock to avoid deadlocks while asking the Arena for memory
     * WARNING: must double check any states obtained before this line if the state will be used below
     */
    read_lock.unlock();

    SizeClassManager &scm = SCMs[get_scm_index(record_size + sizeof(RecordHeader))];
    void* pre_alloc_slot = scm.alloc();

    // making changes to TT, upgrade lock to writelock
    std::unique_lock<std::shared_mutex> write_lock(tt_locks[lock_idx]);

    if(!hashmap_recheck(key, logical_id)){ scm.free(pre_alloc_slot); return false; }
    
    if(loc.in_use.is_in_ram){
        // Someone beat us to it! Just read from RAM and give our unused slot back.
        // Could be a concurrent get or update
        scm.free(pre_alloc_slot); 

        RecordHeader* header = reinterpret_cast<RecordHeader*>(loc.in_use.ram_addr);
        std::memcpy(buf, header->get_payload(), loc.in_use.size);
        out_record_size = loc.in_use.size;
        promote_a_slot(loc.in_use.ram_addr);

        if(config.enable_monitor){
            ram_hit_count.fetch_add(1, std::memory_order_relaxed);
        }
        return true; 
    } 

    // recheck record size here. an update + evict sequence could make old size invalid
    if (get_scm_index(record_size + sizeof(RecordHeader)) != get_scm_index(loc.in_use.size + sizeof(RecordHeader))){
        scm.free(pre_alloc_slot);
        write_lock.unlock();
        return get(key, buf, out_record_size);
    }

    // read from disk and promote. 
    size_t payload_offset = loc.in_use.disk_offset + sizeof(RecordHeader);
    bool success = disk_manager.read_record(payload_offset, buf, loc.in_use.size);
    if(!success){ 
        scm.free(pre_alloc_slot); 
        return false; 
    }
    out_record_size = loc.in_use.size;

    fill_slot_nocheck(pre_alloc_slot, logical_id, buf, loc.in_use.size);
    promote_a_slot(pre_alloc_slot);

    loc.in_use.is_in_ram = true;
    loc.in_use.ram_addr = pre_alloc_slot;

    if(config.enable_monitor){
        ram_miss_count.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

bool StorageEngine::del(const std::string& key){
    uint64_t logical_id;
    bool exist = hashmap.get(key, logical_id);
    if(!exist) {std::cerr<<RED<<"Delete record with key: "<<key<<" error: record not exists\n"<<RESET; return false;}

    size_t lock_idx = logical_id % TT_SHARDS;
    std::unique_lock<std::shared_mutex> write_lock(tt_locks[lock_idx]);

    if(!hashmap_recheck(key, logical_id)){
        // the mapping in hashmap has been changed, means a concurrent delete got ahead
        return false;
    }

    RecordLoc &loc = translation_table[logical_id];

    /* record is in ram, delete directly */
    if (loc.in_use.is_in_ram) {
        void* slot_addr = loc.in_use.ram_addr;
        Page* page = get_struct_page(slot_addr);
        size_t slot_size = page->header.slot_size;

        mark_slot_cold(slot_addr);
        SCMs[get_scm_index(slot_size)].free(slot_addr);

        if(config.enable_monitor){
            ram_hit_count.fetch_add(1, std::memory_order_relaxed);
        }
    } 
    /* record is in disk, logical delete */
    else{
        if(config.enable_monitor){
            ram_miss_count.fetch_add(1, std::memory_order_relaxed);
        }
        // Logical deletion for on-disk records. We just delete metadata.
        // that way from the user's view, the record is deleted as well.
    }

    remove_from_table(logical_id);
    hashmap.del(key);

    return true;
}

/*----------------------------------------------------Eviction logic--------------------------------------------------*/

void StorageEngine::page_hot_rescue(Page* victim_page){
    SizeClassManager &scm = SCMs[get_scm_index(victim_page->header.slot_size)];
    // flag, determine how aggresive sweeper acts
    bool is_critical = arena.is_critical();

    uint16_t max_slots = victim_page->header.max_slots;

    for (uint16_t i = 0; i < max_slots; i++) {
        /* Partial In-Use Slot problem: 
         * An insert might have allocated a slot, but have not written logical id. Same is the pre
         * allocated slot in get/update. I call this a "partial in-use slot". 
         * For slot with is_allocated bit = 1, must double check to know whether it's
         * partial in-use or not
         */

        char* slot_addr = victim_page->get_slot_addr(i);
        uint16_t slot_idx = victim_page->get_slot_idx_nocheck(slot_addr);

        /* skip the slot if it's definitely free. 
         * WARNING: slot passed this check is not guaranteed to be in-use throught out the loop
         */
        if(!get_allocated_bit(victim_page, slot_idx)){ continue; }

        RecordHeader* header = reinterpret_cast<RecordHeader*>(slot_addr);
        uint64_t logical_id = header->logical_id;

        if (logical_id < translation_table.size()){

            size_t lock_idx = logical_id % TT_SHARDS;
            std::unique_lock<std::shared_mutex> write_lock(tt_locks[lock_idx], std::try_to_lock);

            /* (1)is_critical: sweeper will wait to grab every lock, making sure 
             * a full page is evcited.
             * (2)!is_critical: sweeper will let this slot go, ends up freeing only a part
             * of a page
             */
            if(!write_lock.owns_lock()){
                if(!is_critical){
                    continue;
                }else{
                    write_lock.lock();
                }
            } 

            RecordLoc& loc = translation_table[logical_id];
            
            /* A unprincipled solution to the "partial in-use slot" mentioned above. partial in-use 
             * slot will have a garbage logical id. Which can't pass the following check
             */
            if (loc.in_use.is_in_ram && loc.in_use.ram_addr == slot_addr){
                // It's a fully in-use slot!

                size_t total_size = loc.in_use.size + sizeof(RecordHeader);
                uint8_t hotness = get_slot_hotness(slot_addr);
                // --- BEST EFFORT RESCUE ---
                if (config.enable_hot_rescue && hotness >= 3 - config.age_record_speed){
                    /* TRY to ask free space in SCM. Might fail. 
                     * If fail, let the record die(write to disk).
                     *
                     * WARNING: allocation in this section must not sleep!
                     */
                    void* new_slot_addr = scm.alloc_notrigger();
                    
                    if (new_slot_addr != nullptr) {
                        std::memcpy(new_slot_addr, slot_addr, total_size);
                        loc.in_use.ram_addr = new_slot_addr;
                        set_slot_hotness(new_slot_addr, hotness);

                        clear_allocated_bit(victim_page, slot_idx);
                        mark_slot_cold(slot_addr);
                        victim_page->header.used.fetch_sub(1);

                        if(config.enable_monitor){
                            hot_rescued_count.fetch_add(1, std::memory_order_relaxed);
                        }

                        continue;
                    }
                }

                // --- COLD EVICTION (OR RESCUE FAILED) ---
                size_t disk_offset = disk_manager.write_record(slot_addr, total_size);
                loc.in_use.is_in_ram = false;
                loc.in_use.disk_offset = disk_offset;

                clear_allocated_bit(victim_page, slot_idx);
                mark_slot_cold(slot_addr);
                victim_page->header.used.fetch_sub(1);
            }else{
                continue; // slot is partial in-use, let it go.
            }
        }else{
            continue; // logical id is out of bound, meaning it's a partial in-use slot. Let it go.
        }
    }
}

void StorageEngine::evict_cold_page() {
    while (true) {
        Page* victim_page = reinterpret_cast<Page*>(arena.get_lru_tail());
        if (!victim_page) return; 
        if(config.enable_monitor){
            check_page_count.fetch_add(1, std::memory_order_relaxed);
        }

        /* TODO: There is a TOCTOU bug, that the victim page become empty and returned to arena before 
         * sweeper is able to evict it. 
         * The page might has been allocated to another SCM before we are able to quarantine it.
         * The current fix is doing a double check inside scm.quarantine_page(), which is not elegant
         * SUGGESTION: add a generation count in pageheader, that is increased each time page get allocated
         */

        SizeClassManager &scm = SCMs[get_scm_index(victim_page->header.slot_size)];
        // isolate the page so that no insert will use this page. And no free can change
        // the page's lru pointers and partial list pointers.
        bool success = scm.quarantine_page(victim_page);
        if(!success){
            continue;
        }
        uint16_t max_slots = victim_page->header.max_slots;
        

        /* =======================================================================
         * Clock algorithm
         * If a page have (hot records > max_slot/PAGE_HOT_SCALE), give it a second chance.
         * the page is lifted to the head of lru. And all hot bits are cleared
          ======================================================================= */

        uint16_t hot_count = age_and_get_page_hot_count(victim_page, config.age_record_speed);
        if (hot_count > (max_slots * config.page_hot_scale)){
            scm.unquarantine_page(victim_page);
            continue; 
        }
        page_hot_rescue(victim_page); // page selected as evcition target

        /* decide whether the victim page should be linked back to partial list, or return to arena 
         * (1) If page is not fully cleared, e.g. try_lock failed, or meet a partial in use page that eventually in-use
         *      link the page back to partial list
         * (2) If the page is fully cleared, return it to arena
         */
        Status status = scm.unquarantine_page(victim_page);
        if(status == ERROR){
            std::cerr<<RED<<"Sweeper error: unquarantine page met undefined status\n"<<RESET;
            exit(-1);
        }

        if(config.enable_monitor){
            switch (status)
            {
            case FULL_PAGE_EVICTED:
                whole_page_evicted_count.fetch_add(1, std::memory_order_relaxed);
                break;
            case PARTIAL_PAGE_EVICTED:
                partial_page_evicted_count.fetch_add(1, std::memory_order_relaxed);
                break;
            default:
                break;
            }
        }
        
        if(arena.is_safe()){
            break;
        }
    }
}

}//namespace imdb