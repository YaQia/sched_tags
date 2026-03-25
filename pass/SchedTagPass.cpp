#include "SchedTagPass.h"
#include "AtomicDenseAnalysis.h"
#include "ComputeDenseAnalysis.h"
#include "SchedTagCommon.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace sched_tag {

//===----------------------------------------------------------------------===//
// Logging helpers
//===----------------------------------------------------------------------===//

static std::string computeMaskName(uint8_t Mask) {
  if (Mask == SCHED_COMPUTE_NONE)
    return "NONE";
  std::string S;
  if (Mask & SCHED_COMPUTE_INT) {
    if (!S.empty())
      S += "|";
    S += "INT";
  }
  if (Mask & SCHED_COMPUTE_FLOAT) {
    if (!S.empty())
      S += "|";
    S += "FLOAT";
  }
  if (Mask & SCHED_COMPUTE_SIMD) {
    if (!S.empty())
      S += "|";
    S += "SIMD";
  }
  return S;
}

//===----------------------------------------------------------------------===//
// Instrumentation helpers
//===----------------------------------------------------------------------===//

/// Instrument all regions in a DensityResult by emitting SET stores.
///
/// @p TagName        — human-readable tag name for diagnostics
/// @p FieldIdx       — struct field index for the tag payload
/// @p EmitBloomMagic — if true, also emit bloom-filter hash stores to
///                     atomic_magic using each region's BasePointers
/// @p NameFn         — optional callback to pretty-print the TypeMask
///
/// Returns the number of SET stores inserted.
/// Note: Tag clearing is handled by the kernel scheduler on context switch,
///       not by instrumented code.
struct InstrStats {
  unsigned LoopSets = 0;
  unsigned BBSets = 0;
};

static InstrStats
instrumentRegions(Function &F, DensityResult &Plan, GlobalVariable *HintGV,
                  const char *TagName, unsigned FieldIdx,
                  bool EmitBloomMagic = false,
                  std::function<std::string(uint8_t)> NameFn = nullptr) {

  InstrStats Stats;

  // --- Loop-level instrumentation ---
  for (auto &LR : Plan.Loops) {
    // SET before the branch into the loop header.
    {
      IRBuilder<> Builder(LR.Preheader->getTerminator());
      emitFieldStore(Builder, HintGV, FieldIdx, LR.TypeMask);
      if (EmitBloomMagic)
        emitBloomMagicStore(Builder, HintGV, LR.BasePointers);
    }
    Stats.LoopSets++;

    std::string ValStr =
        NameFn ? NameFn(LR.TypeMask) : std::to_string(LR.TypeMask);
    errs() << "[SchedTag]   LOOP SET  " << F.getName() << "::preheader("
           << LR.Preheader->getName() << ") -> " << TagName << " " << ValStr
           << " (bases=" << LR.BasePointers.size() << ")\n";
  }

  // --- BB-level instrumentation ---
  for (auto &BR : Plan.StandaloneBBs) {
    // SET at BB entry (after PHI nodes).
    {
      IRBuilder<> Builder(&*BR.BB->getFirstNonPHIOrDbg());
      emitFieldStore(Builder, HintGV, FieldIdx, BR.TypeMask);
      if (EmitBloomMagic)
        emitBloomMagicStore(Builder, HintGV, BR.BasePointers);
    }
    Stats.BBSets++;

    std::string ValStr =
        NameFn ? NameFn(BR.TypeMask) : std::to_string(BR.TypeMask);
    errs() << "[SchedTag]   BB   SET  " << F.getName()
           << "::" << BR.BB->getName() << " -> " << TagName << " " << ValStr
           << " (bases=" << BR.BasePointers.size() << ")\n";
  }

  return Stats;
}

//===----------------------------------------------------------------------===//
// SchedTagPass::run — pure instrumentation, no analysis
//===----------------------------------------------------------------------===//

PreservedAnalyses SchedTagPass::run(Module &M, ModuleAnalysisManager &MAM) {
  auto &FAM = MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();

  // ---- Collect plans from all functions (read-only phase) ----

  struct FuncPlans {
    Function *F;
    DensityResult Compute;
    DensityResult Atomic;
  };
  SmallVector<FuncPlans, 8> AllPlans;

  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    auto ComputePlan = FAM.getResult<ComputeDense>(F);
    auto AtomicPlan = FAM.getResult<AtomicDense>(F);
    if (!ComputePlan.empty() || !AtomicPlan.empty())
      AllPlans.push_back({&F, std::move(ComputePlan), std::move(AtomicPlan)});
  }

  if (AllPlans.empty()) {
    errs() << "[SchedTag] no dense regions found, skipping.\n";
    return PreservedAnalyses::all();
  }

  // ---- Create the global variable and prctl constructor ----
  GlobalVariable *HintGV = getOrCreateSchedHintGV(M);
  emitPrctlConstructor(M, HintGV);

  // ---- Instrument ----
  InstrStats TotalCompute{}, TotalAtomic{};

  for (auto &[FuncPtr, ComputePlan, AtomicPlan] : AllPlans) {
    Function &F = *FuncPtr;

    if (!ComputePlan.empty()) {
      auto S = instrumentRegions(F, ComputePlan, HintGV, "COMPUTE",
                                 FIELD_COMPUTE_DENSE, false, computeMaskName);
      TotalCompute.LoopSets += S.LoopSets;
      TotalCompute.BBSets += S.BBSets;
    }

    if (!AtomicPlan.empty()) {
      auto S = instrumentRegions(F, AtomicPlan, HintGV, "ATOMIC",
                                 FIELD_ATOMIC_DENSE,
                                 /*EmitBloomMagic=*/true);
      TotalAtomic.LoopSets += S.LoopSets;
      TotalAtomic.BBSets += S.BBSets;
    }
  }

  errs() << "[SchedTag] instrumented across " << AllPlans.size()
         << " functions:\n";
  errs() << "[SchedTag]   compute: " << TotalCompute.LoopSets << " loop-SET, "
         << TotalCompute.BBSets << " bb-SET\n";
  errs() << "[SchedTag]   atomic:  " << TotalAtomic.LoopSets << " loop-SET, "
         << TotalAtomic.BBSets << " bb-SET\n";

  return PreservedAnalyses::none();
}

} // namespace sched_tag

//===----------------------------------------------------------------------===//
// New PM plugin registration
//===----------------------------------------------------------------------===//

llvm::PassPluginLibraryInfo getSchedTagPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "SchedTag", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            // Manual: opt -passes=sched-tag
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "sched-tag") {
                    MPM.addPass(sched_tag::SchedTagPass());
                    return true;
                  }
                  return false;
                });

            // Automatic: run at the very end of the optimisation pipeline
            // (after Loop Vectorizer, SLP Vectorizer, and all clean-up
            // passes) so that the density analysis sees compiler-generated
            // SIMD instructions.
            PB.registerOptimizerLastEPCallback([](ModulePassManager &MPM,
                                                  OptimizationLevel Level,
                                                  ThinOrFullLTOPhase) {
              if (Level != OptimizationLevel::O0)
                MPM.addPass(sched_tag::SchedTagPass());
            });

            // Register both per-function analyses.
            PB.registerAnalysisRegistrationCallback(
                [](FunctionAnalysisManager &FAM) {
                  FAM.registerPass([&] { return sched_tag::ComputeDense(); });
                  FAM.registerPass([&] { return sched_tag::AtomicDense(); });
                });
          }};
}

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getSchedTagPluginInfo();
}
