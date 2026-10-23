#include "SchedTagCommon.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

using namespace llvm;

namespace sched_tag {

//===----------------------------------------------------------------------===//
// getSchedHintType — LLVM struct matching sched_hint.h (64 bytes)
//===----------------------------------------------------------------------===//

StructType *getSchedHintType(LLVMContext &Ctx) {
  StructType *Ty = StructType::getTypeByName(Ctx, "struct.sched_hint");
  if (Ty)
    return Ty;

  auto *I8 = Type::getInt8Ty(Ctx);
  auto *I32 = Type::getInt32Ty(Ctx);
  auto *I64 = Type::getInt64Ty(Ctx);

  Ty = StructType::create(Ctx,
                          {
                              I32,                    //  [0] magic
                              I32,                    //  [1] version
                              I8,                     //  [2] exec_dense
                              I8,                     //  [3] memory_dense
                              I8,                     //  [4] atomic_dense
                              I8,                     //  [5] unshared
                              I8,                     //  [6] load_trend
                              ArrayType::get(I8, 3),  //  [7] reserved[3]
                              I64,                    //  [8] atomic_magic
                              I64,                    //  [9] dep_magic
                              I64,                    // [10] unshared_magic
                              I8,                     // [11] dep_role
                              ArrayType::get(I8, 23), // [12] reserved2[23]
                          },
                          "struct.sched_hint",
                          /*isPacked=*/false);

  return Ty;
}

//===----------------------------------------------------------------------===//
// Runtime declarations
//===----------------------------------------------------------------------===//
//
// The kernel owns the 64-byte slot and maps it VM_SHARED into the process, so
// userspace only has to register each thread once via
// prctl(PR_SET_SCHED_HINT, &slot); the kernel back-fills the slot's user
// address.
//
// The support symbols (the TLS cache, the registrar, the fork hook) live once
// in libsched_tag, not emitted per module: a constructor emitted per
// translation unit would add one .init_array entry per TU. This pass only
// declares the two symbols its instrumented code references.

static constexpr const char *HINT_CACHE_SYMBOL = "__sched_hint_ptr";

GlobalVariable *getOrCreateHintPtrCache(Module &M) {
  if (GlobalVariable *GV = M.getNamedGlobal(HINT_CACHE_SYMBOL))
    return GV;

  auto *PtrTy = PointerType::getUnqual(M.getContext());
  auto *GV = new GlobalVariable(M, PtrTy, /*isConstant=*/false,
                                GlobalValue::ExternalLinkage,
                                /*Initializer=*/nullptr, HINT_CACHE_SYMBOL);
  GV->setThreadLocalMode(GlobalValue::InitialExecTLSModel);
  return GV;
}

// Per-thread registration, provided by libsched_tag: prctl back-fills the slot
// address, then verifies the kernel stamped this build's magic/version into it
// (an ABI guard), and caches the slot (or the sentinel (ptr)1 on failure).
Function *getOrCreateRegisterFn(Module &M) {
  if (Function *F = M.getFunction("__sched_hint_register"))
    return F;

  auto *PtrTy = PointerType::getUnqual(M.getContext());
  return Function::Create(FunctionType::get(PtrTy, false),
                          GlobalValue::ExternalLinkage, "__sched_hint_register",
                          M);
}

void declareHintRuntime(Module &M) {
  // Contract cross-check: the FIELD_* GEP indices must land on the same byte
  // offsets as the C layout in include/sched_hint.h (and the kernel's
  // include/linux/sched/hint.h). A struct-type edit that breaks the ABI is
  // caught here at instrumentation time instead of silently writing tags into
  // the wrong fields.
  {
    StructType *HintTy = getSchedHintType(M.getContext());
    const StructLayout *SL = M.getDataLayout().getStructLayout(HintTy);
    auto CheckOffset = [&](unsigned FieldIdx, uint64_t ExpectOff,
                           const char *Name) {
      if (SL->getElementOffset(FieldIdx) != ExpectOff)
        errs() << "[SchedTag] ABI BROKEN: " << Name << " (field " << FieldIdx
               << ") is at byte offset " << SL->getElementOffset(FieldIdx)
               << ", expected " << ExpectOff
               << ". getSchedHintType() and SchedTagCommon.h FIELD_* are out of"
               << " sync with sched_hint.h!\n";
    };
    CheckOffset(FIELD_EXEC_DENSE, 8, "exec_dense");
    CheckOffset(FIELD_MEMORY_DENSE, 9, "memory_dense");
    CheckOffset(FIELD_ATOMIC_DENSE, 10, "atomic_dense");
    CheckOffset(FIELD_UNSHARED, 11, "unshared");
    CheckOffset(FIELD_LOAD_TREND, 12, "load_trend");
    CheckOffset(FIELD_ATOMIC_MAGIC, 16, "atomic_magic");
    CheckOffset(FIELD_DEP_MAGIC, 24, "dep_magic");
    CheckOffset(FIELD_UNSHARED_MAGIC, 32, "unshared_magic");
    CheckOffset(FIELD_DEP_ROLE, 40, "dep_role");
    if (SL->getSizeInBytes() != 64)
      errs() << "[SchedTag] ABI BROKEN: struct.sched_hint size is "
             << SL->getSizeInBytes() << " bytes, expected 64!\n";
  }

  getOrCreateHintPtrCache(M);
  getOrCreateRegisterFn(M);

  errs() << "[SchedTag] referencing sched_hint library symbols (libsched_tag)\n";
}

//===----------------------------------------------------------------------===//
// emitSlotStore — hot-path store into the current thread's kernel slot
//===----------------------------------------------------------------------===//
//
// Hand-inlined on purpose: a helper call at every tag site would cost a call,
// and the optimizer cannot recover it (this pass runs at the very end of the
// pipeline, after the inliner). So the fast path is load-cache / test / store,
// and only the cold registration is an out-of-line call.
//
// The current block is split at the insertion point so its original tail
// (including any terminator) moves to a continuation block that every path
// rejoins.
static void emitSlotStore(IRBuilder<> &Builder, unsigned FieldIndex, Value *Val) {
  LLVMContext &Ctx = Builder.getContext();
  StructType *HintTy = getSchedHintType(Ctx);
  auto *PtrTy = PointerType::getUnqual(Ctx);
  auto *I64 = Type::getInt64Ty(Ctx);

  Function *Fn = Builder.GetInsertBlock()->getParent();
  Module *M = Fn->getParent();
  GlobalVariable *Cache = getOrCreateHintPtrCache(*M);
  Function *Register = getOrCreateRegisterFn(*M);
  // Sentinel (ptr)1 == "registration failed, give up". Real slots are 64-byte
  // aligned userspace addresses, so <= 1 is never a live slot.
  Value *Disabled = ConstantExpr::getIntToPtr(ConstantInt::get(I64, 1), PtrTy);

  BasicBlock *Head = Builder.GetInsertBlock();
  Instruction *IP = &*Builder.GetInsertPoint();
  Value *Cached = Builder.CreateLoad(PtrTy, Cache, "hint.cached");
  Value *Unset = Builder.CreateICmpULE(Cached, Disabled, "hint.unset");

  BasicBlock *Cont = SplitBlock(Head, IP);
  Head->getTerminator()->eraseFromParent();

  BasicBlock *SlowBB = BasicBlock::Create(Ctx, "hint.slow", Fn, Cont);
  BasicBlock *StoreBB = BasicBlock::Create(Ctx, "hint.store", Fn, Cont);

  Builder.SetInsertPoint(Head);
  Builder.CreateCondBr(Unset, SlowBB, StoreBB);

  Builder.SetInsertPoint(SlowBB);
  Value *Slot = Builder.CreateCall(Register, {}, "hint.slot");
  Value *Bad = Builder.CreateICmpULE(Slot, Disabled, "hint.bad");
  Builder.CreateCondBr(Bad, Cont, StoreBB);

  Builder.SetInsertPoint(StoreBB);
  PHINode *Base = Builder.CreatePHI(PtrTy, 2, "hint.base");
  Base->addIncoming(Cached, Head);
  Base->addIncoming(Slot, SlowBB);
  Value *FieldPtr =
      Builder.CreateStructGEP(HintTy, Base, FieldIndex, "hint.field");
  Builder.CreateStore(Val, FieldPtr);
  Builder.CreateBr(Cont);

  // Resume at the start of the continuation block (the split tail).
  Builder.SetInsertPoint(Cont, Cont->begin());
}

//===----------------------------------------------------------------------===//
// emitFieldStore — store a value to a tag payload field
//===----------------------------------------------------------------------===//

void emitFieldStore(IRBuilder<> &Builder, unsigned FieldIndex,
                    uint64_t FieldValue) {
  StructType *HintTy = getSchedHintType(Builder.getContext());
  Type *FieldTy = HintTy->getElementType(FieldIndex);
  emitSlotStore(Builder, FieldIndex, ConstantInt::get(FieldTy, FieldValue));
}

//===----------------------------------------------------------------------===//
// emitBloomMagicStore — bloom-filter hash of base pointers → atomic_magic
//===----------------------------------------------------------------------===//
//
// For each base pointer, compute a k-bit bloom signature in a 64-bit word:
//
//   h     = (uint64_t)ptr * BLOOM_HASH_PRIME
//   bloom = (1 << (h & 63))
//         | (1 << ((h >> 16) & 63))
//         | (1 << ((h >> 32) & 63))
//         | (1 << ((h >> 48) & 63))
//
// Multiple pointers are accumulated with OR (idempotent, commutative).
// The scheduler detects overlap: popcount(magic_a & magic_b) >= K_BLOOM_BITS.

/// Emit IR for one pointer's bloom signature (returns i64 value with k bits set).
static Value *emitBloomBits(IRBuilder<> &Builder, Value *PtrVal) {
  LLVMContext &Ctx = Builder.getContext();
  auto *I64 = Type::getInt64Ty(Ctx);
  Value *One = ConstantInt::get(I64, 1);
  Value *Mask63 = ConstantInt::get(I64, 63);

  // %ptr_int = ptrtoint %ptr to i64
  Value *PtrInt = Builder.CreatePtrToInt(PtrVal, I64, "bloom.ptr");

  // Mask off lower 6 bits to align to 64-byte cache line.
  // This ensures that pointers within the same cache line (potential false sharing)
  // map to the exact same hash, triggering the co-scheduling logic.
  Value *CacheLineMask = ConstantInt::get(I64, ~63ULL);
  Value *AlignedPtr = Builder.CreateAnd(PtrInt, CacheLineMask, "bloom.align");

  // %h = mul i64 %aligned_ptr, BLOOM_HASH_PRIME
  Value *H = Builder.CreateMul(AlignedPtr,
                               ConstantInt::get(I64, BLOOM_HASH_PRIME),
                               "bloom.hash");

  // Extract K_BLOOM_BITS=4 positions at 16-bit intervals and set those bits.
  Value *Bloom = ConstantInt::get(I64, 0);
  for (unsigned i = 0; i < K_BLOOM_BITS; ++i) {
    Value *Shifted = (i == 0) ? H
                              : Builder.CreateLShr(H,
                                    ConstantInt::get(I64, i * (sizeof(uint64_t) * 8) / K_BLOOM_BITS),
                                    "bloom.sh" + Twine(i));
    Value *Pos = Builder.CreateAnd(Shifted, Mask63,
                                   "bloom.pos" + Twine(i));
    Value *Bit = Builder.CreateShl(One, Pos, "bloom.bit" + Twine(i));
    Bloom = Builder.CreateOr(Bloom, Bit, "bloom.acc" + Twine(i));
  }

  return Bloom;
}

void emitBloomMagicStore(IRBuilder<> &Builder,
                         ArrayRef<Value *> BasePointers, unsigned FieldIdx) {
  LLVMContext &Ctx = Builder.getContext();
  auto *I64 = Type::getInt64Ty(Ctx);

  Value *Magic;
  if (BasePointers.empty()) {
    // Unknown — store 0 so the scheduler treats it as "no info".
    Magic = ConstantInt::get(I64, 0);
  } else {
    Magic = ConstantInt::get(I64, 0);
    for (Value *Ptr : BasePointers) {
      Value *Bloom = emitBloomBits(Builder, Ptr);
      Magic = Builder.CreateOr(Magic, Bloom, "bloom.magic");
    }
  }

  emitSlotStore(Builder, FieldIdx, Magic);
}

//===----------------------------------------------------------------------===//
// resolveToPreheaderValue — fix bloom-filter dominance for loop-internal ptrs
//===----------------------------------------------------------------------===//

Value *resolveToPreheaderValue(Value *V, const DominatorTree &DT,
                               BasicBlock *InsertionPoint) {
  // Non-instruction values (Arguments, Constants, GlobalValues) always
  // dominate every point in the function.
  auto *Inst = dyn_cast<Instruction>(V);
  if (!Inst)
    return V;

  // If the instruction already dominates the insertion point, use it directly.
  if (DT.dominates(Inst, InsertionPoint->getTerminator()))
    return V;

  // Cannot resolve — this pointer is loop-internal (e.g. a load from
  // a data structure computed each iteration, or an initial pointer for traversal).
  // Benchmarking shows that trying to track the initial pointer for traversals
  // causes false sharing and degrades scheduling decisions. Skip it.
  return nullptr;
}

//===----------------------------------------------------------------------===//
// collectBasePointers — Extract base pointers from atomic instructions
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
static Value *stripToBase(Value *Ptr) {
  return Ptr->stripInBoundsOffsets();
}

SmallVector<Value *, 4>
collectBasePointers(ArrayRef<BasicBlock *> BBs,
                    const DominatorTree *DT,
                    BasicBlock *InsertionPoint) {
  SmallPtrSet<Value *, 4> Seen;
  SmallVector<Value *, 4> Bases;

  for (BasicBlock *BB : BBs) {
    for (Instruction &I : *BB) {
      Value *Ptr = getAtomicPointerOperand(I);
      if (!Ptr)
        continue;
      Value *Base = stripToBase(Ptr);

      // For loop-level collection, resolve to a preheader-available value.
      if (DT && InsertionPoint) {
        Base = resolveToPreheaderValue(Base, *DT, InsertionPoint);
        if (!Base)
          continue;  // Truly loop-internal — skip.
      }

      if (Seen.insert(Base).second)
        Bases.push_back(Base);
    }
  }

  return Bases;
}

//===----------------------------------------------------------------------===//
// getLabelTypeFieldIndex — Map label type string to field index
//===----------------------------------------------------------------------===//

std::optional<std::pair<unsigned, bool>>
getLabelTypeFieldIndex(StringRef LabelType) {
  // Returns {field_index, needs_bloom_filter}

  if (LabelType == "exec-dense")
    return std::make_pair(FIELD_EXEC_DENSE, false);

  if (LabelType == "memory-dense")
    return std::make_pair(FIELD_MEMORY_DENSE, false);

  if (LabelType == "atomic-dense")
    return std::make_pair(FIELD_ATOMIC_DENSE, true);   // bloom filter for atomic_magic

  if (LabelType == "unshared")
    return std::make_pair(FIELD_UNSHARED, true);       // bloom filter for unshared_magic

  if (LabelType == "load-trend")
    return std::make_pair(FIELD_LOAD_TREND, false);

  // Unknown type: hard error. Silently guessing a field would corrupt an
  // unrelated tag, so the caller must skip this label entirely.
  errs() << "[SchedTag] error: unknown label type '" << LabelType
         << "', skipping label (supported: exec-dense, memory-dense, "
            "atomic-dense, unshared, load-trend)\n";
  return std::nullopt;
}

} // namespace sched_tag
