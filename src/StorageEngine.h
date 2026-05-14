/* StorageEngine.h */
#pragma once

#include <vector>
#include <unordered_map>
#include <string>
#include <cstring>
#include <shared_mutex>
#include "SizeClass.h"
#include "DiskManager.h"
#include "Sweeper.h"
#include "ShardedHashmap.h"

namespace imdb{
#define TABLE_END  0XFFFFFFFFFFFFFFFF

// TODO: will the FIFO feature of TT makes this inefficient?
#define TT_SHARDS 1024

/* strcut RecordLoc(record location). Content in the translation table. 
 *
 * Record lookup is like: 
 * 1. hashmap[key] --> translation table idx.
 * 2. translation_table[idx] --> loc in ram / loc in disk
 * 
 * When unused(caused by record delete), stores next free idx
 */
struct RecordLoc{
    union{
        struct{
            union{
                void* ram_addr;
                size_t disk_offset;
            }; // 8 bytes
            uint32_t size; // 4 bytes
            bool is_in_ram; // 1 byte
            // 3 bytes compiler padding
            // total 16 bytes **try to keep it small!**
        }in_use;

        uint64_t next_free_idx;
    };
}; 

/* record header. logical id must be stored here to complete reverse map in evcition.
 * TODO: might consider putting 'record size' here, too! For DB persistence
 */
#pragma pack(push, 1)
struct RecordHeader{
    uint64_t logical_id;

    char* get_payload() {
        return reinterpret_cast<char*>(this + 1);
    }
};
#pragma pack(pop)

/* config object*/
struct DBConfig{
    std::string db_file_path = "./data/imdb.aof";
    // uint8_t table_grow_speed = 2;
    size_t arena_size = 1024 * 1024 * 512; // default 512MB
    bool enable_hot_rescue = 1;
    uint8_t age_record_speed = 1;
    uint8_t page_hot_scale = 1;
};

/* StorageEngine, the highest level interface, provides put, get and delete. 
 * TODO: it owns sweeper thread. Might wish to move it else where.
 */
class StorageEngine{
    private:
        DBConfig config;
        Arena arena;
        DiskManager disk_manager;
        /* Recordheader is 8 bytes. Record >= 512B is managed by LargeRecordManager.
         * SCMs ranging from 16 bytes to 256 bytes.
         * 16, 32, 64, 128, 256
         */
        SizeClassManager SCMs[5];
        EvictionSweeper sweeper;

        /* logical id generator, also serves as first_free_idx in translation table */
        uint64_t next_logical_id;
        /* translation table, bridge hashmap and record location. See comment at struct RecordLoc */
        std::vector<RecordLoc> translation_table;
        /* locks that protects TT's content. */
        // TODO: proformance degrade as tt grows large?
        std::shared_mutex tt_locks[TT_SHARDS];
        /* lock that protects the internal free list in TT. Only used in entry allocation/free */
        std::shared_mutex tt_meta_rw_lock;
       
        /* tracks key -> translation_table index mappings */
        ShardedHashMap hashmap;

        uint8_t page_hot_scale;
        uint8_t age_record_speed;

        void evict_cold_page();
        /* rescue hot record, and write other records to disk.*/
        void page_hot_rescue(Page* victim_page);

        /* --------------- helper functions -------------------------- */

        /* get the idx of corresponding SCM by size */
        uint8_t get_scm_index(size_t total_size);
        /* given an allocated slot, initialize it with the given logical id and payload 
         * don't check whether the size fits, nor whether the slot is allocated.     */
        void fill_slot_nocheck(void* slot_addr, uint64_t logical_id, const char* record, uint64_t record_size);
        /* use to check whether the key->logical id mapping obtained from last query is still valid */
        bool hashmap_recheck(const std::string &key, uint64_t expected_id);

        /* Update a record that already exists in the Translation Table */
        bool update_record(const std::string &key, uint64_t logical_id, const char* record, uint64_t record_size);
        /* allocate and insert a new record */
        bool insert_record(const std::string& key, const char* record, uint64_t record_size, uint64_t &collided_id);


        /* -------- translation table operations -------- */

        uint64_t add_to_table(RecordLoc& new_loc); // on success, return the inserted data's logical id
        void remove_from_table(uint64_t logical_id);
        /* grow the table by factor config.table_grow_speed */
        void grow_table();

    public:
        // debug:
        std::atomic<uint64_t> hot_rescued_count{0};
        std::atomic<uint64_t> check_page_count{0};
        std::atomic<uint64_t> whole_page_evicted_count{0};
        std::atomic<uint64_t> partial_page_evicted_count{0};
        bool is_arena_critical() const { return arena.is_critical(); }

        StorageEngine(const DBConfig &cfg = DBConfig());

        ~StorageEngine() = default;

        /* Insert a new record or update a existing one. Return true on success */
        bool put(const std::string& key, const char* record, uint64_t record_size);
        /* Read a record from DB using key. Return true on success */
        bool get(const std::string& key, char* buf, uint64_t& record_size);
        /* Delete a record from DB using key. Return true on success */
        bool del(const std::string& key);
};

} // namespace imdb