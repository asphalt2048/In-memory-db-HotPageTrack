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

bool FULL_LATENCY_TRACKING = true;

// ============================================================================
// SAMPLING CONFIGURATION (Change these to toggle strategies!)
// ============================================================================

// If tracking is 'false', we sample 1 out of every (SAMPLE_MASK + 1) operations.
// 0xFF = 255 (samples 1 in 256). 0x3FF = 1023 (samples 1 in 1024).
constexpr uint64_t SAMPLE_MASK = 0xFF; 
constexpr uint64_t SAMPLE_MULTIPLIER = SAMPLE_MASK + 1;

// Automatically sizes the vector based on your tracking choice
size_t VECTOR_CAPACITY = FULL_LATENCY_TRACKING ? 50000000 : 200000;

// ============================================================================
// 1. HIGH RESOLUTION TIMER
// ============================================================================
struct Timer {
    std::chrono::time_point<std::chrono::high_resolution_clock> start_time;
    
    void start() {
        start_time = std::chrono::high_resolution_clock::now();
    }
    
    double elapsed_ns() { 
        auto end_time = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
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
// Set to 'true' to record 100% of operations (Requires ~3.2GB RAM, absolute best P99 accuracy).
// Set to 'false' to use bitwise sampling (Requires ~6MB RAM, slightly more sensitive to OS jitter).
constexpr size_t SIZE_RANGE_START = 20;
constexpr size_t SIZE_RANGE_END = 248;

struct BenchConfig {
    uint64_t num_records = 1000000;    
    int num_threads = 8;               
    int warmup_seconds = 5;            
    int measure_seconds = 60;          
    int read_percent = 95;             
    double zipf_theta = 0.99;          
};

std::atomic<bool> keep_running{true};

// Force 64-byte alignment to prevent False Sharing
struct alignas(64) ThreadMetrics {
    uint64_t total_ops = 0;
    uint64_t successful_reads = 0;
    uint64_t successful_updates = 0;
    uint64_t corruptions = 0;          
    double total_latency_us = 0;
    size_t latency_count = 0;
    std::vector<double> latencies; 
};

enum class BenchState { WARMUP, MEASURE, DONE };
std::atomic<BenchState> global_bench_state{BenchState::WARMUP};

std::vector<size_t> global_record_sizes;

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
    if (actual_size < 16 || actual_size > 256) return false; 
    if (buffer[actual_size - 1] != '#') return false; 

    std::string expected_prefix = "K" + std::to_string(expected_id);
    if (std::memcmp(buffer, expected_prefix.c_str(), expected_prefix.length()) != 0) {
        return false;
    }

    char expected_pad = static_cast<char>((expected_id % 10) + '0');
    size_t start_pad_idx = expected_prefix.length() + 1; 
    for (size_t i = start_pad_idx; i < actual_size - 1; i++) {
        if (buffer[i] != expected_pad) return false;
    }

    return true;
}

// ============================================================================
// 5. THE LOAD PHASE 
// ============================================================================
void load_database(StorageEngine& db, BenchConfig& config) {
    global_record_sizes.resize(config.num_records + 1);

    std::cout << "[Load Phase] Sequentially inserting " << config.num_records << " records with random sizes"<<"\n"
                <<"START: "<<SIZE_RANGE_START<<" END: "<<SIZE_RANGE_END<<"\n";
    Timer timer;
    timer.start();

    char payload_buffer[256];
    uint64_t dropped_records = 0;
    
    std::mt19937 rng(999);
    std::uniform_int_distribution<size_t> size_dist(SIZE_RANGE_START, SIZE_RANGE_END); 

    for (uint64_t i = 1; i <= config.num_records; i++) {
        std::string key = "user:" + std::to_string(i);
        size_t dynamic_size = size_dist(rng);
        global_record_sizes[i] = dynamic_size;
        generate_payload(i, false, payload_buffer, dynamic_size);
        
        bool success = db.put(key, payload_buffer, dynamic_size);
        if(!success) dropped_records++;
    }
    if (dropped_records > 0) {
        std::cout << "[WARNING] Arena OOM during load! Dropped " << dropped_records << " records.\n";
    }
    std::cout << "[Load Phase] Complete. Took " << timer.elapsed_ms() << " ms.\n";
}

// ============================================================================
// 6. THE WORKER THREAD 
// ============================================================================
void benchmark_worker(int thread_id, StorageEngine* db, BenchConfig config, ThreadMetrics* metrics) {
    FastZipfian zipf(config.num_records, config.zipf_theta, 42 + thread_id);
    std::mt19937 rng(1337 + thread_id);
    std::uniform_int_distribution<int> ratio_dist(1, 100);
    std::uniform_int_distribution<size_t> size_dist(SIZE_RANGE_START, SIZE_RANGE_END); 

    // Uses the compile-time configuration to dynamically size RAM usage
    metrics->latencies.resize(VECTOR_CAPACITY, 0.0);

    char update_payload[256];
    char read_buffer[256]; 

    Timer op_timer;

    while (global_bench_state.load(std::memory_order_relaxed) != BenchState::DONE) {
        uint64_t key_id = zipf.next();
        std::string key = "user:" + std::to_string(key_id);

        int dice_roll = ratio_dist(rng);
        bool is_read = (dice_roll <= config.read_percent);

        // Uses constexpr to eliminate the branch entirely if FULL_LATENCY_TRACKING is true
        bool sample_latency = FULL_LATENCY_TRACKING || ((metrics->total_ops & SAMPLE_MASK) == 0);
        if (sample_latency) op_timer.start();

        bool success = false;
        uint64_t out_size = 0;

        if (is_read) {
            success = db->get(key, read_buffer, out_size);
            if (success && !verify_payload(key_id, read_buffer, out_size)) {
                metrics->corruptions++;
            }
        } else {
            size_t size = size_dist(rng);
            // size_t size = global_record_sizes[key_id];
            generate_payload(key_id, true, update_payload, size);
            success = db->put(key, update_payload, size);
        }
        
        if (global_bench_state.load(std::memory_order_relaxed) == BenchState::MEASURE) {
            metrics->total_ops++;
            
            if (sample_latency) {
                double latency = op_timer.elapsed_ns() / 1000.0;
                
                // Adjust total latency sum based on sampling rate
                if (FULL_LATENCY_TRACKING) {
                    metrics->total_latency_us += latency;
                } else {
                    metrics->total_latency_us += (latency * SAMPLE_MULTIPLIER); 
                }
                
                if (metrics->latency_count < VECTOR_CAPACITY) {
                    metrics->latencies[metrics->latency_count++] = latency;
                }
            }
            
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
        if (global_bench_state.load(std::memory_order_relaxed) == BenchState::MEASURE) {
            telemetry_samples.fetch_add(1, std::memory_order_relaxed);
            if (db->is_arena_critical()) {
                critical_samples.fetch_add(1, std::memory_order_relaxed);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// ============================================================================
// 7. THE MAIN HARNESS
// ============================================================================
int main(int argc, char* argv[]) {
    std::filesystem::remove("./data/imdb.aof");

    BenchConfig config;
    DBConfig db_config;
    db_config.arena_size = 1024 * 1024 * 64; 
    db_config.page_hot_scale = 2;            
    db_config.age_record_speed = 1;          
    db_config.enable_hot_rescue = true;
    db_config.enable_monitor = true; 
    db_config.backpressure_sleep_us = 0; 

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--arena-mb" && i + 1 < argc) {
            db_config.arena_size = std::stoull(argv[++i]) * 1024 * 1024;
        } else if (arg == "--hot-scale" && i + 1 < argc) {
            db_config.page_hot_scale = std::stoi(argv[++i]);
        } else if (arg == "--age-speed" && i + 1 < argc) {
            db_config.age_record_speed = std::stoi(argv[++i]);
        } else if (arg == "--backpressure-us" && i + 1 < argc) {
            db_config.backpressure_sleep_us = std::stoull(argv[++i]);
        }else if (arg == "--no-hot-rescue") {
            db_config.enable_hot_rescue = false;
        }else if (arg == "--partial-track"){
            FULL_LATENCY_TRACKING = false;
        }else if (arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [OPTIONS]\n"
                      << "Options:\n"
                      << "  --arena-mb <MB>         Set arena size in MB (default: 64)\n"
                      << "  --hot-scale <SCALE>     Set page hot scale (default: 2)\n"
                      << "  --age-speed <SPEED>     Set age record speed (default: 1)\n"
                      << "  --backpressure-us <US>  Set backpressure sleep time in microseconds (default: 1)\n"
                      << "  --no-hot-rescue         Disable hot rescue mechanism\n"
                      << "  --partial-trakc         Track latency once every 256 operations\n"
                      << "  --help                  Show this help message\n";
            return 0;
        }
    }

    std::cout << "========================================================\n";
    std::cout << "   IMDB Benchmark (Dynamic Sizes - YCSB Workload B)     \n";
    std::cout << "   Config: " << (db_config.arena_size / 1024 / 1024) << "MB Arena | "
              << "Scale: " << (unsigned int)db_config.page_hot_scale << " | "
              << "AgeSpeed: " << (unsigned int)db_config.age_record_speed << " | "
              << "Backpressure: " << db_config.backpressure_sleep_us << "us\n";
    std::cout << "   Latency Mode: " << (FULL_LATENCY_TRACKING ? "100% Tracking (~3GB RAM)" : "Sampled") << "\n";
    std::cout << "========================================================\n";

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
    double total_latency_sum = 0;

    for (int i = 0; i < config.num_threads; i++) {
        total_ops += metrics[i].total_ops;
        total_reads += metrics[i].successful_reads;
        total_updates += metrics[i].successful_updates;
        total_corruptions += metrics[i].corruptions;
        total_latency_sum += metrics[i].total_latency_us;
    }

    std::cout << "\n[Phase 5] Sorting latency samples...\n";
    std::vector<double> all_latencies;
    
    for (int i = 0; i < config.num_threads; i++) {
        all_latencies.insert(all_latencies.end(), 
                             metrics[i].latencies.begin(), 
                             metrics[i].latencies.begin() + metrics[i].latency_count);
    }
    
    std::sort(all_latencies.begin(), all_latencies.end());

    double p50 = 0, p90 = 0, p99 = 0, p999 = 0, p100 = 0;
    if (!all_latencies.empty()) {
        size_t sample_count = all_latencies.size();
        p50 = all_latencies[sample_count * 0.50];
        p90 = all_latencies[sample_count * 0.90];
        p99 = all_latencies[sample_count * 0.99];
        p999 = all_latencies[sample_count * 0.999];
        p100 = all_latencies.back();
    }

    std::cout << "\n[Phase 6] Running validation sweep...\n";
    uint64_t validation_success = 0;
    char val_buf[256];
    uint64_t val_size;
    
    // Check 5000 HOT records (Mostly in RAM)
    for (uint64_t i = 1; i <= 5000; i++) {
        std::string key = "user:" + std::to_string(i);
        if (db.get(key, val_buf, val_size)) {
            if (verify_payload(i, val_buf, val_size)) validation_success++;
        }
    }
    
    // Check 5000 COLD records (Definitely on disk)
    for (uint64_t i = 0; i < 5000; i++) {
        uint64_t cold_key = 900000 + i; 
        std::string key = "user:" + std::to_string(cold_key);
        if (db.get(key, val_buf, val_size)) {
            if (verify_payload(cold_key, val_buf, val_size)) validation_success++;
        }
    }
    
    std::cout << "Validation: " << validation_success << " / 10000 records securely verified.\n";
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
    std::cout << "P50 Latency:   " << p50 << " us\n";
    std::cout << "P90 Latency:   " << p90 << " us\n";
    std::cout << "P99 Latency:   " << p99 << " us\n";
    std::cout << "P99.9 Latency: " << p999 << " us\n";
    std::cout << "Max Latency:   " << p100 << " us (p100)\n";
    std::cout << "Reads/Updates: " << total_reads << " / " << total_updates << "\n";
    if (total_corruptions > 0) std::cout << "\033[31mCORRUPTIONS:   " << total_corruptions << " detected!\n\033[0m";
    else std::cout << "CORRUPTIONS:   0 (Clean run!)\n";
    std::cout << "========================================================\n";

    double critical_percent = (double)critical_samples / telemetry_samples * 100.0;
    std::cout << "Arena Critical State: " << std::fixed << std::setprecision(2) 
              << critical_percent << "% of the time (" 
              << critical_samples << " / " << telemetry_samples << " ms)\n";

    uint64_t hot_rescued = db.hot_rescued_count.load();
    uint64_t whole_evicted = db.whole_page_evicted_count.load();
    uint64_t partial_evicted = db.partial_page_evicted_count.load();
    uint64_t total_scanned = db.check_page_count.load();

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

    double cache_hit_rate = (db.ram_hit_count.load() + db.ram_miss_count.load() > 0) ? 
                        (double)db.ram_hit_count.load() / (db.ram_hit_count.load() + db.ram_miss_count.load()) * 100.0 : 0.0;

    std::cout << "cache hit rate: " << std::fixed << std::setprecision(2) << cache_hit_rate << "%\n";

    return 0;
}