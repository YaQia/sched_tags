#ifndef SCHED_TAG_COMMON_H
#define SCHED_TAG_COMMON_H

//===----------------------------------------------------------------------===//
// SchedTagCommon.h — Shared constants, result types, and utility declarations
//                    used by all sched-tag analysis and instrumentation passes.
//===----------------------------------------------------------------------===//

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"

namespace sched_tag {
using namespace llvm;

//===----------------------------------------------------------------------===//
// Constants — must match sched_hint.h exactly
//===----------------------------------------------------------------------===//

static constexpr uint32_t SCHED_HINT_MAGIC = 0x5348494EU;
static constexpr uint32_t SCHED_HINT_VERSION = 1;
static constexpr const char *SCHED_HINT_SECTION = "__sched_hint";
static constexpr int PR_SET_SCHED_HINT_OFFSET = 83;

// Compute-dense sub-type bitmask.
static constexpr uint8_t SCHED_COMPUTE_NONE = 0;
static constexpr uint8_t SCHED_COMPUTE_INT = 1U << 0;
static constexpr uint8_t SCHED_COMPUTE_FLOAT = 1U << 1;
static constexpr uint8_t SCHED_COMPUTE_SIMD = 1U << 2;

// Struct field indices (must match getSchedHintType layout).
// Header: magic(0), version(1)
// Tag payloads starting at offset 2:
static constexpr unsigned FIELD_COMPUTE_DENSE = 2; // offset 8  (i8)
static constexpr unsigned FIELD_BRANCH_DENSE = 3;  // offset 9  (i8)
static constexpr unsigned FIELD_MEMORY_DENSE = 4;  // offset 10 (i8)
static constexpr unsigned FIELD_ATOMIC_DENSE = 5;  // offset 11 (i8)
static constexpr unsigned FIELD_IO_DENSE = 6;      // offset 12 (i8)
static constexpr unsigned FIELD_UNSHARED = 7;      // offset 13 (i8)
static constexpr unsigned FIELD_COMPUTE_PREP = 8;  // offset 14 (i8)
// Extended payloads:
static constexpr unsigned FIELD_ATOMIC_MAGIC = 10;   // offset 16 (i64)
static constexpr unsigned FIELD_DEP_MAGIC = 11;      // offset 24 (i64)
static constexpr unsigned FIELD_UNSHARED_MAGIC = 12; // offset 32 (i64)
static constexpr unsigned FIELD_DEP_ROLE = 13;       // offset 40 (i8)

// Bloom filter parameters for atomic_magic (register-width bloom filter).
// Each pointer address sets K_BLOOM_BITS bit-positions in a 64-bit word.
// The scheduler detects overlap via: popcount(magic_a & magic_b) >=
// K_BLOOM_BITS.
static constexpr unsigned K_BLOOM_BITS = 4;
static constexpr uint64_t BLOOM_HASH_PRIME = 0x9E3779B97F4A7C15ULL; // fibonacci

//===----------------------------------------------------------------------===//
// Analysis result types — the "instrumentation plan"
//===----------------------------------------------------------------------===//

/// A dense loop that should be instrumented at loop boundaries.
struct LoopRegion {
  llvm::BasicBlock *Preheader; // SET here
  llvm::SmallVector<llvm::BasicBlock *, 4>
      ExitBlocks;   // (unused; CLR now handled by kernel scheduler)
  uint8_t TypeMask; // value to store in the tag payload field

  /// Base pointers (stripped of GEP/bitcasts) of atomic RMW/CAS instructions
  /// in this region.  Used to compute the bloom-filter atomic_magic at the
  /// SET boundary.  Empty for non-atomic analyses.
  llvm::SmallVector<llvm::Value *, 4> BasePointers;
};

/// A standalone dense BB (not covered by any dense loop).
struct BBRegion {
  llvm::BasicBlock *BB;
  uint8_t TypeMask; // value to store in the tag payload field

  /// Base pointers — same semantics as LoopRegion::BasePointers.
  llvm::SmallVector<llvm::Value *, 4> BasePointers;
};

/// Per-function instrumentation plan produced by a density analysis.
struct DensityResult {
  llvm::SmallVector<LoopRegion, 4> Loops;
  llvm::SmallVector<BBRegion, 8> StandaloneBBs;

