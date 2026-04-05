#include "SchedTagPass.h"
#include "AtomicDenseAnalysis.h"
#include "ComputeDenseAnalysis.h"
#include "SchedTagCommon.h"
#include "SourceLabelAnalysis.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

//===----------------------------------------------------------------------===//
// Command-line options
//===----------------------------------------------------------------------===//

static cl::opt<std::string> SchedTagsFile(
    "sched-tags-file",
    cl::desc("Path to sched_tags.json configuration file"),
    cl::init("sched_tags.json"),
    cl::Hidden);

static cl::opt<bool> SchedAutoAnalysis(
    "sched-auto-analysis",
    cl::desc("Enable automatic static analysis (ComputeDense/AtomicDense)"),
    cl::init(true),
    cl::Hidden);

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

/// Remove regions from Plan that overlap with regions already marked by
/// SourcePlan. This prevents duplicate instrumentation when source labels
/// explicitly mark a region.
///
/// Priority: SourceLabel > AutoAnalysis (Compute/Atomic)
static void deduplicateRegions(DensityResult &Plan,
                                const DensityResult &SourcePlan) {
  // Build a set of preheaders/BBs already marked by source labels
  llvm::SmallPtrSet<llvm::BasicBlock *, 16> MarkedBBs;

  // Add all loop preheaders from SourcePlan
  for (const auto &LR : SourcePlan.Loops) {
    if (LR.Preheader)
      MarkedBBs.insert(LR.Preheader);
  }

  // Add all standalone BBs from SourcePlan
  for (const auto &BR : SourcePlan.StandaloneBBs) {
    MarkedBBs.insert(BR.BB);
  }

  if (MarkedBBs.empty())
    return; // Nothing to deduplicate

  // Filter out loops whose preheader is already marked
  Plan.Loops.erase(
      llvm::remove_if(Plan.Loops,
                      [&](const LoopRegion &LR) {
                        return LR.Preheader && MarkedBBs.count(LR.Preheader);
                      }),
      Plan.Loops.end());

  // Filter out standalone BBs that are already marked
  Plan.StandaloneBBs.erase(
      llvm::remove_if(Plan.StandaloneBBs,
                      [&](const BBRegion &BR) {
                        return MarkedBBs.count(BR.BB);
                      }),
      Plan.StandaloneBBs.end());
}

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

  // ---- Load source labels from sched_tags.json ----
  auto SourceLabels = parseSchedTagsJSON(SchedTagsFile);
  size_t NumLabels = SourceLabels.size();
  if (!SourceLabels.empty()) {
    SourceLabelAnalysis::setLabels(std::move(SourceLabels));
    errs() << "[SchedTag] loaded " << NumLabels
           << " source label(s) from " << SchedTagsFile << "\n";
  }

  if (!SchedAutoAnalysis) {
    errs() << "[SchedTag] auto-analysis disabled, using source labels only\n";
  }

  // ---- Collect plans from all functions (read-only phase) ----

  struct FuncPlans {
    Function *F;
    DensityResult Compute;
    DensityResult Atomic;
    SourceLabelResults Source;
  };
  SmallVector<FuncPlans, 8> AllPlans;

  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    DensityResult ComputePlan, AtomicPlan;
    if (SchedAutoAnalysis) {
      ComputePlan = FAM.getResult<ComputeDense>(F);
      AtomicPlan = FAM.getResult<AtomicDense>(F);
    }
    auto SourcePlan = FAM.getResult<SourceLabelAnalysis>(F);
    if (!ComputePlan.empty() || !AtomicPlan.empty() || !SourcePlan.empty())
      AllPlans.push_back({&F, std::move(ComputePlan), std::move(AtomicPlan),
                          std::move(SourcePlan)});
  }

  if (AllPlans.empty()) {
    errs() << "[SchedTag] no dense regions found, skipping.\n";
    return PreservedAnalyses::all();
  }

  // ---- Create the global variable and prctl constructor ----
  GlobalVariable *HintGV = getOrCreateSchedHintGV(M);
  emitPrctlConstructor(M, HintGV);

  // ---- Deduplicate and instrument ----
  InstrStats TotalCompute{}, TotalAtomic{}, TotalSource{};
  unsigned DeduplicatedLoops = 0, DeduplicatedBBs = 0;

  for (auto &[FuncPtr, ComputePlan, AtomicPlan, SourcePlan] : AllPlans) {
    Function &F = *FuncPtr;

    // First, instrument source labels (highest priority)
    // Process each label separately to preserve type information
    for (const auto &SourceLabel : SourcePlan.Labels) {
      // Get the field index and bloom filter flag for this label type
      auto [FieldIndex, NeedsBloom] = getLabelTypeFieldIndex(SourceLabel.LabelType);
      
      // Make a copy to pass as non-const reference
      DensityResult RegionsCopy = SourceLabel.Regions;
      auto S = instrumentRegions(F, RegionsCopy, HintGV, "SOURCE",
                                 FieldIndex, NeedsBloom);
      TotalSource.LoopSets += S.LoopSets;
      TotalSource.BBSets += S.BBSets;
    }

    // Build a combined DensityResult from all source labels for deduplication
    DensityResult CombinedSourcePlan;
    for (const auto &SourceLabel : SourcePlan.Labels) {
      CombinedSourcePlan.Loops.append(SourceLabel.Regions.Loops.begin(),
                                      SourceLabel.Regions.Loops.end());
      CombinedSourcePlan.StandaloneBBs.append(
          SourceLabel.Regions.StandaloneBBs.begin(),
          SourceLabel.Regions.StandaloneBBs.end());
    }

    // Then deduplicate automatic analyses against source labels
    size_t ComputeLoopsBefore = ComputePlan.Loops.size();
    size_t ComputeBBsBefore = ComputePlan.StandaloneBBs.size();
    size_t AtomicLoopsBefore = AtomicPlan.Loops.size();
    size_t AtomicBBsBefore = AtomicPlan.StandaloneBBs.size();

    deduplicateRegions(ComputePlan, CombinedSourcePlan);
    deduplicateRegions(AtomicPlan, CombinedSourcePlan);

    size_t ComputeLoopsAfter = ComputePlan.Loops.size();
    size_t ComputeBBsAfter = ComputePlan.StandaloneBBs.size();
    size_t AtomicLoopsAfter = AtomicPlan.Loops.size();
    size_t AtomicBBsAfter = AtomicPlan.StandaloneBBs.size();

    DeduplicatedLoops += (ComputeLoopsBefore - ComputeLoopsAfter) +
                         (AtomicLoopsBefore - AtomicLoopsAfter);
    DeduplicatedBBs += (ComputeBBsBefore - ComputeBBsAfter) +
                       (AtomicBBsBefore - AtomicBBsAfter);

    // Instrument remaining regions
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
  errs() << "[SchedTag]   source:  " << TotalSource.LoopSets << " loop-SET, "
         << TotalSource.BBSets << " bb-SET\n";
  if (DeduplicatedLoops > 0 || DeduplicatedBBs > 0) {
    errs() << "[SchedTag]   deduplicated: " << DeduplicatedLoops
           << " loops, " << DeduplicatedBBs
           << " BBs (source labels take priority)\n";
  }

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

            // Register all per-function analyses.
            PB.registerAnalysisRegistrationCallback(
                [](FunctionAnalysisManager &FAM) {
                  FAM.registerPass([&] { return sched_tag::ComputeDense(); });
                  FAM.registerPass([&] { return sched_tag::AtomicDense(); });
                  FAM.registerPass([&] { return sched_tag::SourceLabelAnalysis(); });
                });
          }};
}

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getSchedTagPluginInfo();
}
