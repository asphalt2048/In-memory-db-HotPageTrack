#!/bin/bash

# Ensure we have the latest compiled version (assuming makefile is in root)
echo "Compiling..."
make clean
make benchmark

# Define output CSV file
OUTPUT_CSV="./test/benchmark_results.csv"
echo "Arena(MB),HotScale,AgeSpeed,Throughput,P99_us,P99.9_us,P100_us,Critical_State_%,Success_Rate_%,Hot_Rescued,Whole_Evictions,Partial_Evictions" > $OUTPUT_CSV

# Define the testing matrices
ARENA_SIZES=(64)           # arena is 64MB
HOT_SCALES=(1 2 4)         # 1 = Normal, 2 = Strict, 4 = clock algorithm off
AGE_SPEEDS=(1 2)           # 1 = Normal decay, 2 = Accelerated decay
BACKPRESSURE_US=(1 2 5)  # backpressure in us

TOTAL_TESTS=$((${#ARENA_SIZES[@]} * ${#HOT_SCALES[@]} * ${#AGE_SPEEDS[@]} * ${#BACKPRESSURE_US[@]}))
CURRENT_TEST=1

echo "Starting automated benchmark suite. Total tests: $TOTAL_TESTS"
echo "Results will be saved to $OUTPUT_CSV"
echo "--------------------------------------------------------"

for mb in "${ARENA_SIZES[@]}"; do
    for scale in "${HOT_SCALES[@]}"; do
        for age in "${AGE_SPEEDS[@]}"; do
            for Backpressure_us in "${BACKPRESSURE_US[@]}"; do
            
                echo "Running Test $CURRENT_TEST / $TOTAL_TESTS : Arena=${mb}MB, Scale=${scale}, Age=${age}, Backpressure us=${Backpressure_us}..."
                
                # Note: Calling the executable inside the /build folder!
                ./build/db_bench --arena-mb $mb --hot-scale $scale --age-speed $age > ./test/temp_run.log
                
                # Extract basic metrics
                THROUGHPUT=$(grep "Throughput:" ./test/temp_run.log | awk '{print $2}')
                
                # Extract Latencies
                P99=$(grep "P99 Latency:" ./test/temp_run.log | awk '{print $3}')
                P999=$(grep "P99.9 Latency:" ./test/temp_run.log | awk '{print $3}')
                P100=$(grep "Max Latency:" ./test/temp_run.log | awk '{print $3}')
                
                # Extract Status & Eviction Metrics
                CRITICAL=$(grep "Arena Critical State:" ./test/temp_run.log | awk '{print $4}' | tr -d '%')
                SUCCESS_RATE=$(grep "Eviction Success Rate:" ./test/temp_run.log | awk '{print $4}' | tr -d '%')
                HOT_RESCUED=$(grep "Hot rescued record:" ./test/temp_run.log | awk '{print $4}')
                WHOLE_EVICTED=$(grep "Whole page evicted count:" ./test/temp_run.log | awk '{print $5}')
                PARTIAL_EVICTED=$(grep "Partial page evicted count:" ./test/temp_run.log | awk '{print $5}')
                
                # Append to CSV
                echo "$mb,$scale,$age,$Backpressure_us,$THROUGHPUT,$P99,$P999,$P100,$CRITICAL,$SUCCESS_RATE,$HOT_RESCUED,$WHOLE_EVICTED,$PARTIAL_EVICTED" >> $OUTPUT_CSV
                
                echo "  -> Done! Tput: $THROUGHPUT, P99: $P99 us, P100: $P100 us, Success: $SUCCESS_RATE%"
                ((CURRENT_TEST++))
                
                # Sleep for 5 seconds between runs to let the OS clear page caches & cool CPU
                sleep 5
            done
        done
    done
done

rm ./test/temp_run.log
echo "--------------------------------------------------------"
echo "All tests complete! Check $OUTPUT_CSV for results."