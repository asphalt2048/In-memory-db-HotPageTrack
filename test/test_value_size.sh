#!/bin/bash
# Test the system's scalabilty under different value sizes. Output Hit Rate, P99, and MAF.
set -e

echo "========================================"
echo " Building Benchmark Engine..."
echo "========================================"
make benchmark

OUTPUT_CSV="./test/value_size_results.csv"

echo "value size,Cache Hit Rate,P99(us),Whole/Partial Ratio,Average MAF" > $OUTPUT_CSV
echo "-----------------------------------------------------------------------------------------"
echo -e "Value Size\t| Hit Rate (%)\t| P99 (us)\t| Whole/Partial\t| Average MAF"
echo "-----------------------------------------------------------------------------------------"

VALUE_SIZES=(24 56 248 57)

for SIZE in "${VALUE_SIZES[@]}"; do
    # Run the benchmark
    RAW_OUTPUT=$(./build/db_bench --value-size $SIZE --partial-track)
    
    HIT_RATE=$(echo "$RAW_OUTPUT" | grep "cache hit rate:" | awk '{print $4}')
    P99=$(echo "$RAW_OUTPUT" | grep "P99 Latency:" | awk '{print $3}')
    WHOLE_PARTIAL=$(echo "$RAW_OUTPUT" | grep "Whole/Partial Ratio:" | awk '{print $3}')
    AVG_MAF=$(echo "$RAW_OUTPUT" | grep "Average MAF:" | awk '{print $3}')

    echo -e "${SIZE}\t\t| ${HIT_RATE}\t\t| ${P99}\t\t| ${WHOLE_PARTIAL}\t\t| ${AVG_MAF}"
    
    echo "${SIZE},${HIT_RATE},${P99},${WHOLE_PARTIAL},${AVG_MAF}" >> $OUTPUT_CSV

    sleep 10
done

echo "-----------------------------------------------------------------------------------------"
echo "Value size testing complete! See ${OUTPUT_CSV} for result."