#ifndef EXEC_DENSE_ANALYSIS_H
#define EXEC_DENSE_ANALYSIS_H

//===----------------------------------------------------------------------===//
// ExecDenseAnalysis.h — Per-function execution-density analysis
//
// Classifies instructions as INT / FLOAT / SIMD / CTRL / NONE, then
// identifies exec-dense loops and basic blocks that exceed configurable
// thresholds. Covers both arithmetic pressure (INT/FLOAT/SIMD) and
// control-flow pressure (CTRL, formerly the standalone branch-dense tag).
//===----------------------------------------------------------------------===//

#include "SchedTagCommon.h"
#include "llvm/IR/Analysis.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"

namespace sched_tag {

//===----------------------------------------------------------------------===//
// Per-instruction classification
//===----------------------------------------------------------------------===//

enum class ExecOpType {
  INT,
  FLOAT,
  SIMD,
  CTRL,
  NONE,
};

/// Classify a single LLVM IR instruction's execution type.
/// CTRL covers control-flow pressure: conditional branches, switch, and
/// indirect branches (unconditional br is straight-line flow, not a
/// decision, and is not counted).
ExecOpType execOpType(llvm::Instruction &I);

//===----------------------------------------------------------------------===//
// ExecDense — per-function density analysis pass
//===----------------------------------------------------------------------===//

/// Produces a DensityResult describing all exec-dense loops and BBs.
///
/// Algorithm:
///   1. Single pass — cache per-BB instruction counts.
///   2. Loop-level — aggregate per-BB counts; check >= LOOP_DENSE_THRESHOLD
///      for arithmetic bits and >= CTRL_DENSE_THRESHOLD for the CTRL bit.
///   3. BB-level fallback — dense BBs not inside any dense loop.
struct ExecDense : public llvm::AnalysisInfoMixin<ExecDense> {
public:
  using Result = DensityResult;
  Result run(llvm::Function &F, llvm::FunctionAnalysisManager &);

private:
  static llvm::AnalysisKey Key;
  friend struct llvm::AnalysisInfoMixin<ExecDense>;
};

} // namespace sched_tag

#endif // EXEC_DENSE_ANALYSIS_H
