#ifndef SCHED_TAG_PASS_H
#define SCHED_TAG_PASS_H

//===----------------------------------------------------------------------===//
// SchedTagPass.h — Module pass that orchestrates all sched-tag analyses
//                  and inserts runtime hint stores.
//
// This pass:
//   1. Runs per-function density analyses (ComputeDense, AtomicDense, ...).
//   2. Creates the TLS global @__sched_hint_data (magic + version header).
//   3. Emits a module constructor to register the TLS offset via prctl().
//   4. Instruments dense region boundaries with SET / CLR stores to
//      the corresponding payload fields (non-zero = active).
//===----------------------------------------------------------------------===//

#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"

namespace sched_tag {

struct SchedTagPass : public llvm::PassInfoMixin<SchedTagPass> {
  llvm::PreservedAnalyses run(llvm::Module &M,
                              llvm::ModuleAnalysisManager &AM);
};

} // namespace sched_tag

#endif // SCHED_TAG_PASS_H
