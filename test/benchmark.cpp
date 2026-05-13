#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <random>
#include <cmath>
#include <atomic>
#include <algorithm>
#include <numeric>
#include <iomanip>
#include <cstring>
#include <filesystem>

#include "StorageEngine.h" 

using namespace imdb;

// ============================================================================
// 1. HIGH RESOLUTION TIMER
// ============================================================================
struct Timer {
    std::chrono::time_point<std::chrono::high_resolution_clock> start_time;
    
    void start() {
        start_time = std::chrono::high_resolution_clock::now();
    }
    
    double elapsed_us() { 
        auto end_time = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
    }
    
    double elapsed_ms() { 
        return elapsed_us() / 1000.0;
    }
};

// ============================================================================
// 2. FAST ZIPFIAN GENERATOR
// ============================================================================
class FastZipfian {
private:
    uint64_t n;
    double alpha, zetan, eta;
    std::mt19937_64 rng;
    std::uniform_real_distribution<double> dist;

    double zeta(uint64_t n, double theta) {
        double sum = 0;
        for (uint64_t i = 1; i <= n; i++) sum += 1.0 / std::pow(i, theta);
        return sum;
    }

public:
    FastZipfian(uint64_t n, double theta = 0.99, uint64_t seed = 42) 
        : n(n), alpha(1.0 / (1.0 - theta)), rng(seed), dist(0.0, 1.0) {
        zetan = zeta(n, theta);
        eta = (1.0 - std::pow(2.0 / n, 1.0 - theta)) / (1.0 - zeta(2, theta) / zetan);
    }

    uint64_t next() {
        double u = dist(rng);
        double uz = u * zetan;
        if (uz < 1.0) return 1;
        if (uz < 1.0 + std::pow(0.5, 1.0 / alpha)) return 2;
        return 1 + (uint64_t)(n * std::pow(eta * u - eta + 1.0, alpha));
    }
};

// ============================================================================
// 3. BENCHMARK CONFIGURATION
// ============================================================================
struct BenchConfig {
    uint64_t num_records = 1000000;    
    int num_threads = 8;               
    int warmup_seconds = 5;            
    int measure_seconds = 60;          
    int read_percent = 95;             
    double zipf_theta = 0.99;          
};

std::atomic<bool> keep_running{true};

struct ThreadMetrics {
    uint64_t total_ops = 0;
    uint64_t successful_reads = 0;
    uint64_t successful_updates = 0;
    uint64_t corruptions = 0;          
    double total_latency_us = 0;
    double max_latency_us = 0;
};

enum class BenchState { WARMUP, MEASURE, DONE };
std::atomic<BenchState> global_bench_state{BenchState::WARMUP};

// ============================================================================
// 4. DYNAMIC SELF-VALIDATING PAYLOAD HELPERS
// ============================================================================

void generate_payload(uint64_t key_id, bool is_update, char* buffer, size_t target_size) {
    std::string prefix = "K" + std::to_string(key_id) + (is_update ? "U" : "I");
    
    if (target_size < prefix.length() + 1) {
        std::memcpy(buffer, prefix.c_str(), std::min(prefix.length(), target_size));
        return;
    }

    char padding_char = static_cast<char>((key_id % 10) + '0');
    std::memset(buffer, padding_char, target_size);
    std::memcpy(buffer, prefix.c_str(), prefix.length());
    buffer[target_size - 1] = '#'; 
}

bool verify_payload(uint64_t expected_id, const char* buffer, size_t actual_size) {
    // 1. Check Bounds
    if (actual_size < 16 || actual_size > 256) return false; 
    
    // 2. Check Boundary Marker
    if (buffer[actual_size - 1] != '#') return false; 

    // 3. Check Identity Prefix
    std::string expected_prefix = "K" + std::to_string(expected_id);
    if (std::memcmp(buffer, expected_prefix.c_str(), expected_prefix.length()) != 0) {
        return false;
    }

    // 4. Strict Padding Scan (Catches overlapping writes from other records)
    char expected_pad = static_cast<char>((expected_id % 10) + '0');
    size_t start_pad_idx = expected_prefix.length() + 1; // skip 'U' or 'I'
    for (size_t i = start_pad_idx; i < actual_size - 1; i++) {
        if (buffer[i] != expected_pad) return false;
    }

    return true;
}

