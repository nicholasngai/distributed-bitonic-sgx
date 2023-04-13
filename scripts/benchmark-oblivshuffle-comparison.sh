#!/bin/bash

set -euo pipefail

cd "$(dirname "$0")/.."

. scripts/benchmark-common.sh

BENCHMARK_DIR=benchmarks

mkdir -p "$BENCHMARK_DIR"

a=orshuffle
e=1
s=1048576
t=1

# Build command template.
cmd_template="mpiexec -hosts enclave$ENCLAVE_OFFSET ./host/parallel ./enclave/parallel_enc.signed"

warm_up="$cmd_template bitonic 256 1"
echo "Warming up: $warm_up"
$warm_up

cleanup() {
    if "$AZ"; then
        deallocate_az_vm "$ENCLAVE_OFFSET" "$(( ENCLAVE_OFFSET + 1 ))"
    fi
}
trap cleanup EXIT

for b in 256 512 1024 2048 4096; do
    echo "Elem size: $b"

    output_filename="$BENCHMARK_DIR/$a-sgx2-enclaves$e-elemsize$b-size$s-threads$t.txt"
    if [ -f "$output_filename" ]; then
        echo "Output file $output_filename already exists; skipping"
        continue
    fi

    set_elem_size $b

    cmd="$cmd_template $a $s $t $REPEAT"
    echo "Command: $cmd"
    $cmd | tee "$output_filename"
done
