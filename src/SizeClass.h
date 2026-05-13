/* SizeClass.h 
 * A size class is a group of pages storing records of same power-of-2 size
 * for example, size class 64 byte will store and only store record of (32 byte, 64 byte]
 * 
 * Size class is used for smaller sized records(<512 byte, a quater of PAGE_SIZE)
 * for larger sized record, use LargeObjectManager, which need to use off-page metadata, 
 * and different allocation strategy.
 * 
 * TODO: implement LargeObjectManager
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <shared_mutex>
#include "Arena.h"

#define SLOT_END 0XFFFF

namespace imdb{
enum Status{
    FULL_PAGE_EVICTED,
    PARTIAL_PAGE_EVICTED,
    ERROR
};

/* Manager of one kind of size class, allocates and frees slots 
 */
class SizeClassManager{
    private:
        uint16_t slot_size;
        /* list head of partially full pages
         * A SizeClassManager only needs to track partially full pages
         * a free page should be returned to arena
         * a full page can not longer be allocated from thus not tracked
         */
        Page *partial_list_head;

        Arena& arena;

        std::shared_mutex rw_lock;

        void push_to_partial_list(Page *page);
        void remove_from_partial_list(Page *page);

        /* ask a page from arena. Might sleep */
        Page* get_a_page();
        /* remove from lru and return the page */
        void return_a_page(Page* page);

        
        /* Input should be a partial page. Returns the first free slot's idx. 
         * Kill the program when handed a full page */
        uint16_t get_free_slot(Page* page);

        /* Helper function. init a page with the correct format, setup interal free slots list */
        Page* init_page(void* raw_page_base);

    public:
        SizeClassManager(uint16_t slot_size, Arena& arena): slot_size(slot_size), arena(arena) {partial_list_head = nullptr;}
        ~SizeClassManager() = default;
        
        uint16_t get_slot_size() const { return slot_size; }

        /* alloate a slot, if page becomes full, stop tracking the page
         * will ask arena for new page if needed, might sleep. Not supposed to fail
         */
        void* alloc();
        /* free a slot, if page become free, return it to the arena*/
        void free(void* raw_addr);
        /* allocate a slot if SCM have free space, never ask new page. Might fail */
        void* alloc_notrigger();
        /* used by sweeper thread to isolate page */
        bool quarantine_page(Page* page);
        /* used by sweeper to unisolate page */
        Status unquarantine_page(Page* page);
};

/* helper function. Used for is_hot array */
// increase is_hot to make a slot have lower chance to be swapped out
void promote_a_slot(void* slot_addr, uint8_t inc = 1);
// decrease is_hot to make a slot have higher chance to be swapped out
void age_a_slot(void* slot_addr);
// mark is_hot as zero. 
void mark_slot_cold(void* slot_addr);
uint8_t get_slot_hotness(void* slot_addr);
void set_slot_hotness(void* slot_addr, uint8_t exact_val);
// return the total is_hot count
uint16_t get_page_hot_count(Page* page);
// also do '-1' on is_hot
uint16_t age_and_get_page_hot_count(Page* page);
// clear is_hot
void clear_page_hot_bits(Page* page);

/* helper function. Input: addr of a slot. Output: struct page the slot belonging to */
Page* get_struct_page(void* slot_addr);

/* helper function. Used for is_allocated bit map */
/* set the is_allocated bit */
void set_allocated_bit(Page* page, uint16_t slot_idx);
/* clear is_allcated bit */
void clear_allocated_bit(Page* page, uint16_t slot_idx);
bool get_allocated_bit(Page* page, uint16_t slot_idx);
}// namespace imdb