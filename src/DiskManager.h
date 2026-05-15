#pragma once
#include <string>
#include <atomic>
#include <mutex>

namespace imdb {
/* Currently, disk IO is perfromed 'append only'. DiskManager bindly puts records after 'file_end'. 
 * It never cares about the fragmentation caused by deleting a record or reading a record back to ram.
 *
 * Working this way, a 'compaction' routine is needed to defragement such fragmentation. 
 * // TODO: compaction
 * Currently, we just ignore those waste.
 */

class DiskManager {
private:
    int db_fd;
    std::atomic<size_t> file_end; // Tracks the current end of the file for appends

    std::mutex write_lock; 

public:
    DiskManager(const std::string& db_file_path);
    ~DiskManager();

    /* Appends a record to the end of the disk file. Returns the byte offset. 
     * TODO: If ever what to reduce syscalls to pwrite, a user-space buffer should be added.  
     */
    size_t write_record(const char* data, size_t size);

    /* Reads a record by byte offset. */
    bool read_record(size_t offset, char* buffer, size_t size);
    
    /* Ensures the OS Page Cache physically flushes to the SSD chips */
    void sync(); 
};

} // namespace imdb