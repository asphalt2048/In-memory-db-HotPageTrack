#!/bin/bash
# Test the system's scalabilty under varying threads (including oversubscribed).
# Outputs all metrics required for Table 6-1 directly to CSV.

set -e

echo "========================================"
echo " Building Benchmark Engine..."
echo "========================================"
make benchmark

OUTPUT_CSV="./test/scalability_results.csv"

echo "Threads,Throughput(M op/s),Avg Latency(us),P50(us),P90(us),P99(us),P99.9(us),Cache Hit Rate(%),Critical(%),Whole/Partial Ratio" > $OUTPUT_CSV

echo -e "\nStarting Scalability & Oversubscription Test..."
echo "Results will be saved to $OUTPUT_CSV"
echo "------------------------------------------------------------"
echo -e "Threads\t| Status"
echo "------------------------------------------------------------"

THREAD_COUNTS=(1 2 4 8 16 32 48 64)

for THREADS in "${THREAD_COUNTS[@]}"; do
    echo -n -e "${THREADS}\t| Running... "
    
    RAW_OUTPUT=$(./build/db_bench --threads $THREADS --partial-track)
    
    TPUT_M=$(echo "$RAW_OUTPUT" | grep "Throughput:" | awk '{printf "%.2f", $2/1000000}')
    
    AVG_LAT=$(echo "$RAW_OUTPUT" | grep "Avg Latency:" | awk '{print $3}')
    P50=$(echo "$RAW_OUTPUT" | grep "P50 Latency:" | awk '{print $3}')
    P90=$(echo "$RAW_OUTPUT" | grep "P90 Latency:" | awk '{print $3}')
    P99=$(echo "$RAW_OUTPUT" | grep "P99 Latency:" | awk '{print $3}')
    P999=$(echo "$RAW_OUTPUT" | grep "P99.9 Latency:" | awk '{print $3}')
    
    HIT_RATE=$(echo "$RAW_OUTPUT" | grep "cache hit rate:" | awk '{print $4}' | tr -d '%')
    CRITICAL=$(echo "$RAW_OUTPUT" | grep "Arena Critical State:" | awk '{print $4}' | tr -d '%')
    RATIO=$(echo "$RAW_OUTPUT" | grep "Whole/Partial Ratio:" | awk '{print $3}')
    
    echo "Done! (Tput: ${TPUT_M} M op/s, P99: ${P99} us)"
    
    echo "${THREADS},${TPUT_M},${AVG_LAT},${P50},${P90},${P99},${P999},${HIT_RATE},${CRITICAL},${RATIO}" >> $OUTPUT_CSV

    sleep 15
done

echo "------------------------------------------------------------"
echo "Scalability testing complete! See ${OUTPUT_CSV} for results."