#include "ExecDenseAnalysis.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace sched_tag {

//===----------------------------------------------------------------------===//
// Thresholds (exec-dense specific)
//===----------------------------------------------------------------------===//

static constexpr unsigned MIN_BB_SIZE = 10;
static constexpr unsigned MIN_LOOP_SIZE = 10;
static constexpr double LOOP_DENSE_THRESHOLD = 0.5;
static constexpr double BB_DENSE_THRESHOLD = 0.7;

// Minimum fraction of arithmetic instructions a single type must represent
// before its bit is set in the bitmask.
static constexpr double PER_TYPE_THRES = 0.25;

// Minimum fraction of branch instructions (conditional br / switch /
// indirectbr) for the CTRL bit. In LLVM IR only a terminator can branch,
// so a basic block contributes at most one; density therefore measures
// "short blocks chained by decisions" — the classic branchy profile.
static constexpr double CTRL_DENSE_THRESHOLD = 0.08;

//===----------------------------------------------------------------------===//
// Per-instruction classification
//===----------------------------------------------------------------------===//

ExecOpType execOpType(Instruction &I) {
  // Control-flow pressure: conditional branches and multi-way branches.
  // An unconditional br is straight-line flow, not a decision.
  if (auto *BI = dyn_cast<BranchInst>(&I))
    return BI->isConditional() ? ExecOpType::CTRL : ExecOpType::NONE;
  if (isa<SwitchInst>(I) || isa<IndirectBrInst>(I))
    return ExecOpType::CTRL;

  // Exclude non-compute instructions.
  // Use CallBase to cover both CallInst and InvokeInst (C++/Rust exceptions)
  if (isa<LoadInst>(I) || isa<StoreInst>(I) || isa<PHINode>(I) ||
      isa<SelectInst>(I) || isa<AllocaInst>(I) || I.isTerminator() ||
      isa<CallBase>(I))
    return ExecOpType::NONE;

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
    return ExecOpType::SIMD;

  default:
    return ExecOpType::NONE;
  }

  // Vector operands -> SIMD regardless of opcode category.
  bool isVector = I.getType()->isVectorTy() ||
      llvm::any_of(I.operands(), [](const Use &U) {
        return U->getType()->isVectorTy();
      });
  if (isVector)
    return ExecOpType::SIMD;

  if (isIntOp)
    return ExecOpType::INT;
  if (isFloatOp)
    return ExecOpType::FLOAT;
  return ExecOpType::NONE;
}

//===----------------------------------------------------------------------===//
// Helper: classify from counts + threshold
//===----------------------------------------------------------------------===//

/// Return a bitmask of SCHED_EXEC_* bits.
///
/// - The CTRL bit is set independently of the arithmetic gate: a branchy
///   region with little arithmetic is exactly the CTRL-only profile the
///   SMT scheduler wants to spread across cores.
/// - The arithmetic bits (INT/FLOAT/SIMD) require the combined arithmetic
///   ratio to meet Threshold; each individual type must then represent
///   >= max(PER_TYPE_THRES, Threshold / 3) of arithmetic instructions to
///   earn its bit.
static uint8_t classifyFromCounts(uint32_t IntCnt, uint32_t FloatCnt,
                                  uint32_t SIMDCnt, uint32_t CTRLCnt,
                                  uint32_t Total, double Threshold) {
  if (Total == 0)
    return SCHED_EXEC_NONE;

  uint8_t Mask = 0;

  if (CTRLCnt > 0 &&
      static_cast<double>(CTRLCnt) / Total >= CTRL_DENSE_THRESHOLD)
    Mask |= SCHED_EXEC_CTRL;

  uint32_t ArithTotal = IntCnt + FloatCnt + SIMDCnt;
  if (static_cast<double>(ArithTotal) / Total >= Threshold) {
    double AT = static_cast<double>(ArithTotal);
    if (IntCnt > 0 && IntCnt / AT >= std::max(PER_TYPE_THRES, Threshold / 3))
      Mask |= SCHED_EXEC_INT;
    if (FloatCnt > 0 &&
        FloatCnt / AT >= std::max(PER_TYPE_THRES, Threshold / 3))
      Mask |= SCHED_EXEC_FLOAT;
    if (SIMDCnt > 0 &&
        SIMDCnt / AT >= std::max(PER_TYPE_THRES, Threshold / 3))
      Mask |= SCHED_EXEC_SIMD;
  }
  return Mask;
}