  bool empty() const { return Loops.empty() && StandaloneBBs.empty(); }
};

//===----------------------------------------------------------------------===//
// Shared utility functions
//===----------------------------------------------------------------------===//

/// Create (or retrieve) the LLVM StructType matching struct sched_hint (64 B).
llvm::StructType *getSchedHintType(llvm::LLVMContext &Ctx);

/// Create (or reuse) the module-level TLS global with symbol name
/// "__sched_hint.data". The period in the name makes it illegal to define in
/// C/C++ source code, preventing user-space interference while maintaining
/// link-time consistency.
llvm::GlobalVariable *getOrCreateSchedHintGV(llvm::Module &M);

/// Emit a module constructor that calls prctl(PR_SET_SCHED_HINT_OFFSET, ...)
/// to register the TLS offset with the kernel.
void emitPrctlConstructor(llvm::Module &M, llvm::GlobalVariable *HintGV);

/// Map a label type string to its corresponding struct field index.
/// Returns the field index and a boolean indicating if bloom filter is needed.
/// Returns {0, false} for unknown types (will print a warning).
std::pair<unsigned, bool> getLabelTypeFieldIndex(llvm::StringRef LabelType);

/// Generic helper: emit a store to a tag payload field.
///
///   store i8 <FieldValue>, &hint.<FieldIndex>
///
/// The payload value itself serves as the activity indicator (non-zero =
/// active).
void emitFieldStore(llvm::IRBuilder<> &Builder, llvm::GlobalVariable *GV,
                    unsigned FieldIndex, uint8_t FieldValue);


/// Emit a bloom-filter hash of the given base pointers and store the result
/// into the specified magic field (i64).  Each pointer is hashed to K_BLOOM_BITS
/// bit-positions in a 64-bit word; results are OR'd together (idempotent).
///
/// @p FieldIdx — target field index (FIELD_ATOMIC_MAGIC or FIELD_UNSHARED_MAGIC)
///
/// If BasePointers is empty, stores 0 (unknown — scheduler should not co-locate).
void emitBloomMagicStore(::llvm::IRBuilder<> &Builder,
                         ::llvm::GlobalVariable *GV,
                         ::llvm::ArrayRef<::llvm::Value *> BasePointers,
                         unsigned FieldIdx = FIELD_ATOMIC_MAGIC);

/// Resolve a Value to one that dominates the given InsertionPoint.
///
/// For loop-level bloom filter computation, base pointers of atomic
/// instructions may be defined inside the loop body (e.g. PHI nodes for
/// linked-list traversal).  These values do not dominate the preheader
/// where the bloom-filter store is emitted, violating SSA dominance.
///
/// This function resolves such values:
///   - Arguments / Constants / Globals → returned as-is (always dominate).
///   - Instructions that already dominate InsertionPoint → returned as-is.
///   - PHI nodes at the loop header → the incoming value from the preheader
///     is returned (the "initial" pointer that starts the traversal).
///   - All other loop-internal values → nullptr (caller should skip).
///
/// Returns the resolved Value, or nullptr if it cannot be made available
/// at the insertion point.
::llvm::Value *resolveToPreheaderValue(::llvm::Value *V,
                                       const ::llvm::DominatorTree &DT,
                                       ::llvm::BasicBlock *InsertionPoint);

/// Collect unique base pointers from all atomic RMW/CAS instructions in a
/// set of basic blocks.  Results are deduplicated by SSA Value identity.
///
/// When DT and InsertionPoint are provided (loop-level collection),
/// each base pointer is resolved through resolveToPreheaderValue() to ensure
/// it dominates the bloom-filter insertion point.  Unresolvable pointers are
/// silently skipped (the bloom filter will store 0 = "no info" if all are
/// filtered out).
::llvm::SmallVector<::llvm::Value *, 4>
collectBasePointers(::llvm::ArrayRef<::llvm::BasicBlock *> BBs,
                    const ::llvm::DominatorTree *DT = nullptr,
                    ::llvm::BasicBlock *InsertionPoint = nullptr);

} // namespace sched_tag

#endif // SCHED_TAG_COMMON_H
