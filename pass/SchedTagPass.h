#ifndef SCHED_TAG_PASS_H
#define SCHED_TAG_PASS_H

#include "llvm/ADT/MapVector.h"
#include "llvm/IR/Analysis.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"

namespace sched_tag {

struct ComputeDense : public llvm::AnalysisInfoMixin<ComputeDense> {
public:
  enum ComputeDenseType {
    INT,
    FLOAT,
    SIMD,
    NONE,
  };
  using Result = llvm::MapVector<llvm::BasicBlock const *, ComputeDenseType>;
  Result run(llvm::Function &F, llvm::FunctionAnalysisManager &);

private:
  static llvm::AnalysisKey Key;
  friend struct llvm::AnalysisInfoMixin<ComputeDense>;
};

struct SchedTagPass : public llvm::PassInfoMixin<SchedTagPass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);
};

} // namespace sched_tag

#endif // SCHED_TAG_PASS_H
