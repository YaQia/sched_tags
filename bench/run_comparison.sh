#!/bin/bash
#
# bench/run_comparison.sh — Compare baseline vs instrumented atomic_dense scheduling
#
# This script builds both baseline and instrumented versions of the benchmark
# and runs them side-by-side to demonstrate the scheduling improvement.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "======================================================================"
echo "  Atomic-Dense Scheduling Validation"
echo "======================================================================"
echo ""

# Check if pass is built
if [ ! -f "$PROJECT_ROOT/build/pass/libSchedTagPass.so" ]; then
	echo "Error: Pass not found at build/pass/libSchedTagPass.so"
	echo "Please run: cmake --build build"
	exit 1
fi

# Configuration
NUM_THREADS=${1:-4}
DURATION_MS=${2:-2000}

echo "Configuration:"
echo "  Threads:      $NUM_THREADS"
echo "  Duration:     ${DURATION_MS}ms"
echo ""

# Build baseline version (no instrumentation)
echo "[1/3] Building baseline version (no instrumentation)..."
clang -O2 "$SCRIPT_DIR/atomic_contention.c" -pthread \
	-o /tmp/atomic_baseline
echo "  → /tmp/atomic_baseline"
echo ""

# Build instrumented version (with atomic_dense hints)
echo "[2/3] Building instrumented version (with atomic_dense hints)..."
clang -O2 -fpass-plugin="$PROJECT_ROOT/build/pass/libSchedTagPass.so" \
	"$SCRIPT_DIR/atomic_contention.c" -pthread \
	-o /tmp/atomic_tagged
echo "  → /tmp/atomic_tagged"
echo ""

# Verify instrumentation
echo "[2.5/3] Verifying instrumentation..."
if readelf -s /tmp/atomic_tagged | grep -q "__sched_hint\.data"; then
	echo "  ✓ __sched_hint.data TLS symbol found"
else
	echo "  ✗ Warning: __sched_hint.data not found (pass may not have instrumented)"
fi
echo ""

# Run baseline
echo "======================================================================"
echo "[3a/3] Running BASELINE (no instrumentation)..."
echo "======================================================================"
echo ""
/tmp/atomic_baseline "$NUM_THREADS" "$DURATION_MS" >/tmp/baseline_output.txt
cat /tmp/baseline_output.txt
echo ""

# Extract baseline metrics
BASELINE_THROUGHPUT=$(grep "Throughput:" /tmp/baseline_output.txt | awk '{print $2}')
BASELINE_SMT_RATE=$(grep "SMT sibling pairs:" /tmp/baseline_output.txt | grep -oP '\(\K[0-9.]+(?=%\))')

echo ""
echo "======================================================================"
echo "[3b/3] Running INSTRUMENTED (with atomic_dense hints)..."
echo "======================================================================"
echo ""
/tmp/atomic_tagged "$NUM_THREADS" "$DURATION_MS" >/tmp/tagged_output.txt
cat /tmp/tagged_output.txt
echo ""

# Extract instrumented metrics
TAGGED_THROUGHPUT=$(grep "Throughput:" /tmp/tagged_output.txt | awk '{print $2}')
TAGGED_SMT_RATE=$(grep "SMT sibling pairs:" /tmp/tagged_output.txt | grep -oP '\(\K[0-9.]+(?=%\))')

# Calculate improvement
if [ -n "$BASELINE_SMT_RATE" ] && [ -n "$TAGGED_SMT_RATE" ]; then
	SMT_IMPROVEMENT=$(awk "BEGIN {printf \"%.1f\", $TAGGED_SMT_RATE - $BASELINE_SMT_RATE}")
else
	SMT_IMPROVEMENT="N/A"
fi

if [ -n "$BASELINE_THROUGHPUT" ] && [ -n "$TAGGED_THROUGHPUT" ]; then
	THROUGHPUT_IMPROVEMENT=$(awk "BEGIN {printf \"%.1f\", ($TAGGED_THROUGHPUT - $BASELINE_THROUGHPUT) / $BASELINE_THROUGHPUT * 100}")
else
	THROUGHPUT_IMPROVEMENT="N/A"
fi

# Print summary
echo ""
echo "======================================================================"
echo "  COMPARISON SUMMARY"
echo "======================================================================"
echo ""
printf "%-25s %15s %15s %15s\n" "Metric" "Baseline" "Instrumented" "Improvement"
echo "----------------------------------------------------------------------"
printf "%-25s %15s %15s %15s\n" \
	"SMT Co-location Rate" \
	"${BASELINE_SMT_RATE}%" \
	"${TAGGED_SMT_RATE}%" \
	"+${SMT_IMPROVEMENT}pp"
printf "%-25s %15s %15s %15s\n" \
	"Throughput (M ops/s)" \
	"$BASELINE_THROUGHPUT" \
	"$TAGGED_THROUGHPUT" \
	"+${THROUGHPUT_IMPROVEMENT}%"
echo "----------------------------------------------------------------------"
echo ""

# Evaluation
if [ -n "$TAGGED_SMT_RATE" ] && [ -n "$BASELINE_SMT_RATE" ]; then
	if awk "BEGIN {exit !($TAGGED_SMT_RATE > $BASELINE_SMT_RATE + 5.0)}"; then
		echo "✓ SUCCESS: Instrumented version shows improved SMT co-location!"
		echo "  The scheduler is using atomic_dense hints effectively."
		exit 0
	else
		echo "✗ INCONCLUSIVE: SMT co-location improvement is minimal or negative."
		echo "  Possible reasons:"
		echo "  1. Kernel scheduler does not support atomic_dense hints yet"
		echo "  2. System topology doesn't benefit from SMT co-location"
		echo "  3. Test duration too short or system load too high"
		echo "  4. Pass did not instrument the hot loop correctly"
		exit 1
	fi
else
	echo "✗ ERROR: Failed to extract metrics from benchmark output"
	exit 1
fi
