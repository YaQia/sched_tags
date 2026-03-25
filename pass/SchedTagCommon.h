#ifndef SCHED_TAG_COMMON_H
#define SCHED_TAG_COMMON_H

//===----------------------------------------------------------------------===//
// SchedTagCommon.h — Shared constants, result types, and utility declarations
//                    used by all sched-tag analysis and instrumentation passes.
//===----------------------------------------------------------------------===//

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"

namespace sched_tag {

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
static constexpr unsigned FIELD_COMPUTE_DENSE = 2;
static constexpr unsigned FIELD_ATOMIC_DENSE = 5;
static constexpr unsigned FIELD_ATOMIC_MAGIC = 10;

// Bloom filter parameters for atomic_magic (register-width bloom filter).
// Each pointer address sets K_BLOOM_BITS bit-positions in a 64-bit word.
// The scheduler detects overlap via: popcount(magic_a & magic_b) >= K_BLOOM_BITS.
static constexpr unsigned K_BLOOM_BITS = 4;
static constexpr uint64_t BLOOM_HASH_PRIME = 0x9E3779B97F4A7C15ULL; // fibonacci

//===----------------------------------------------------------------------===//
// Analysis result types — the "instrumentation plan"
//===----------------------------------------------------------------------===//

/// A dense loop that should be instrumented at loop boundaries.
struct LoopRegion {
  llvm::BasicBlock *Preheader;                           // SET here
  llvm::SmallVector<llvm::BasicBlock *, 4> ExitBlocks;   // (unused; CLR now handled by kernel scheduler)
  uint8_t TypeMask;  // value to store in the tag payload field

  /// Base pointers (stripped of GEP/bitcasts) of atomic RMW/CAS instructions
  /// in this region.  Used to compute the bloom-filter atomic_magic at the
  /// SET boundary.  Empty for non-atomic analyses.
  llvm::SmallVector<llvm::Value *, 4> BasePointers;
};

/// A standalone dense BB (not covered by any dense loop).
struct BBRegion {
  llvm::BasicBlock *BB;
  uint8_t TypeMask;  // value to store in the tag payload field

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

/// Create (or reuse) the module-level TLS global with symbol name "__sched_hint.data".
/// The period in the name makes it illegal to define in C/C++ source code,
/// preventing user-space interference while maintaining link-time consistency.
llvm::GlobalVariable *getOrCreateSchedHintGV(llvm::Module &M);

/// Emit a module constructor that calls prctl(PR_SET_SCHED_HINT_OFFSET, ...)
/// to register the TLS offset with the kernel.
void emitPrctlConstructor(llvm::Module &M, llvm::GlobalVariable *HintGV);

/// Generic helper: emit a store to a tag payload field.
///
///   store i8 <FieldValue>, &hint.<FieldIndex>
///
/// The payload value itself serves as the activity indicator (non-zero = active).
void emitFieldStore(llvm::IRBuilder<> &Builder, llvm::GlobalVariable *GV,
                    unsigned FieldIndex, uint8_t FieldValue);

/// Emit a bloom-filter hash of the given base pointers and store the result
/// into the atomic_magic field (i64).  Each pointer is hashed to K_BLOOM_BITS
/// bit-positions in a 64-bit word; results are OR'd together (idempotent).
///
/// If BasePointers is empty, stores 0 (unknown — scheduler should not co-locate).
void emitBloomMagicStore(llvm::IRBuilder<> &Builder, llvm::GlobalVariable *GV,
                         llvm::ArrayRef<llvm::Value *> BasePointers);

} // namespace sched_tag

#endif // SCHED_TAG_COMMON_H
