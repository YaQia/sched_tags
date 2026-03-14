#ifndef SCHED_TAG_PASS_H
#define SCHED_TAG_PASS_H

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Analysis.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"

namespace sched_tag {

/// Classification of a single instruction's compute type.
enum class ComputeOpType {
  INT,
  FLOAT,
  SIMD,
  NONE,
};

/// Classify a single instruction's compute type.
ComputeOpType computeOpType(llvm::Instruction &I);

//===----------------------------------------------------------------------===//
// Analysis result types — the "instrumentation plan"
//===----------------------------------------------------------------------===//

/// A dense loop that should be instrumented at loop boundaries.
struct LoopRegion {
  llvm::BasicBlock *Preheader;                       // SET here
  llvm::SmallVector<llvm::BasicBlock *, 4> ExitBlocks; // CLR here
  uint8_t TypeMask;  // bitmask of SCHED_COMPUTE_* bits
};

/// A standalone dense BB (not covered by any dense loop).
struct BBRegion {
  llvm::BasicBlock *BB;
  uint8_t TypeMask;  // bitmask of SCHED_COMPUTE_* bits
};

/// Per-function instrumentation plan produced by ComputeDense analysis.
struct DensityResult {
  llvm::SmallVector<LoopRegion, 4> Loops;
  llvm::SmallVector<BBRegion, 8> StandaloneBBs;

  bool empty() const { return Loops.empty() && StandaloneBBs.empty(); }
};

//===----------------------------------------------------------------------===//
// ComputeDense — unified per-function density analysis
//===----------------------------------------------------------------------===//

/// Per-function analysis pass.
///
/// Single traversal over every instruction, then:
///   1. Aggregate cached per-BB counts into per-loop density checks
///      (outermost-first, skip sub-loops covered by a dense ancestor).
///   2. Any dense BBs not covered by a dense loop become standalone entries.
///
/// Result is a DensityResult ready for the instrumentation pass to consume.
struct ComputeDense : public llvm::AnalysisInfoMixin<ComputeDense> {
public:
  using Result = DensityResult;
  Result run(llvm::Function &F, llvm::FunctionAnalysisManager &);

private:
  static llvm::AnalysisKey Key;
  friend struct llvm::AnalysisInfoMixin<ComputeDense>;
};

//===----------------------------------------------------------------------===//
// SchedTagPass — instrumentation (Module pass)
//===----------------------------------------------------------------------===//

/// Module pass: reads ComputeDense results and inserts runtime tag stores.
///
/// Instrumentation granularity (in priority order):
///   1. Loop-level — SET at preheader, CLR at each exit block.
///   2. BB-level fallback — SET at BB entry, CLR before the terminator.
struct SchedTagPass : public llvm::PassInfoMixin<SchedTagPass> {
  llvm::PreservedAnalyses run(llvm::Module &M,
                              llvm::ModuleAnalysisManager &AM);
};

} // namespace sched_tag

#endif // SCHED_TAG_PASS_H
