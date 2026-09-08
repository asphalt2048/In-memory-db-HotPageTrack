# Slab-like K-V Engine with Hot Record Rescue
This is a kernel-bypass, slab-like key-value storage engine. I code this engine to complete my research on "hot spot rescue", which is the machaniam and policy that helps to keep the most accessed records in RAM when Data Size >> RAM Size. Such feature is essential for almost every In-Memory DB. 

Basically the engine will have to manage the allocation, free and swap-in/out of free space without relying on the default options provided by OS. This doc will explain the architectural design of this project.

If you are interested in running the project, See [This Section](#compile-and-run).

---

## Why slab-like 
In-Memory DB like redis uses Object-Level Management, which maintains LRU/LFU metadata on individual key-value objects. While eviction is precise, updating object-level metadata on every request causes severe multi-core cache-line bouncing and lock contention, while incurring heavy metadata memory overhead.  

Slab-like, on the other hand, allocates and evicts memory in fixed-size slots across 4KB physical pages. This enables $O(1)$ allocation speeds, ultra-compact metadata, and seamless lock-free bitmap tracking for multi-core scalability. 

The problem faced by slab-like design is "Granularity Mismatch": coarse 4KB page-level eviction often "mis-kills" active hot records that happen to reside within an otherwise cold page. This is the major problem this project wants to solve, through the "hot spot rescue" mechanism.

---

## Design of Memory Pool
This part involves the `Arena` and `SizeClassManager(SCM)`. 

### Arena
Arena is the global memory pool, which requests a configurable block of memory from the OS, locks it in RAM using `mlock`, and logically divides it into standard 4KB pages. The entire storage engine relies on this pre-allocated space to store records.  

Currently, Arena manages pages through a simple yet performant design: it pairs a lock-free bitmap with a heuristic cursor to achieve O(1) free page allocations. The cursor tracks the most recent bitmap chunk where a free page was located, allowing threads to skip saturated memory blocks and avoid scanning the entire bitmap on every request. However, a key trade-off of this scheme is that allocating contiguous physical pages is difficult. 

I intentionally avoided a buddy allocator design because the system is specifically optimized for extremely fast operations on small records. Incorporating support for large records that require contiguous pages into this main pool would introduce significant management overhead and severely degrade small-record performance. It is far more effective to implement a dedicated, separate allocation path for large records rather than mixing both paradigms in the same pool.

The bitmap structure also provides a distinct concurrency advantage. Allocation state updates can be performed using atomic Compare-and-Swap (CAS) instructions, making the page allocation path entirely lockless.  

The other core responsibility of Arena is monitoring global memory pressure. Arena continuously tracks usage against high, low, and critical watermarks, automatically signaling the background sweeper thread to initiate page reclamation whenever usage breaches defined safety limits.   

### SCM
SizeClassManager (SCM) is the fixed-length slot allocator that sits on top of Arena. It takes the 4KB logical pages managed by Arena and partitions them into uniform slots tailored for specific record size classes. 

While Arena handles coarse page allocation, SCM provides fine-grained slot allocation and deallocation for small records. SCM manages pages through a single linked list of partially full pages (`partial_list_head`). Fully populated pages are unlinked immediately to eliminate traversal overhead during allocation, while completely empty pages are returned directly to Arena to prevent local page hoarding. Slot availability within a page is tracked using an explicit bitmask array (`is_allocated`) located in the page header, allowing $O(1)$ slot location via hardware `__builtin_ctzll` instructions.  

The benefit of using bitmask array instead of internal free list(storing free-pointers inside unallocated slots) is that it avoids cacheline bouncing when searching for free slot, while providing an atomic way to check slot status.

The concurrency design of SCM also decouples slot-level state updates from page-level list management. While modifying the partial page list uses a local lock, updating individual slot metadata—such as record hotness promotion or aging—is performed via lock-free CAS loops directly on the page header arrays. 

SCM also provides `alloc_notrigger()`, an optimistic allocation path that fails fast if no partial pages exist instead of triggering new page allocations from Arena. This interface is used by sweeper when rescuing hot records.

The other primary responsibility of SCM is cooperating with the background eviction subsystem. Through `quarantine_page()` and `unquarantine_page()`, SCM allows the Sweeper thread to safely isolate victim pages from frontend write paths while preventing double-free race conditions.  

### The Lifecycle of Page
![Page Lifecycle](./images/Memory%20Page%20Management%20Flow.png)

---

## Design of DB Operations
The engine support standard K-V storage interface like `put()`, `get()` and `del()`. This part explains the internal structure and mechanism that support those operations.

### Metadata with Sharded Locks
To support $O(1)$ key-value operations while enabling efficient reverse lookups for background eviction, the engine decouples string key management from physical storage locations using a two-tier secondary index built on intermediate Logical IDs. Managed separately from the main Arena pool, this layer routes requests through two distinct structures: a Sharded Hash Table and a global Translation Table (TT).

```txt
[Key] ---> (Sharded Hash Table) ---> [Logical ID] --
--> (Translation Table array) ---> [RAM Pointer / Disk Offset]
```

- Sharded Hash Table: Maps variable-length string keys to unique Logical IDs. This key-to-ID mapping remains static throughout the lifetime of the key-value pair.  
- Global Translation Table (TT): A dense array indexed directly by Logical ID. To minimize memory footprint, each TT entry (RecordLoc) is packed into a tight 16-byte structure using C++ union fields.

### Operation's Workflow
While this indexing layout enables rapid routing, executing concurrent CRUD operations across these metadata shards introduces a fundamental synchronization challenge when interacting with background memory reclamation. To prevent deadlocks between foreground worker threads and background Sweeper evictions, the engine replaces traditional "lock-first" allocations with an optimistic Execute-Recheck-Rollback protocol:

- Execute (Lock-Free Pre-allocation): Threads optimistically request a physical slot from the SCM without holding any metadata locks, ensuring physical memory allocation never blocks background sweeps.  
- Recheck (Lock & Validate): After securing a slot, the thread acquires the target Translation Table (TT) shard lock and verifies record state to detect TOCTOU race conditions.  
- Rollback or Commit: If validation fails, the thread immediately frees the pre-allocated slot and retries or returns early; if validation passes, it safely commits the metadata update.  

By moving heavy physical allocation outside metadata lock boundaries, this protocol completely eliminates circular lock-allocation dependencies while maintaining multi-core throughput. 

---

## Design of Background Sweeper
The Sweeper is an asynchronous background thread responsible for memory reclamation when global Arena watermarks are breached. 

### Hotness Evaluation & Rescue Policy
To prevent coarse page eviction from mis-killing active hot records, the engine combines slot-level access tracking, page-level clock exemptions, and zero-I/O hot record rescue. 

- Slot-Level 2-Bit Clock: Inspired by Linux MGLRU, each slot uses a 2-bit counter (0–3) modified via lock-free CAS loops. Foreground reads and writes increment the counter via lock-free CAS loops. The Sweeper applies atomic temporal decay during background sweeps.
- Page-Level Clock Exemption: Before evicting an LRU victim page, the Sweeper aggregates the hotness of all its slots. If the sum exceeds a threshold, the entire page earns a "second chance"
- Precise Hot Rescue: For un-exempted victim pages, the Sweeper rescues max-heat records (heat = 3) by moving them into existing micro-voids in partially full pages via `scm.alloc_notrigger()`. 

### Sweeper Workflow
The logic of sweeper is defined in `StorageEngine.cpp`, see `evict_cold_page()` and `page_hot_rescue()`. Outlined in the flowchart below:

![Sweeper Workflow](./images/Sweeper-workflow.png)

---

## System Evaluation

**Platform:** Basically WSL2 on my PC, which has Intel Core i7-14700HX CPU (20 cores / 28 threads) with DDR5 RAM and an NVMe SSD, running Ubuntu 22.04 LTS. Compiled using `g++ -O3`. 


**Workload Configuration:** Benchmarked under an 8-thread YCSB workload configured for 95% Read / 5% Update under extreme Zipfian skew ($\theta = 0.99$).


**Capacity Inversion Pressure:** Managing 1,000,000 variable-length records (~127.8 MiB logical data) requires ~191.6 MiB of physical RAM when fully resident. With the global Arena strictly capped at 64 MiB, the system operates under a severe **2.99:1 effective capacity inversion ratio**, forcing active background swapping.

Enabling Page Clock Exemption and Hot Rescue suppressed P99 tail latency by **54.6%** (dropping from 14.96μs to 6.79μs) while boosting overall throughput to 5.99 M ops/s under heavy memory inversion.

| Metric | Baseline (Disabled) | Optimized (Enabled) |
| --- | --- | --- |
| **Throughput** | 5.51 M ops/s | **5.99 M ops/s** |
| Average Latency | 1.32 μs | 1.21 μs |
| P50 Latency | 0.50 μs | 0.50 μs |
| **P99 Latency** | 14.96 μs| **6.79 μs (-54.6%)** |
| Cache Hit Rate | 85.35% | 87.39% |
| Hot Rescue Count | 0 | 1,301 M |


---

## Compile and Run
I have included a makefile and several scripts to make things easier. Use:
```bash
> make clean && make benchmark
```
to generate the benchmark binary. Then use `./build/db_bench` to run. 

The benchmark support multiple CLI args that changes the configuration of the engine or benchmark itself, run the benchmark with `--help` to view all args.

There are also some script that test how this engine scales with thread number and record size, run `test_threads.sh` and `test_value_size.sh` to get a result on your own.