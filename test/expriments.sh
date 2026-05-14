#!/bin/bash

# Ensure we have the latest compiled version (assuming makefile is in root)
echo "Compiling..."
make benchmark

# Define our output CSV file (saving it in the test folder)
OUTPUT_CSV="./test/benchmark_results.csv"
echo "Arena(MB),HotScale,AgeSpeed,Throughput,P100_us,Critical_State_%,Success_Rate_%,Hot_Rescued,Whole_Evictions,Partial_Evictions" > $OUTPUT_CSV

# Define the testing matrices
ARENA_SIZES=(64)           # Start with 64MB
HOT_SCALES=(1 2 4)         # 1 = Normal, 2 = Strict (max_slots/2), 4 = Brutal (max_slots/4)
AGE_SPEEDS=(1 2)           # 1 = Normal decay, 2 = Accelerated decay

TOTAL_TESTS=$((${#ARENA_SIZES[@]} * ${#HOT_SCALES[@]} * ${#AGE_SPEEDS[@]}))
CURRENT_TEST=1

echo "Starting automated benchmark suite. Total tests: $TOTAL_TESTS"
echo "Results will be saved to $OUTPUT_CSV"
echo "--------------------------------------------------------"

for mb in "${ARENA_SIZES[@]}"; do
    for scale in "${HOT_SCALES[@]}"; do
        for age in "${AGE_SPEEDS[@]}"; do
            
            echo "Running Test $CURRENT_TEST / $TOTAL_TESTS : Arena=${mb}MB, Scale=${scale}, Age=${age}..."
            
            # Note: Calling the executable inside the /build folder!
            ./build/db_bench --arena-mb $mb --hot-scale $scale --age-speed $age > ./test/temp_run.log
            
            # Extract basic metrics
            THROUGHPUT=$(grep "Throughput:" ./test/temp_run.log | awk '{print $2}')
            P100=$(grep "Max Latency:" ./test/temp_run.log | awk '{print $3}')
            CRITICAL=$(grep "Arena Critical State:" ./test/temp_run.log | awk '{print $4}' | tr -d '%')
            SUCCESS_RATE=$(grep "Eviction Success Rate:" ./test/temp_run.log | awk '{print $4}' | tr -d '%')
            
            # Extract Eviction Specifics
            HOT_RESCUED=$(grep "Hot rescued record:" ./test/temp_run.log | awk '{print $4}')
            WHOLE_EVICTED=$(grep "Whole page evicted count:" ./test/temp_run.log | awk '{print $5}')
            PARTIAL_EVICTED=$(grep "Partial page evicted count:" ./test/temp_run.log | awk '{print $5}')
            
            # Append to CSV
            echo "$mb,$scale,$age,$THROUGHPUT,$P100,$CRITICAL,$SUCCESS_RATE,$HOT_RESCUED,$WHOLE_EVICTED,$PARTIAL_EVICTED" >> $OUTPUT_CSV
            
            echo "  -> Done! Tput: $THROUGHPUT, Success: $SUCCESS_RATE%, Rescued: $HOT_RESCUED"
            ((CURRENT_TEST++))
            
            # Sleep for 5 seconds between runs to let the OS clear page caches & cool CPU
            sleep 5
        done
    done
done

rm ./test/temp_run.log
echo "--------------------------------------------------------"
echo "All tests complete! Check $OUTPUT_CSV for results."