// ============================================================================
// 5. THE LOAD PHASE (DYNAMIC SIZES)
// ============================================================================
void load_database(StorageEngine& db, BenchConfig& config) {
    std::cout << "[Load Phase] Sequentially inserting " << config.num_records << " records with random sizes...\n";
    Timer timer;
    timer.start();

    char payload_buffer[256];
    uint64_t dropped_records = 0;
    
    // RNG for dynamic load sizes
    std::mt19937 rng(999);
    std::uniform_int_distribution<size_t> size_dist(20, 248); 

    for (uint64_t i = 1; i <= config.num_records; i++) {
        std::string key = "user:" + std::to_string(i);
        size_t dynamic_size = size_dist(rng);
        
        generate_payload(i, false, payload_buffer, dynamic_size);
        
        bool success = db.put(key, payload_buffer, dynamic_size);
        if(!success) dropped_records++;
        
        if (i % (config.num_records / 10) == 0) {
            std::cout << "  ... " << (i * 100) / config.num_records << "% loaded.\n";
        }
    }
    if (dropped_records > 0) {
        std::cout << "[WARNING] Arena OOM during load! Dropped " << dropped_records << " records.\n";
    }
    std::cout << "[Load Phase] Complete. Took " << timer.elapsed_ms() << " ms.\n";
}

// ============================================================================
// 6. THE WORKER THREAD (DYNAMIC SIZES)
// ============================================================================
void benchmark_worker(int thread_id, StorageEngine* db, BenchConfig config, ThreadMetrics* metrics) {
    FastZipfian zipf(config.num_records, config.zipf_theta, 42 + thread_id);
    std::mt19937 rng(1337 + thread_id);
    std::uniform_int_distribution<int> ratio_dist(1, 100);
    
    // Worker randomly resizes records on update!
    std::uniform_int_distribution<size_t> size_dist(20, 248); 

    char update_payload[256];
    char read_buffer[256]; 

    Timer op_timer;

    while (global_bench_state.load(std::memory_order_relaxed) != BenchState::DONE) {
        uint64_t key_id = zipf.next();
        std::string key = "user:" + std::to_string(key_id);

        int dice_roll = ratio_dist(rng);
        bool is_read = (dice_roll <= config.read_percent);

        op_timer.start();
        bool success = false;
        uint64_t out_size = 0;

        if (is_read) {
            success = db->get(key, read_buffer, out_size);
            if (success && !verify_payload(key_id, read_buffer, out_size)) {
                metrics->corruptions++;
            }
        } else {
            // Pick a completely new size to force Reallocations and SCM::free()
            size_t new_dynamic_size = size_dist(rng);
            generate_payload(key_id, true, update_payload, new_dynamic_size);
            success = db->put(key, update_payload, new_dynamic_size);
        }

        double latency = op_timer.elapsed_us();
        
        if (global_bench_state.load(std::memory_order_relaxed) == BenchState::MEASURE) {
            metrics->total_ops++;
            metrics->total_latency_us += latency;
            if (latency > metrics->max_latency_us) metrics->max_latency_us = latency;
            if (success) {
                if (is_read) metrics->successful_reads++;
                else metrics->successful_updates++;
            }
        }
    }
}

std::atomic<uint64_t> telemetry_samples{0};
std::atomic<uint64_t> critical_samples{0};

