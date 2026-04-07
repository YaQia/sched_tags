#ifndef ATOMIC_DENSE_ANALYSIS_H
#define ATOMIC_DENSE_ANALYSIS_H

//===----------------------------------------------------------------------===//
// AtomicDenseAnalysis.h — Per-function atomic-instruction density analysis
//
// Identifies regions (loops and basic blocks) with a high concentration of
// atomic RMW/CAS instructions (the MESI-traffic-generating subset).
// Also collects base pointers for bloom-filter magic computation.
// The analysis follows the same loop-first, BB-fallback strategy used by
// ComputeDense.
//===----------------------------------------------------------------------===//

#include "SchedTagCommon.h"
#include "llvm/IR/Analysis.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"

namespace sched_tag {

//===----------------------------------------------------------------------===//
// Per-instruction classification
//===----------------------------------------------------------------------===//

/// Return true if the instruction is an atomic read-modify-write or CAS.
///
/// Only counts instructions that generate MESI coherence traffic
/// (require Exclusive cache line state under contention):
///   - AtomicRMWInst  (fetch_add, fetch_sub, xchg, ...)
///   - AtomicCmpXchgInst
bool isAtomicOp(const llvm::Instruction &I);

//===----------------------------------------------------------------------===//
// AtomicDense — per-function density analysis pass
//===----------------------------------------------------------------------===//

/// Produces a DensityResult describing all atomic-dense loops and BBs.
/// Value in the result is always 1 (atomic_dense is a boolean field).
struct AtomicDense : public llvm::AnalysisInfoMixin<AtomicDense> {
public:
  using Result = DensityResult;
  Result run(llvm::Function &F, llvm::FunctionAnalysisManager &);

private:
  static llvm::AnalysisKey Key;
  friend struct llvm::AnalysisInfoMixin<AtomicDense>;
};

} // namespace sched_tag

#endif // ATOMIC_DENSE_ANALYSIS_H
