#ifndef COMPUTE_DENSE_ANALYSIS_H
#define COMPUTE_DENSE_ANALYSIS_H

//===----------------------------------------------------------------------===//
// ComputeDenseAnalysis.h — Per-function compute-density analysis
//
// Classifies instructions as INT / FLOAT / SIMD / NONE, then identifies
// compute-dense loops and basic blocks that exceed configurable thresholds.
//===----------------------------------------------------------------------===//

#include "SchedTagCommon.h"
#include "llvm/IR/Analysis.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"

namespace sched_tag {

//===----------------------------------------------------------------------===//
// Per-instruction classification
//===----------------------------------------------------------------------===//

enum class ComputeOpType {
  INT,
  FLOAT,
  SIMD,
  NONE,
};

/// Classify a single LLVM IR instruction's compute type.
ComputeOpType computeOpType(llvm::Instruction &I);

//===----------------------------------------------------------------------===//
// ComputeDense — per-function density analysis pass
//===----------------------------------------------------------------------===//

/// Produces a DensityResult describing all compute-dense loops and BBs.
///
/// Algorithm:
///   1. Single pass — cache per-BB instruction counts.
///   2. Loop-level — aggregate per-BB counts; check >= LOOP_DENSE_THRESHOLD.
///   3. BB-level fallback — dense BBs not inside any dense loop.
struct ComputeDense : public llvm::AnalysisInfoMixin<ComputeDense> {
public:
  using Result = DensityResult;
  Result run(llvm::Function &F, llvm::FunctionAnalysisManager &);

private:
  static llvm::AnalysisKey Key;
  friend struct llvm::AnalysisInfoMixin<ComputeDense>;
};

} // namespace sched_tag

#endif // COMPUTE_DENSE_ANALYSIS_H
