#include "SchedTagCommon.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/raw_ostream.h"
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
                              I8,                     //  [2] compute_dense
                              I8,                     //  [3] branch_dense
                              I8,                     //  [4] memory_dense
                              I8,                     //  [5] atomic_dense
                              I8,                     //  [6] io_dense
                              I8,                     //  [7] unshared
                              I8,                     //  [8] compute_prep
                              I8,                     //  [9] reserved_pad
                              I64,                    // [10] atomic_magic
                              I64,                    // [11] dep_magic
                              I8,                     // [12] dep_role
                              ArrayType::get(I8, 7),  // [13] reserved1[7]
                              ArrayType::get(I8, 24), // [14] reserved2[24]
                          },
                          "struct.sched_hint",
                          /*isPacked=*/false);

  return Ty;
}

//===----------------------------------------------------------------------===//
// getOrCreateSchedHintGV — module-level TLS global with obfuscated name
//===----------------------------------------------------------------------===//
//
// Symbol name: "__sched_hint.data"
//
// The period character makes this name illegal in C/C++ source code,
// preventing user-space code from defining a conflicting variable.
// This fixed name ensures link-time consistency across multiple compilation units.

static constexpr const char *SYMBOL_NAME = "__sched_hint.data";

GlobalVariable *getOrCreateSchedHintGV(Module &M) {
  LLVMContext &Ctx = M.getContext();
  StructType *HintTy = getSchedHintType(Ctx);
  auto *I8 = Type::getInt8Ty(Ctx);
  auto *I32 = Type::getInt32Ty(Ctx);
  auto *I64 = Type::getInt64Ty(Ctx);

  Constant *Init = ConstantStruct::get(
      HintTy, {
                  ConstantInt::get(I32, SCHED_HINT_MAGIC),
                  ConstantInt::get(I32, SCHED_HINT_VERSION),
                  ConstantInt::get(I8, SCHED_COMPUTE_NONE), // compute_dense
                  ConstantInt::get(I8, 0),                  // branch_dense
                  ConstantInt::get(I8, 0),                  // memory_dense
                  ConstantInt::get(I8, 0),                  // atomic_dense
                  ConstantInt::get(I8, 0),                  // io_dense
                  ConstantInt::get(I8, 0),                  // unshared
                  ConstantInt::get(I8, 0),                  // compute_prep
                  ConstantInt::get(I8, 0),                  // reserved_pad
                  ConstantInt::get(I64, 0),                 // atomic_magic
                  ConstantInt::get(I64, 0),                 // dep_magic
                  ConstantInt::get(I8, 0),                  // dep_role
                  ConstantAggregateZero::get(ArrayType::get(I8, 7)),  // reserved1
                  ConstantAggregateZero::get(ArrayType::get(I8, 24)), // reserved2
              });

  // Check if a global with this name already exists
  if (auto *Existing = M.getNamedGlobal(SYMBOL_NAME)) {
    // If it's an external declaration, convert to definition
    if (!Existing->hasInitializer()) {
      Existing->setInitializer(Init);
      Existing->setLinkage(GlobalValue::WeakAnyLinkage);
      Existing->setThreadLocalMode(GlobalValue::InitialExecTLSModel);
      Existing->setSection(SCHED_HINT_SECTION);
      Existing->setAlignment(Align(64));
      appendToUsed(M, {Existing});
    }
    return Existing;
  }

  // Create new global variable
  auto *GV = new GlobalVariable(M, HintTy, /*isConstant=*/false,
                                GlobalValue::WeakAnyLinkage, Init,
                                SYMBOL_NAME);

  GV->setThreadLocalMode(GlobalValue::InitialExecTLSModel);
  GV->setSection(SCHED_HINT_SECTION);
  GV->setAlignment(Align(64));
  appendToUsed(M, {GV});
  
  errs() << "[SchedTag] created TLS global: " << SYMBOL_NAME << "\n";
  
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
//       prctl(PR_SET_SCHED_HINT_OFFSET, offset, vaddr, 0, 0);
//   }
//
// The kernel uses `offset` for future child threads (TP + offset -> hint addr)
// and `vaddr` directly for the calling (main) thread.

void emitPrctlConstructor(Module &M, GlobalVariable *HintGV) {
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
  Value *VAddr = Builder.CreatePtrToInt(HintGV, I64, "vaddr");

  // %tp = call ptr @llvm.thread.pointer.p0()
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

  // call i32 (i32, ...) @prctl(i32 83, i64 %offset, i64 %vaddr, i32 0, i32 0)
  Builder.CreateCall(Prctl, {ConstantInt::get(I32, PR_SET_SCHED_HINT_OFFSET),
                             Offset, VAddr, ConstantInt::get(I32, 0),
                             ConstantInt::get(I32, 0)});

  Builder.CreateRetVoid();

  // Append to @llvm.global_ctors (priority 65535 = default).
  appendToGlobalCtors(M, Ctor, /*Priority=*/65535);

  errs() << "[SchedTag] emitted prctl constructor __sched_hint_report()\n";
}

//===----------------------------------------------------------------------===//
// emitFieldStore — store a value to a tag payload field
//===----------------------------------------------------------------------===//

void emitFieldStore(IRBuilder<> &Builder, GlobalVariable *GV,
                    unsigned FieldIndex, uint8_t FieldValue) {
  LLVMContext &Ctx = Builder.getContext();
  StructType *HintTy = getSchedHintType(Ctx);

  // Store the value to the tag payload field.
  Value *FieldPtr =
      Builder.CreateStructGEP(HintTy, GV, FieldIndex, "hint.field.ptr");
  Builder.CreateStore(ConstantInt::get(Type::getInt8Ty(Ctx), FieldValue),
                      FieldPtr);
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

void emitBloomMagicStore(IRBuilder<> &Builder, GlobalVariable *GV,
                         ArrayRef<Value *> BasePointers) {
  LLVMContext &Ctx = Builder.getContext();
  auto *I64 = Type::getInt64Ty(Ctx);
  StructType *HintTy = getSchedHintType(Ctx);

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

  Value *MagicPtr =
      Builder.CreateStructGEP(HintTy, GV, FIELD_ATOMIC_MAGIC, "hint.magic.ptr");
  Builder.CreateStore(Magic, MagicPtr);
}

} // namespace sched_tag