//===----------------------------------------------------------------------===//
// ExecDense::run — unified density analysis
//===----------------------------------------------------------------------===//

AnalysisKey ExecDense::Key;

ExecDense::Result ExecDense::run(Function &F,
                                 FunctionAnalysisManager &FAM) {
  DensityResult Plan;

  //--- Step 1: Single pass — cache per-BB instruction counts ---------------

  struct BBCounts {
    uint32_t Total = 0;
    uint32_t IntCnt = 0;
    uint32_t FloatCnt = 0;
    uint32_t SIMDCnt = 0;
    uint32_t CTRLCnt = 0;
  };

  DenseMap<BasicBlock *, BBCounts> BBStats;

  for (BasicBlock &BB : F) {
    BBCounts C;
    for (Instruction &I : BB) {
      C.Total++;
      switch (execOpType(I)) {
      case ExecOpType::INT:
        C.IntCnt++;
        break;
      case ExecOpType::FLOAT:
        C.FloatCnt++;
        break;
      case ExecOpType::SIMD:
        C.SIMDCnt++;
        break;
      case ExecOpType::CTRL:
        C.CTRLCnt++;
        break;
      default:
        break;
      }
    }
    BBStats[&BB] = C;
  }

  //--- Step 2: Loop-level analysis (outermost-first) -----------------------

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

    // Aggregate counts from cached per-BB stats.
    uint32_t Total = 0, IntCnt = 0, FloatCnt = 0, SIMDCnt = 0, CTRLCnt = 0;
    for (BasicBlock *BB : L->getBlocks()) {
      auto It = BBStats.find(BB);
      if (It == BBStats.end())
        continue;
      const BBCounts &C = It->second;
      Total += C.Total;
      IntCnt += C.IntCnt;
      FloatCnt += C.FloatCnt;
      SIMDCnt += C.SIMDCnt;
      CTRLCnt += C.CTRLCnt;
    }

    if (Total < MIN_LOOP_SIZE) {
      for (Loop *Sub : L->getSubLoops())
        Worklist.push_back(Sub);
      continue;
    }

    uint8_t LoopMask = classifyFromCounts(IntCnt, FloatCnt, SIMDCnt, CTRLCnt,
                                          Total, LOOP_DENSE_THRESHOLD);
    if (LoopMask == SCHED_EXEC_NONE) {
      for (Loop *Sub : L->getSubLoops())
        Worklist.push_back(Sub);
      continue;
    }

    // Dense loop — need a preheader to instrument.
    BasicBlock *Preheader = L->getLoopPreheader();
    if (!Preheader) {
      for (Loop *Sub : L->getSubLoops())
        Worklist.push_back(Sub);
      continue;
    }

    LoopRegion LR;
    LR.Preheader = Preheader;
    LR.Value = LoopMask;

    // Get deduplicated exit blocks directly using LLVM's built-in method
    L->getUniqueExitBlocks(LR.ExitBlocks);

    Plan.Loops.push_back(std::move(LR));

    for (BasicBlock *BB : L->getBlocks())
      CoveredByLoop.insert(BB);
  }

  //--- Step 3: BB-level fallback -------------------------------------------

  for (BasicBlock &BB : F) {
    if (CoveredByLoop.contains(&BB))
      continue;

    const BBCounts &C = BBStats[&BB];
    if (C.Total < MIN_BB_SIZE)
      continue;

    uint8_t BBMask = classifyFromCounts(C.IntCnt, C.FloatCnt, C.SIMDCnt,
                                        C.CTRLCnt, C.Total, BB_DENSE_THRESHOLD);
    if (BBMask == SCHED_EXEC_NONE)
      continue;

    Plan.StandaloneBBs.push_back({&BB, BBMask});
  }

  return Plan;
}

} // namespace sched_tag