void telemetry_worker(StorageEngine* db) {
    while (global_bench_state.load(std::memory_order_relaxed) != BenchState::DONE) {
        // Only record data during the MEASURE phase
        if (global_bench_state.load(std::memory_order_relaxed) == BenchState::MEASURE) {
            telemetry_samples.fetch_add(1, std::memory_order_relaxed);
            
            if (db->is_arena_critical()) {
                critical_samples.fetch_add(1, std::memory_order_relaxed);
            }
        }
        // Wake up every 1 millisecond to sample the system
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// ============================================================================
// 7. THE MAIN HARNESS
// ============================================================================
int main() {
    std::filesystem::remove("../data/imdb.aof");

    std::cout << "========================================================\n";
    std::cout << "   IMDB Benchmark (Dynamic Sizes - YCSB Workload B)     \n";
    std::cout << "========================================================\n";

    BenchConfig config;
    DBConfig db_config;
    db_config.arena_size = 1024 * 1024 * 64; // chenge this when needed
    StorageEngine db(db_config); 

    load_database(db, config);

    std::vector<std::thread> workers;
    std::vector<ThreadMetrics> metrics(config.num_threads);

    std::cout << "[Phase 2] Starting " << config.num_threads << " Zipfian worker threads...\n";
    for (int i = 0; i < config.num_threads; i++) {
        workers.emplace_back(benchmark_worker, i, &db, config, &metrics[i]);
    }
    std::thread telemetry_thread(telemetry_worker, &db);

    std::cout << "[Phase 2] Warming up for " << config.warmup_seconds << " seconds...\n";
    std::this_thread::sleep_for(std::chrono::seconds(config.warmup_seconds));

    std::cout << "[Phase 3] WARMUP COMPLETE. Starting measurement for " << config.measure_seconds << " seconds!\n";
    global_bench_state.store(BenchState::MEASURE, std::memory_order_relaxed);

    std::this_thread::sleep_for(std::chrono::seconds(config.measure_seconds));

    std::cout << "[Phase 4] Stopping threads...\n";
    global_bench_state.store(BenchState::DONE, std::memory_order_relaxed);

    for (auto& t : workers) {
        t.join();
    }
    telemetry_thread.join();

    uint64_t total_ops = 0, total_reads = 0, total_updates = 0, total_corruptions = 0;
    double global_max_latency = 0, total_latency_sum = 0;

    for (int i = 0; i < config.num_threads; i++) {
        total_ops += metrics[i].total_ops;
        total_reads += metrics[i].successful_reads;
        total_updates += metrics[i].successful_updates;
        total_corruptions += metrics[i].corruptions;
        total_latency_sum += metrics[i].total_latency_us;
        if (metrics[i].max_latency_us > global_max_latency) global_max_latency = metrics[i].max_latency_us;
    }

    std::cout << "\n[Phase 5] Running validation sweep...\n";
    uint64_t validation_success = 0;
    char val_buf[256];
    uint64_t val_size;
    
    for (uint64_t i = 1; i <= 10000; i++) {
        std::string key = "user:" + std::to_string(i);
        if (db.get(key, val_buf, val_size)) {
            if (verify_payload(i, val_buf, val_size)) validation_success++;
        }
    }
    
    std::cout << "Validation: " << validation_success << " / 10000 initial records securely verified.\n";
    if (validation_success == 10000 && total_corruptions == 0) std::cout << "Status: PASS (No data corruption detected!)\n\n";
    else std::cout << "\033[31mStatus: FAIL (Data corruption or lost records detected!)\033[0m\n\n";

    double ops_per_second = (double)total_ops / config.measure_seconds;
    double avg_latency = total_ops > 0 ? (total_latency_sum / total_ops) : 0;

    std::cout << "\n========================================================\n";
    std::cout << "                 BENCHMARK RESULTS                      \n";
    std::cout << "========================================================\n";
    std::cout << "Threads:       " << config.num_threads << "\n";
    std::cout << "Workload:      " << config.read_percent << "% Read / " << (100 - config.read_percent) << "% Update\n";
    std::cout << "Zipfian Theta: " << config.zipf_theta << "\n";
    std::cout << "--------------------------------------------------------\n";
    std::cout << "Total Ops:     " << total_ops << "\n";
    std::cout << "Throughput:    " << std::fixed << std::setprecision(2) << ops_per_second << " ops/sec\n";
    std::cout << "Avg Latency:   " << avg_latency << " us\n";
    std::cout << "Max Latency:   " << global_max_latency << " us (p100)\n";
    std::cout << "Reads/Updates: " << total_reads << " / " << total_updates << "\n";
    if (total_corruptions > 0) std::cout << RED << "CORRUPTIONS:   " << total_corruptions << " detected!\n" << RESET;
    else std::cout << "CORRUPTIONS:   0 (Clean run!)\n";
    std::cout << "========================================================\n";

    double critical_percent = (double)critical_samples / telemetry_samples * 100.0;
    std::cout << "Arena Critical State: " << std::fixed << std::setprecision(2) 
              << critical_percent << "% of the time (" 
              << critical_samples << " / " << telemetry_samples << " ms)\n";

    // --- EVICTION STATS ANALYSIS ---
    uint64_t hot_rescued = db.hot_rescued_count.load();
    uint64_t whole_evicted = db.whole_page_evicted_count.load();
    uint64_t partial_evicted = db.partial_page_evicted_count.load();
    uint64_t total_scanned = db.check_page_count.load();

    // The arena capacity in pages (based on your 64MB / 4KB config)
    const uint64_t arena_pages = db_config.arena_size / 4096; 
    
    double iterations = (double)total_scanned / arena_pages;
    double candidate_pct = total_scanned > 0 ? ((double)(whole_evicted + partial_evicted) / total_scanned) * 100.0 : 0;
    double eviction_ratio = partial_evicted > 0 ? (double)whole_evicted / partial_evicted : (double)whole_evicted;

    std::cout << "=========================================================\n";
    std::cout << "Eviction Stats:\n";
    std::cout << "Hot rescued record:         " << hot_rescued << "\n";
    std::cout << "Whole page evicted count:   " << whole_evicted << "\n";
    std::cout << "Partial page evicted count: " << partial_evicted << "\n";
    std::cout << "Check page count:           " << total_scanned << "\n";
    std::cout << "---------------------------------------------------------\n";
    std::cout << "Arena Revolutions:          " << std::fixed << std::setprecision(2) << iterations << "x\n";
    std::cout << "Eviction Success Rate:      " << candidate_pct << "% (Evicted/Scanned)\n";
    std::cout << "Whole/Partial Ratio:        " << eviction_ratio << ":1\n";
    std::cout << "=========================================================\n";

    return 0;
}