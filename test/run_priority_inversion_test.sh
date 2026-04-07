#!/bin/bash
#
# test/run_priority_inversion_test.sh
#
# Build and run the priority inversion prevention test.
# This script compares behavior WITH and WITHOUT the unshared tag instrumentation.
#
# Usage:
#   ./test/run_priority_inversion_test.sh [options]
#
# Options:
#   --with-tag      Only run the instrumented version
#   --without-tag   Only run the plain version
#   --compare       Run both and compare (default)
#   --verbose       Show verbose output including IR diff
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"
TMP_DIR="/tmp/priority_inversion_test"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Default options
RUN_WITH_TAG=true
RUN_WITHOUT_TAG=true
VERBOSE=false

# Parse arguments
while [[ $# -gt 0 ]]; do
	case $1 in
	--with-tag)
		RUN_WITHOUT_TAG=false
		shift
		;;
	--without-tag)
		RUN_WITH_TAG=false
		shift
		;;
	--compare)
		RUN_WITH_TAG=true
		RUN_WITHOUT_TAG=true
		shift
		;;
	--verbose)
		VERBOSE=true
		shift
		;;
	-h | --help)
		echo "Usage: $0 [--with-tag|--without-tag|--compare] [--verbose]"
		exit 0
		;;
	*)
		echo "Unknown option: $1"
		exit 1
		;;
	esac
done

echo -e "${BLUE}================================================================${NC}"
echo -e "${BLUE}Priority Inversion Prevention Test${NC}"
echo -e "${BLUE}================================================================${NC}"
echo ""

# Check prerequisites
check_prerequisites() {
	echo -e "${YELLOW}Checking prerequisites...${NC}"

	# Check for LLVM tools
	for tool in clang opt llc; do
		if ! command -v $tool &>/dev/null; then
			echo -e "${RED}Error: $tool not found in PATH${NC}"
			exit 1
		fi
	done

	# Check for the pass plugin
	PASS_PLUGIN="$BUILD_DIR/pass/libSchedTagPass.so"
	if [[ ! -f "$PASS_PLUGIN" ]]; then
		echo -e "${RED}Error: Pass plugin not found at $PASS_PLUGIN${NC}"
		echo "Please build the project first: cmake --build build"
		exit 1
	fi

	# Check for test files
	for file in "$SCRIPT_DIR/critical_section.c" \
		"$SCRIPT_DIR/priority_inversion_test.c" \
		"$SCRIPT_DIR/priority_inversion_tags.json"; do
		if [[ ! -f "$file" ]]; then
			echo -e "${RED}Error: Required file not found: $file${NC}"
			exit 1
		fi
	done

	echo -e "${GREEN}Prerequisites OK${NC}"
	echo ""
}

# Create temp directory
setup_temp_dir() {
	mkdir -p "$TMP_DIR"
	echo "Using temp directory: $TMP_DIR"
	echo ""
}

# Build version WITH unshared tag instrumentation
build_with_tag() {
	echo -e "${YELLOW}Building WITH unshared tag instrumentation...${NC}"

	# Step 1: Compile C to LLVM IR (with -g to preserve debug info for magic_vars lookup)
	echo "  [1/4] Compiling critical_section.c to LLVM IR (with debug info)..."
	clang -g -S -emit-llvm -O1 \
		"$SCRIPT_DIR/critical_section.c" \
		-o "$TMP_DIR/critical.ll"

	# Step 2: Run the SchedTag pass
	echo "  [2/4] Running SchedTagPass with unshared tag..."
	opt -load-pass-plugin="$BUILD_DIR/pass/libSchedTagPass.so" \
		-passes=sched-tag \
		-sched-tags-file="$SCRIPT_DIR/priority_inversion_tags.json" \
		-sched-auto-analysis=false \
		"$TMP_DIR/critical.ll" \
		-o "$TMP_DIR/critical_tagged.bc" \
		2>&1 | tee "$TMP_DIR/pass_output.txt"

	# Check if pass found the magic_vars
	if grep -q "bases=0" "$TMP_DIR/pass_output.txt"; then
		echo -e "${YELLOW}  Warning: magic_vars not found (bases=0). Bloom filter will be empty.${NC}"
	fi

	if $VERBOSE; then
		echo ""
		echo "  Pass output:"
		cat "$TMP_DIR/pass_output.txt" | sed 's/^/    /'
	fi

	# Step 3: Compile to object file
	echo "  [3/4] Compiling to object file..."
	llc "$TMP_DIR/critical_tagged.bc" -filetype=obj \
		-o "$TMP_DIR/critical_tagged.o"

	# Step 4: Link with test program
	echo "  [4/4] Linking test program..."
	clang -O2 \
		"$SCRIPT_DIR/priority_inversion_test.c" \
		"$TMP_DIR/critical_tagged.o" \
		-I"$PROJECT_DIR/include" \
		-lpthread \
		-o "$TMP_DIR/test_with_tag"

	echo -e "${GREEN}Build complete: $TMP_DIR/test_with_tag${NC}"
	echo ""

	if $VERBOSE; then
		echo "  Verifying TLS symbol in object file:"
		readelf -s "$TMP_DIR/critical_tagged.o" 2>/dev/null | grep -E "(sched_hint|TLS)" | sed 's/^/    /' || true
		echo ""
	fi
}

