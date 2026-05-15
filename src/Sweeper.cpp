#include "Sweeper.h"
#include <iostream>

namespace imdb {

EvictionSweeper::EvictionSweeper(Arena& a, std::function<void()> evict_func)
    : arena(a), evict_callback(std::move(evict_func)) 
{
    arena.is_sweeper_running.store(true);
    
    // Spawn the thread, pointing it to our internal loop
    sweeper_thread = std::thread(&EvictionSweeper::evict_loop, this);
}

/* This guarantees that when StorageEngine is destroyed, the background thread
    doesn't become a zombie or crash trying to access freed memory. */
EvictionSweeper::~EvictionSweeper(){
    // Raise the shutdown flag
    shutdown_flag.store(true);
    arena.is_sweeper_running.store(false);

    // The thread might be asleep waiting for the Arena. 
    // We MUST trigger the condition variable to wake it up so it sees the shutdown flag.
    arena.sweeper_cv.notify_all();

    // Block the main thread until the sweeper finishes its current loop
    if (sweeper_thread.joinable()) {
        sweeper_thread.join();
    }
}

// 3. The Infinite Background Loop
void EvictionSweeper::evict_loop(){
    while (!shutdown_flag.load()){
        
        // Lock the mutex and go to sleep. Wait for arena to wake it up
        std::unique_lock<std::mutex> cv_lock(arena.sweeper_mutex);
        arena.sweeper_cv.wait(cv_lock, [this]() {
            return arena.needs_sweeping() || shutdown_flag.load();
        });

        // If we woke up because the destructor was called, exit the loop immediately.
        if (shutdown_flag.load()) {
            break; 
        }

        // Evict pages until we drop back down to the High Watermark.
        while (!arena.is_safe() && !shutdown_flag.load()) {
            
            // Execute the callback! 
            if (evict_callback) {
                evict_callback();
            } else {
                std::cerr << "Fatal: Eviction callback is null!\n";
                break;
            }
        }
    }
}

} // namespace imdb