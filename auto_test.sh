#!/usr/bin/env bash

# Create a directory to store logs (optional)
mkdir -p logs

for SMP in $(seq 1 32); do
    echo "============================================="
    echo "Starting experiment with SMP=${SMP}"
    echo "============================================="

    for N in $(seq 1 5); do
        echo "Building for SMP=${SMP}, N=${N}..."
        echo "Building 'make check SMP=${SMP} STOP_BOGOMIPS=0'..."
        make clean
        make check SMP=$SMP STOP_BOGOMIPS=0
        
        echo "Running perf..."
        sudo perf stat -B -e cache-references,cache-misses,cycles,instructions,branches,faults,migrations,L1-dcache-load-misses,L1-dcache-loads,L1-dcache-stores,L1-icache-load-misses,LLC-loads,LLC-load-misses,LLC-stores,LLC-store-misses,LLC-prefetches ./semu -k Image -c 1 -b minimal.dtb -i rootfs.cpio -n tap -d ext4.img 2>&1 | tee "logs/emulator_SMP_${SMP}_${N}_0.log"

        echo "Building 'make check SMP=${SMP} STOP_BOGOMIPS=1'..."
        make clean
        make check SMP=$SMP STOP_BOGOMIPS=1
        
        echo "Running perf..."
        sudo perf stat -B -e cache-references,cache-misses,cycles,instructions,branches,faults,migrations,L1-dcache-load-misses,L1-dcache-loads,L1-dcache-stores,L1-icache-load-misses,LLC-loads,LLC-load-misses,LLC-stores,LLC-store-misses,LLC-prefetches ./semu -k Image -c 1 -b minimal.dtb -i rootfs.cpio -n tap -d ext4.img 2>&1 | tee "logs/emulator_SMP_${SMP}_${N}_1.log"

        echo "Done with SMP=${SMP}, N=${N}. Logs saved:"
        echo "  - logs/emulator_SMP_${SMP}_${N}_0.log"
        echo "  - logs/emulator_SMP_${SMP}_${N}_1.log"
        echo
    done

    echo "Done SMP=${SMP}"
done

echo "All experiments complete!"