# Build version WITHOUT instrumentation
build_without_tag() {
	echo -e "${YELLOW}Building WITHOUT instrumentation (baseline)...${NC}"

	# Compile directly without the pass
	echo "  [1/2] Compiling critical_section.c..."
	clang -O2 -c \
		"$SCRIPT_DIR/critical_section.c" \
		-o "$TMP_DIR/critical_plain.o"

	echo "  [2/2] Linking test program..."
	clang -O2 \
		"$SCRIPT_DIR/priority_inversion_test.c" \
		"$TMP_DIR/critical_plain.o" \
		-I"$PROJECT_DIR/include" \
		-lpthread \
		-o "$TMP_DIR/test_without_tag"

	echo -e "${GREEN}Build complete: $TMP_DIR/test_without_tag${NC}"
	echo ""
}

# Run test and capture results
run_test() {
	local test_name=$1
	local test_binary=$2
	local output_file=$3

	echo -e "${YELLOW}Running: $test_name${NC}"
	echo ""

	if [[ $(id -u) -ne 0 ]]; then
		echo -e "${YELLOW}Note: Running without root privileges. Priority settings may be limited.${NC}"
		echo "      For full test, run: sudo $0"
		echo ""
	fi

	# Run the test
	"$test_binary" 2>&1 | tee "$output_file"
	echo ""
}

# Compare results
compare_results() {
	echo -e "${BLUE}================================================================${NC}"
	echo -e "${BLUE}Comparison Summary${NC}"
	echo -e "${BLUE}================================================================${NC}"
	echo ""

	# Extract slowdown factors from both runs
	local with_slowdown=$(grep "Slowdown factor:" "$TMP_DIR/result_with_tag.txt" 2>/dev/null | awk '{print $3}' | tr -d 'x')
	local without_slowdown=$(grep "Slowdown factor:" "$TMP_DIR/result_without_tag.txt" 2>/dev/null | awk '{print $3}' | tr -d 'x')

	local with_cs=$(grep "With interference:" "$TMP_DIR/result_with_tag.txt" 2>/dev/null | awk '{print $3}')
	local without_cs=$(grep "With interference:" "$TMP_DIR/result_without_tag.txt" 2>/dev/null | awk '{print $3}')

	echo "Critical section time under load:"
	echo "  WITHOUT unshared tag: ${without_cs:-N/A} us"
	echo "  WITH unshared tag:    ${with_cs:-N/A} us"
	echo ""

	echo "Slowdown factors:"
	echo "  WITHOUT unshared tag: ${without_slowdown:-N/A}x"
	echo "  WITH unshared tag:    ${with_slowdown:-N/A}x"
	echo ""

	# Calculate improvement
	if [[ -n "$with_slowdown" && -n "$without_slowdown" ]]; then
		local improvement=$(echo "scale=2; $without_slowdown - $with_slowdown" | bc 2>/dev/null || echo "N/A")
		local pct=$(echo "scale=1; (($without_slowdown - $with_slowdown) * 100) / $without_slowdown" | bc 2>/dev/null || echo "N/A")

		echo "Improvement: ${improvement}x reduction in slowdown (${pct}%)"
		echo ""

		if (($(echo "$with_slowdown < 2.0" | bc -l 2>/dev/null || echo 0))); then
			echo -e "${GREEN}SUCCESS: The unshared tag effectively prevents priority inversion!${NC}"
			echo "         Scheduler is protecting the critical section from interference."
		elif (($(echo "$with_slowdown < $without_slowdown" | bc -l 2>/dev/null || echo 0))); then
			echo -e "${YELLOW}PARTIAL SUCCESS: Some improvement, but inversion still occurring.${NC}"
		else
			echo -e "${RED}NO IMPROVEMENT: Scheduler may not be using the unshared hint.${NC}"
		fi
	else
		echo "Could not calculate improvement (missing data)"
	fi

	echo ""
}

# Main execution
main() {
	check_prerequisites
	setup_temp_dir

	if $RUN_WITH_TAG; then
		build_with_tag
	fi

	if $RUN_WITHOUT_TAG; then
		build_without_tag
	fi

	echo -e "${BLUE}================================================================${NC}"
	echo -e "${BLUE}Running Tests${NC}"
	echo -e "${BLUE}================================================================${NC}"
	echo ""

	if $RUN_WITHOUT_TAG; then
		run_test "Test WITHOUT unshared tag (baseline)" \
			"$TMP_DIR/test_without_tag" \
			"$TMP_DIR/result_without_tag.txt"
	fi

	if $RUN_WITH_TAG; then
		run_test "Test WITH unshared tag" \
			"$TMP_DIR/test_with_tag" \
			"$TMP_DIR/result_with_tag.txt"
	fi

	if $RUN_WITH_TAG && $RUN_WITHOUT_TAG; then
		compare_results
	fi

	echo -e "${BLUE}================================================================${NC}"
	echo "Test artifacts saved in: $TMP_DIR"
	echo -e "${BLUE}================================================================${NC}"
}

main "$@"
