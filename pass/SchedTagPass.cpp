#include "SchedTagPass.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

using namespace llvm;

namespace sched_tag {

//===----------------------------------------------------------------------===//
// Constants — must match sched_hint.h exactly
//===----------------------------------------------------------------------===//
static constexpr uint32_t SCHED_HINT_MAGIC = 0x5348494EU;
static constexpr uint32_t SCHED_HINT_VERSION = 1;
static constexpr uint64_t SCHED_TAG_COMPUTE_DENSE = 1ULL << 0;
static constexpr uint8_t SCHED_COMPUTE_NONE = 0;
static constexpr uint8_t SCHED_COMPUTE_INT = 1;
static constexpr uint8_t SCHED_COMPUTE_FLOAT = 2;
static constexpr uint8_t SCHED_COMPUTE_SIMD = 3;
static constexpr const char *SCHED_HINT_SECTION = "__sched_hint";
static constexpr int PR_SET_SCHED_HINT_OFFSET = 83;

// Struct field indices (must match getSchedHintType layout)
static constexpr unsigned FIELD_TAGS_ACTIVE = 3;
static constexpr unsigned FIELD_COMPUTE_DENSE = 4;

// Minimum number of instructions in a BB for standalone BB-level tagging.
static constexpr unsigned MIN_BB_SIZE = 10;

// Minimum total instructions across a loop for loop-level tagging.
static constexpr unsigned MIN_LOOP_SIZE = 10;

// Density thresholds.
static constexpr double LOOP_DENSE_THRESHOLD = 0.5;
static constexpr double BB_DENSE_THRESHOLD = 0.7;

//===----------------------------------------------------------------------===//
// Per-instruction classification
//===----------------------------------------------------------------------===//

ComputeOpType computeOpType(Instruction &I) {
  // Exclude non-compute instructions.
  if (isa<LoadInst>(I) || isa<StoreInst>(I) || isa<PHINode>(I) ||
      isa<SelectInst>(I) || isa<AllocaInst>(I) || I.isTerminator() ||
      isa<CallInst>(I))
    return ComputeOpType::NONE;

  bool isIntOp = false, isFloatOp = false;
  switch (I.getOpcode()) {
  case Instruction::Add:
  case Instruction::Sub:
  case Instruction::Mul:
  case Instruction::UDiv:
  case Instruction::SDiv:
  case Instruction::URem:
  case Instruction::SRem:
  case Instruction::Shl:
  case Instruction::LShr:
  case Instruction::AShr:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
  case Instruction::ICmp:
  case Instruction::Trunc:
  case Instruction::ZExt:
  case Instruction::SExt:
  case Instruction::FPToUI:
  case Instruction::FPToSI:
  case Instruction::PtrToInt:
  case Instruction::IntToPtr:
    isIntOp = true;
    break;

  case Instruction::FNeg:
  case Instruction::FAdd:
  case Instruction::FSub:
  case Instruction::FMul:
  case Instruction::FDiv:
  case Instruction::FRem:
  case Instruction::FCmp:
  case Instruction::FPTrunc:
  case Instruction::FPExt:
  case Instruction::UIToFP:
  case Instruction::SIToFP:
    isFloatOp = true;
    break;

  case Instruction::ExtractElement:
  case Instruction::InsertElement:
  case Instruction::ShuffleVector:
    return ComputeOpType::SIMD;

  default:
    return ComputeOpType::NONE;
  }

  // Vector operands → SIMD regardless of opcode category.
  bool isVector = I.getType()->isVectorTy();
  if (!isVector) {
    for (unsigned i = 0; i < I.getNumOperands(); ++i) {
      if (I.getOperand(i)->getType()->isVectorTy()) {
        isVector = true;
        break;
      }
    }
  }
  if (isVector)
    return ComputeOpType::SIMD;

  if (isIntOp)
    return ComputeOpType::INT;
  if (isFloatOp)
    return ComputeOpType::FLOAT;
  return ComputeOpType::NONE;
}

//===----------------------------------------------------------------------===//
// Helper: classify from (IntCnt, FloatCnt, SIMDCnt, Total) + threshold
//===----------------------------------------------------------------------===//

static ComputeOpType classifyFromCounts(uint32_t IntCnt, uint32_t FloatCnt,
                                        uint32_t SIMDCnt, uint32_t Total,
                                        double Threshold) {
  if (Total == 0)
    return ComputeOpType::NONE;
  double ratio = 1.0 / Total;
  if (IntCnt * ratio >= Threshold)
    return ComputeOpType::INT;
  if (FloatCnt * ratio >= Threshold)
    return ComputeOpType::FLOAT;
  if (SIMDCnt * ratio >= Threshold)
    return ComputeOpType::SIMD;
  return ComputeOpType::NONE;
}

//===----------------------------------------------------------------------===//
// ComputeDense::run — unified density analysis
//===----------------------------------------------------------------------===//

AnalysisKey ComputeDense::Key;

ComputeDense::Result ComputeDense::run(Function &F,
                                       FunctionAnalysisManager &FAM) {
  DensityResult Plan;

  //--- Step 1: Single pass — cache per-BB instruction counts ---------------

  struct BBCounts {
    uint32_t Total = 0;
    uint32_t IntCnt = 0;
    uint32_t FloatCnt = 0;
    uint32_t SIMDCnt = 0;
  };

  DenseMap<BasicBlock *, BBCounts> BBStats;

  for (BasicBlock &BB : F) {
    BBCounts C;
    for (Instruction &I : BB) {
      C.Total++;
      switch (computeOpType(I)) {
      case ComputeOpType::INT:
        C.IntCnt++;
        break;
      case ComputeOpType::FLOAT:
        C.FloatCnt++;
        break;
      case ComputeOpType::SIMD:
        C.SIMDCnt++;
        break;
      default:
        break;
      }
    }
    BBStats[&BB] = C;
  }

  //--- Step 2: Loop-level analysis (outermost-first) -----------------------
  //
  // Aggregate the cached per-BB counts across each loop's blocks.
  // If an outer loop is dense, all its BBs (including sub-loop BBs) are
  // "covered" and won't need standalone BB-level instrumentation.

  auto &LI = FAM.getResult<LoopAnalysis>(F);

  DenseSet<BasicBlock *> CoveredByLoop;

  SmallVector<Loop *, 8> Worklist;
  for (Loop *TopL : LI)
    Worklist.push_back(TopL);

  while (!Worklist.empty()) {
    Loop *L = Worklist.pop_back_val();

    // Skip if already covered by an outer dense loop.
    bool AlreadyCovered = false;
    for (BasicBlock *BB : L->getBlocks()) {
      if (CoveredByLoop.contains(BB)) {
        AlreadyCovered = true;
        break;
      }
    }
    if (AlreadyCovered)
      continue;

    // Aggregate counts from cached per-BB stats — no re-traversal.
    uint32_t Total = 0, IntCnt = 0, FloatCnt = 0, SIMDCnt = 0;
    for (BasicBlock *BB : L->getBlocks()) {
      auto It = BBStats.find(BB);
      if (It == BBStats.end())
        continue;
      const BBCounts &C = It->second;
      Total += C.Total;
      IntCnt += C.IntCnt;
      FloatCnt += C.FloatCnt;
      SIMDCnt += C.SIMDCnt;
    }

    if (Total < MIN_LOOP_SIZE) {
      // Too small — recurse into sub-loops.
      for (Loop *Sub : L->getSubLoops())
        Worklist.push_back(Sub);
      continue;
    }

    ComputeOpType LoopType = classifyFromCounts(IntCnt, FloatCnt, SIMDCnt,
                                                Total, LOOP_DENSE_THRESHOLD);
    if (LoopType == ComputeOpType::NONE) {
      // Not dense as a whole — try sub-loops.
      for (Loop *Sub : L->getSubLoops())
        Worklist.push_back(Sub);
      continue;
    }

    // Dense loop — need a preheader to instrument.
    BasicBlock *Preheader = L->getLoopPreheader();
    if (!Preheader) {
      // Can't cleanly instrument without a preheader.  Fall through.
      for (Loop *Sub : L->getSubLoops())
        Worklist.push_back(Sub);
      continue;
    }

    // Collect and deduplicate exit blocks.
    SmallVector<BasicBlock *, 4> RawExits;
    L->getExitBlocks(RawExits);

    LoopRegion LR;
    LR.Preheader = Preheader;
    LR.Type = LoopType;

    DenseSet<BasicBlock *> Seen;
    for (BasicBlock *Exit : RawExits) {
      if (Seen.insert(Exit).second)
        LR.ExitBlocks.push_back(Exit);
    }

    Plan.Loops.push_back(std::move(LR));

    // Mark all loop BBs as covered.
    for (BasicBlock *BB : L->getBlocks())
      CoveredByLoop.insert(BB);
    // Don't recurse into sub-loops — they're covered.
  }

  //--- Step 3: BB-level fallback -------------------------------------------
  //
  // Dense BBs not inside any dense loop get standalone SET+CLR.

  for (BasicBlock &BB : F) {
    if (CoveredByLoop.contains(&BB))
      continue;

    const BBCounts &C = BBStats[&BB];
    if (C.Total < MIN_BB_SIZE)
      continue;

    ComputeOpType BBType = classifyFromCounts(C.IntCnt, C.FloatCnt, C.SIMDCnt,
                                              C.Total, BB_DENSE_THRESHOLD);
    if (BBType == ComputeOpType::NONE)
      continue;

    Plan.StandaloneBBs.push_back({&BB, BBType});
  }

  return Plan;
}

//===----------------------------------------------------------------------===//
// getSchedHintType — LLVM struct matching sched_hint.h (64 bytes)
//===----------------------------------------------------------------------===//

static StructType *getSchedHintType(LLVMContext &Ctx) {
  StructType *Ty = StructType::getTypeByName(Ctx, "struct.sched_hint");
  if (Ty)
    return Ty;

  auto *I8 = Type::getInt8Ty(Ctx);
  auto *I32 = Type::getInt32Ty(Ctx);
  auto *I64 = Type::getInt64Ty(Ctx);

  Ty = StructType::create(Ctx,
                          {
                              I32,                   //  [0] magic
                              I32,                   //  [1] version
                              I64,                   //  [2] tags_present
                              I64,                   //  [3] tags_active
                              I8,                    //  [4] compute_dense
                              I8,                    //  [5] branch_dense
                              I8,                    //  [6] memory_dense
                              I8,                    //  [7] atomic_dense
                              I8,                    //  [8] io_dense
                              I8,                    //  [9] unshared
                              I8,                    // [10] compute_prep
                              I8,                    // [11] reserved_pad
                              I64,                   // [12] atomic_magic
                              I64,                   // [13] dep_magic
                              I8,                    // [14] dep_role
                              ArrayType::get(I8, 7), // [15] reserved1[7]
                              ArrayType::get(I8, 8), // [16] reserved2[8]
                          },
                          "struct.sched_hint",
                          /*isPacked=*/false);

  return Ty;
}

//===----------------------------------------------------------------------===//
// getOrCreateSchedHintGV
//===----------------------------------------------------------------------===//

static GlobalVariable *getOrCreateSchedHintGV(Module &M) {
  if (auto *Existing = M.getNamedGlobal("__sched_hint_data"))
    return Existing;

  LLVMContext &Ctx = M.getContext();
  StructType *HintTy = getSchedHintType(Ctx);
  auto *I8 = Type::getInt8Ty(Ctx);
  auto *I32 = Type::getInt32Ty(Ctx);
  auto *I64 = Type::getInt64Ty(Ctx);

  Constant *Init = ConstantStruct::get(
      HintTy,
      {
          ConstantInt::get(I32, SCHED_HINT_MAGIC),
          ConstantInt::get(I32, SCHED_HINT_VERSION),
          ConstantInt::get(I64, SCHED_TAG_COMPUTE_DENSE), // tags_present
          ConstantInt::get(I64, 0),                       // tags_active
          ConstantInt::get(I8, SCHED_COMPUTE_NONE),
          ConstantInt::get(I8, 0),
          ConstantInt::get(I8, 0),
          ConstantInt::get(I8, 0),
          ConstantInt::get(I8, 0),
          ConstantInt::get(I8, 0),
          ConstantInt::get(I8, 0),
          ConstantInt::get(I8, 0),
          ConstantInt::get(I64, 0),
          ConstantInt::get(I64, 0),
          ConstantInt::get(I8, 0),
          ConstantAggregateZero::get(ArrayType::get(I8, 7)),
          ConstantAggregateZero::get(ArrayType::get(I8, 8)),
      });

  auto *GV = new GlobalVariable(M, HintTy, /*isConstant=*/false,
                                GlobalValue::ExternalLinkage, Init,
                                "__sched_hint_data");

  GV->setThreadLocalMode(GlobalValue::InitialExecTLSModel);
  GV->setSection(SCHED_HINT_SECTION);
  GV->setAlignment(Align(64));
  appendToUsed(M, {GV});
  return GV;
}

//===----------------------------------------------------------------------===//
// emitPrctlConstructor — register TLS offset with the kernel via prctl
//===----------------------------------------------------------------------===//
//
// Generates a module constructor equivalent to:
//
//   __attribute__((constructor))
//   void __sched_hint_report(void) {
//       void *vaddr = &__sched_hint_data;
//       void *tp    = __builtin_thread_pointer();
//       long offset = (long)vaddr - (long)tp;
//       prctl(PR_SET_SCHED_HINT_OFFSET, offset, vaddr);
//   }
//
// The kernel uses `offset` for future child threads (TP + offset → hint addr)
// and `vaddr` directly for the calling (main) thread.

static void emitPrctlConstructor(Module &M, GlobalVariable *HintGV) {
  LLVMContext &Ctx = M.getContext();
  auto *I32 = Type::getInt32Ty(Ctx);
  auto *I64 = Type::getInt64Ty(Ctx);

  // void @__sched_hint_report()
  FunctionType *CtorTy = FunctionType::get(Type::getVoidTy(Ctx), false);
  Function *Ctor = Function::Create(CtorTy, GlobalValue::InternalLinkage,
                                    "__sched_hint_report", M);

  BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", Ctor);
  IRBuilder<> Builder(Entry);

  // %vaddr = ptrtoint ptr @__sched_hint_data to i64
  //   (TLS address for the current / main thread)
  Value *VAddr = Builder.CreatePtrToInt(HintGV, I64, "vaddr");

  // %tp = call ptr @llvm.thread.pointer.p0()
  //   Overloaded intrinsic in LLVM 21+ (llvm_anyptr_ty), needs type arg.
  Type *PtrTy = PointerType::getUnqual(Ctx);
  Function *ThreadPtr =
      Intrinsic::getOrInsertDeclaration(&M, Intrinsic::thread_pointer, {PtrTy});
  Value *TP = Builder.CreateCall(ThreadPtr, {}, "tp");
  Value *TPInt = Builder.CreatePtrToInt(TP, I64, "tp.int");

  // %offset = sub i64 %vaddr, %tp.int
  Value *Offset = Builder.CreateSub(VAddr, TPInt, "offset");

  // Declare: int prctl(int option, ...)
  FunctionType *PrctlTy = FunctionType::get(I32, {I32}, /*isVarArg=*/true);
  FunctionCallee Prctl = M.getOrInsertFunction("prctl", PrctlTy);

  // call i32 (i32, ...) @prctl(i32 83, i64 %offset, i64 %vaddr)
  Builder.CreateCall(
      Prctl, {ConstantInt::get(I32, PR_SET_SCHED_HINT_OFFSET), Offset, VAddr});

  Builder.CreateRetVoid();

  // Append to @llvm.global_ctors (priority 65535 = default).
  appendToGlobalCtors(M, Ctor, /*Priority=*/65535);

  errs() << "[SchedTag] emitted prctl constructor __sched_hint_report()\n";
}

//===----------------------------------------------------------------------===//
// Instrumentation helpers
//===----------------------------------------------------------------------===//

static uint8_t denseTypeToHintConst(ComputeOpType T) {
  switch (T) {
  case ComputeOpType::INT:
    return SCHED_COMPUTE_INT;
  case ComputeOpType::FLOAT:
    return SCHED_COMPUTE_FLOAT;
  case ComputeOpType::SIMD:
    return SCHED_COMPUTE_SIMD;
  default:
    return SCHED_COMPUTE_NONE;
  }
}

static const char *hintConstName(uint8_t C) {
  switch (C) {
  case SCHED_COMPUTE_INT:
    return "INT";
  case SCHED_COMPUTE_FLOAT:
    return "FLOAT";
  case SCHED_COMPUTE_SIMD:
    return "SIMD";
  default:
    return "NONE";
  }
}

/// Emit tag stores at the current IRBuilder insertion point:
///   store i8 <type>, &hint.compute_dense
///   old = load i64, &hint.tags_active
///   new = old | bit   (or  old & ~bit  for NONE)
///   store i64 new, &hint.tags_active
static void emitTagStores(IRBuilder<> &Builder, GlobalVariable *GV,
                          uint8_t ComputeType) {
  LLVMContext &Ctx = Builder.getContext();
  StructType *HintTy = getSchedHintType(Ctx);

  Value *ComputePtr = Builder.CreateStructGEP(HintTy, GV, FIELD_COMPUTE_DENSE,
                                              "hint.compute.ptr");
  Builder.CreateStore(ConstantInt::get(Type::getInt8Ty(Ctx), ComputeType),
                      ComputePtr);

  Value *TagsPtr =
      Builder.CreateStructGEP(HintTy, GV, FIELD_TAGS_ACTIVE, "hint.tags.ptr");
  LoadInst *OldTags =
      Builder.CreateLoad(Type::getInt64Ty(Ctx), TagsPtr, "hint.tags.old");

  Value *NewTags;
  if (ComputeType != SCHED_COMPUTE_NONE) {
    NewTags = Builder.CreateOr(
        OldTags,
        ConstantInt::get(Type::getInt64Ty(Ctx), SCHED_TAG_COMPUTE_DENSE),
        "hint.tags.set");
  } else {
    NewTags = Builder.CreateAnd(
        OldTags,
        ConstantInt::get(Type::getInt64Ty(Ctx), ~SCHED_TAG_COMPUTE_DENSE),
        "hint.tags.clear");
  }
  Builder.CreateStore(NewTags, TagsPtr);
}

//===----------------------------------------------------------------------===//
// SchedTagPass::run — pure instrumentation, no analysis
//===----------------------------------------------------------------------===//

PreservedAnalyses SchedTagPass::run(Module &M, ModuleAnalysisManager &MAM) {
  auto &FAM = MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();

  // Collect plans from all functions (read-only phase).
  struct FuncPlan {
    Function *F;
    DensityResult Plan;
  };
  SmallVector<FuncPlan, 8> AllPlans;

  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    auto Plan = FAM.getResult<ComputeDense>(F);
    if (!Plan.empty())
      AllPlans.push_back({&F, std::move(Plan)});
  }

