#ifndef SCHED_TAG_PASS_H
#define SCHED_TAG_PASS_H

//===----------------------------------------------------------------------===//
// SchedTagPass.h — Module pass that orchestrates all sched-tag analyses
//                  and inserts hint stores.
//
// This pass:
//   1. Runs per-function density analyses (ExecDense, AtomicDense, ...).
//   2. Declares the sched_hint library symbols its instrumented code references
//      (the per-thread slot cache and registrar, defined in libsched_tag).
//   3. Instruments dense region boundaries with SET / CLR stores into the
//      current thread's kernel slot (non-zero = active). The hot path is
//      hand-inlined; only the cold registration is an out-of-line call.
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
