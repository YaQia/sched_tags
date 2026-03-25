#include "AtomicDenseAnalysis.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PatternMatch.h"

using namespace llvm;
using namespace llvm::PatternMatch;

namespace sched_tag {

//===----------------------------------------------------------------------===//
// Thresholds (atomic-dense specific)
//===----------------------------------------------------------------------===//

// Minimum number of instructions for a BB / loop to be considered.
// Lowered to 2 for benchmarking purposes — in production, tight CAS loops
// with just 2-3 atomic instructions are common (spinlock, atomic counter).
static constexpr unsigned ATOMIC_MIN_BB_SIZE = 8;
static constexpr unsigned ATOMIC_MIN_LOOP_SIZE = 2;

// Density thresholds.
// Lowered to 0.15 for microbenchmarking — allows testing 4-atomic-op loops
// with CPU migration checking overhead (~4 atomics / 25 instructions = 0.16).
// In production, typical lock-free loops have higher density.
static constexpr double ATOMIC_LOOP_DENSE_THRESHOLD = 0.15;
static constexpr double ATOMIC_BB_DENSE_THRESHOLD = 0.6;

//===----------------------------------------------------------------------===//
// Per-instruction classification
//===----------------------------------------------------------------------===//

bool isAtomicOp(const Instruction &I) {
  // Only count read-modify-write and compare-and-swap — these are the
  // instructions that require the cache line in Exclusive state and generate
  // MESI invalidation traffic under contention (LOCK prefix on x86,
  // LL/SC or LSE atomics on AArch64).
  //
  // NOT counted:
  //   - Atomic load  (any ordering): cache line stays Shared, no MESI cost
  //     beyond a normal load.  Ordering is a compiler/hardware fence, orthogonal.
  //   - Atomic store (any ordering): same as a normal store (Modified state).
  //     Even seq_cst store's XCHG on x86 is an implementation detail; the
  //     scheduler cares about sustained coherence *traffic*, not single stores.
  //   - FenceInst: pipeline barrier, no cache line transfer.
  return isa<AtomicRMWInst>(I) || isa<AtomicCmpXchgInst>(I);
}

//===----------------------------------------------------------------------===//
// Base pointer extraction
//===----------------------------------------------------------------------===//

/// Get the pointer operand of an atomic RMW or CAS instruction.
/// Returns nullptr for non-atomic instructions.
static Value *getAtomicPointerOperand(Instruction &I) {
  if (auto *RMW = dyn_cast<AtomicRMWInst>(&I))
    return RMW->getPointerOperand();
  if (auto *CAS = dyn_cast<AtomicCmpXchgInst>(&I))
    return CAS->getPointerOperand();
  return nullptr;
}

/// Strip pointer casts and in-bounds GEPs to find the underlying base object.
/// This groups operations on different fields of the same struct together.
static Value *stripToBase(Value *Ptr) {
  return Ptr->stripInBoundsOffsets();
}

/// Collect unique base pointers from all atomic RMW/CAS instructions in a
/// set of basic blocks.  Results are deduplicated by SSA Value identity.
static SmallVector<Value *, 4> collectBasePointers(ArrayRef<BasicBlock *> BBs) {
  SmallPtrSet<Value *, 4> Seen;
  SmallVector<Value *, 4> Bases;

  for (BasicBlock *BB : BBs) {
    for (Instruction &I : *BB) {
      Value *Ptr = getAtomicPointerOperand(I);
      if (!Ptr)
        continue;
      Value *Base = stripToBase(Ptr);
      if (Seen.insert(Base).second)
        Bases.push_back(Base);
    }
  }

  return Bases;
}

//===----------------------------------------------------------------------===//
// CAS Retry Loop Pattern Detection
//===----------------------------------------------------------------------===//

static constexpr unsigned CAS_RETRY_MAX_LOOP_SIZE = 32;

/// Checks if a loop matches the "CAS retry spin loop" pattern.
/// The pattern is characterized by:
/// 1. A small loop body (<= CAS_RETRY_MAX_LOOP_SIZE instructions).
/// 2. At least one cmpxchg instruction.
/// 3. The loop's latch logic (conditional branch to backedge) depends directly
///    on the success of AT LEAST ONE of the cmpxchg instructions.
static bool isCASRetryLoop(const Loop *L) {
  // Step 1: Check loop size and find all cmpxchg instructions.
  unsigned TotalInsts = 0;
  SmallVector<AtomicCmpXchgInst *, 2> CASInsts;

  for (BasicBlock *BB : L->getBlocks()) {
    for (Instruction &I : *BB) {
      TotalInsts++;
      if (auto *CAS = dyn_cast<AtomicCmpXchgInst>(&I)) {
        CASInsts.push_back(CAS);
      }
    }
  }

  if (CASInsts.empty() || TotalInsts > CAS_RETRY_MAX_LOOP_SIZE)
    return false;

  // Step 2: The loop must have a single latch with a conditional branch.
  BasicBlock *Latch = L->getLoopLatch();
  if (!Latch)
    return false;

  auto *Br = dyn_cast<BranchInst>(Latch->getTerminator());
  if (!Br || !Br->isConditional())
    return false;

  Value *Cond = Br->getCondition();

  // Step 3: Check if the condition depends cleanly on a CAS result.
  // There are two common lowering patterns for checking CAS success:
  // Pattern A: (extractvalue (cmpxchg ...), 1)
  // Pattern B: (icmp eq/ne (extractvalue (cmpxchg ...), 0), expected_old_val)
  
  auto checkCondForCAS = [&](Value *C, AtomicCmpXchgInst *TheCAS) -> bool {
    if (auto *EV = dyn_cast<ExtractValueInst>(C)) {
      // Pattern A: Success bit directly extracted
      if (EV->getAggregateOperand() == TheCAS && EV->getNumIndices() == 1 &&
          EV->getIndices()[0] == 1) {
        return true;
      }
    } else if (auto *ICmp = dyn_cast<ICmpInst>(C)) {
      // Pattern B: Compare older value with expected.
      if (ICmp->getPredicate() == CmpInst::ICMP_EQ || 
          ICmp->getPredicate() == CmpInst::ICMP_NE) {
        Value *Op0 = ICmp->getOperand(0);
        Value *Op1 = ICmp->getOperand(1);
        
        auto isCASResult = [TheCAS](Value *V) {
          if (auto *EV = dyn_cast<ExtractValueInst>(V)) {
            return EV->getAggregateOperand() == TheCAS && 
                   EV->getNumIndices() == 1 && EV->getIndices()[0] == 0;
          }
          return false;
        };

        Value *CASRes = nullptr;
        Value *ExpectedRes = nullptr;

        if (isCASResult(Op0)) {
          CASRes = Op0;
          ExpectedRes = Op1;
        } else if (isCASResult(Op1)) {
          CASRes = Op1;
          ExpectedRes = Op0;
        }

        if (CASRes && ExpectedRes == TheCAS->getCompareOperand()) {
          return true;
        }
      }
    }
    return false;
  };

  bool CondMatches = false;
  for (AtomicCmpXchgInst *TheCAS : CASInsts) {
    if (checkCondForCAS(Cond, TheCAS)) {
      CondMatches = true;
      break;
    }
    
    // Handle logical NOTs on the condition (e.g. xor i1 %cond, true)
    Value *InnerCond;
    if (match(Cond, m_Not(m_Value(InnerCond)))) {
      if (checkCondForCAS(InnerCond, TheCAS)) {
        CondMatches = true;
        break;
      }
    }
  }

  return CondMatches;
}

//===----------------------------------------------------------------------===//
// AtomicDense::run — atomic-density analysis
//===----------------------------------------------------------------------===//

AnalysisKey AtomicDense::Key;

AtomicDense::Result AtomicDense::run(Function &F,
                                     FunctionAnalysisManager &FAM) {
  DensityResult Plan;

  //--- Step 1: Single pass — cache per-BB instruction counts ---------------

  struct BBCounts {
    uint32_t Total = 0;
    uint32_t AtomicCnt = 0;
  };

  DenseMap<BasicBlock *, BBCounts> BBStats;

  for (BasicBlock &BB : F) {
    BBCounts C;
    for (Instruction &I : BB) {
      C.Total++;
      if (isAtomicOp(I))
        C.AtomicCnt++;
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

    // Aggregate counts.
    uint32_t Total = 0, AtomicCnt = 0;
    for (BasicBlock *BB : L->getBlocks()) {
      auto It = BBStats.find(BB);
      if (It == BBStats.end())
        continue;
      const BBCounts &C = It->second;
      Total += C.Total;
      AtomicCnt += C.AtomicCnt;
    }

    if (Total < ATOMIC_MIN_LOOP_SIZE) {
      for (Loop *Sub : L->getSubLoops())
        Worklist.push_back(Sub);
      continue;
    }

    double Ratio = static_cast<double>(AtomicCnt) / Total;
    bool Dense = (Ratio >= ATOMIC_LOOP_DENSE_THRESHOLD) || isCASRetryLoop(L);
    
    if (!Dense) {
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

    // Collect and deduplicate exit blocks.
    SmallVector<BasicBlock *, 4> RawExits;
    L->getExitBlocks(RawExits);

    LoopRegion LR;
    LR.Preheader = Preheader;
    LR.TypeMask = 1; // atomic_dense is a boolean flag

    DenseSet<BasicBlock *> Seen;
    for (BasicBlock *Exit : RawExits) {
      if (Seen.insert(Exit).second)
        LR.ExitBlocks.push_back(Exit);
    }

    // Collect base pointers for bloom-filter magic computation.
    SmallVector<BasicBlock *, 8> LoopBBs(L->getBlocks().begin(),
                                          L->getBlocks().end());
    LR.BasePointers = collectBasePointers(LoopBBs);

    Plan.Loops.push_back(std::move(LR));

    for (BasicBlock *BB : L->getBlocks())
      CoveredByLoop.insert(BB);
  }

  //--- Step 3: BB-level fallback -------------------------------------------

  for (BasicBlock &BB : F) {
    if (CoveredByLoop.contains(&BB))
      continue;

    const BBCounts &C = BBStats[&BB];
    if (C.Total < ATOMIC_MIN_BB_SIZE)
      continue;

    double Ratio = static_cast<double>(C.AtomicCnt) / C.Total;
    if (Ratio < ATOMIC_BB_DENSE_THRESHOLD)
      continue;

    BBRegion BR;
    BR.BB = &BB;
    BR.TypeMask = 1;
    BasicBlock *BBPtr = &BB;
    BR.BasePointers = collectBasePointers(ArrayRef<BasicBlock *>(&BBPtr, 1));
    Plan.StandaloneBBs.push_back(std::move(BR));
  }

  return Plan;
}

} // namespace sched_tag