  if (AllPlans.empty()) {
    errs() << "[SchedTag] no compute-dense regions found, skipping.\n";
    return PreservedAnalyses::all();
  }

  // Create the global variable and instrument IR.
  GlobalVariable *HintGV = getOrCreateSchedHintGV(M);

  // Emit a module constructor that calls prctl() to register
  // the TLS offset and main-thread address with the kernel.
  emitPrctlConstructor(M, HintGV);

  unsigned LoopSets = 0, LoopClrs = 0, BBSets = 0, BBClrs = 0;

  for (auto &[FuncPtr, Plan] : AllPlans) {
    Function &F = *FuncPtr;

    // --- Loop-level instrumentation ---
    for (auto &LR : Plan.Loops) {
      uint8_t ComputeType = denseTypeToHintConst(LR.Type);

      // SET before the branch into the loop header.
      {
        IRBuilder<> Builder(LR.Preheader->getTerminator());
        emitTagStores(Builder, HintGV, ComputeType);
      }
      LoopSets++;

      errs() << "[SchedTag]   LOOP SET  " << F.getName() << "::preheader("
             << LR.Preheader->getName() << ") -> " << hintConstName(ComputeType)
             << "\n";

      // CLR at entry of each exit block.
      for (BasicBlock *Exit : LR.ExitBlocks) {
        IRBuilder<> Builder(&*Exit->getFirstNonPHIOrDbg());
        emitTagStores(Builder, HintGV, SCHED_COMPUTE_NONE);
        LoopClrs++;

        errs() << "[SchedTag]   LOOP CLR  " << F.getName() << "::exit("
               << Exit->getName() << ") -> NONE\n";
      }
    }

    // --- BB-level instrumentation ---
    for (auto &BR : Plan.StandaloneBBs) {
      uint8_t ComputeType = denseTypeToHintConst(BR.Type);

      // SET at BB entry (after PHI nodes).
      {
        IRBuilder<> Builder(&*BR.BB->getFirstNonPHIOrDbg());
        emitTagStores(Builder, HintGV, ComputeType);
      }
      BBSets++;

      // CLR before terminator.
      {
        IRBuilder<> Builder(BR.BB->getTerminator());
        emitTagStores(Builder, HintGV, SCHED_COMPUTE_NONE);
      }
      BBClrs++;

      errs() << "[SchedTag]   BB   SET+CLR  " << F.getName()
             << "::" << BR.BB->getName() << " -> " << hintConstName(ComputeType)
             << "\n";
    }
  }

  errs() << "[SchedTag] instrumented: " << LoopSets << " loop-SET, " << LoopClrs
         << " loop-CLR, " << BBSets << " bb-SET, " << BBClrs << " bb-CLR "
         << "across " << AllPlans.size() << " functions.\n";

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

            PB.registerAnalysisRegistrationCallback(
                [](FunctionAnalysisManager &FAM) {
                  FAM.registerPass([&] { return sched_tag::ComputeDense(); });
                });
          }};
}

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getSchedTagPluginInfo();
}
