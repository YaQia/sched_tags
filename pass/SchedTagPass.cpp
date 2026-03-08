#include "SchedTagPass.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"

using namespace llvm;

namespace sched_tag {

ComputeDense::ComputeDenseType isComputeDense(Instruction &I) {
  // 1. 先排除非计算指令
  if (isa<LoadInst>(I) || isa<StoreInst>(I) || isa<PHINode>(I) ||
      isa<SelectInst>(I) || isa<AllocaInst>(I) || I.isTerminator() ||
      isa<CallInst>(I)) // 函数调用一般不算 ALU，可按需决定
    return ComputeDense::NONE;

  // 2. 检查操作码是否属于计算指令
  bool isIntOp = false, isFloatOp = false;
  switch (I.getOpcode()) {
  // 整数二元运算
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
  // 整数比较
  case Instruction::ICmp:
  // 整数转换（结果类型为整数）
  case Instruction::Trunc:
  case Instruction::ZExt:
  case Instruction::SExt:
  case Instruction::FPToUI:
  case Instruction::FPToSI:
  case Instruction::PtrToInt:
  case Instruction::IntToPtr:
    isIntOp = true;
    break;

  // 浮点二元运算
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

  // 向量专用指令（它们必然涉及向量，应归为 SIMD）
  case Instruction::ExtractElement:
  case Instruction::InsertElement:
  case Instruction::ShuffleVector:
    return ComputeDense::SIMD;

  default:
    return ComputeDense::NONE;
  }

  // 3. 判断是否涉及向量（结果类型或任一操作数类型为向量）
  bool isVector = I.getType()->isVectorTy();
  if (!isVector) {
    for (unsigned i = 0; i < I.getNumOperands(); ++i) {
      if (I.getOperand(i)->getType()->isVectorTy()) {
        isVector = true;
        break;
      }
    }
  }

  if (isVector) {
    // 向量指令，不论元素类型，都返回 SIMD
    return ComputeDense::SIMD;
  }

  // 4. 标量指令，根据操作码返回 INT 或 FLOAT
  if (isIntOp)
    return ComputeDense::INT;
  if (isFloatOp)
    return ComputeDense::FLOAT;

  return ComputeDense::NONE;
}

AnalysisKey ComputeDense::Key;
ComputeDense::Result ComputeDense::run(Function &F,
                                       FunctionAnalysisManager &FAM) {
  Result ResultMap;

    for (BasicBlock &BB : F) {
      uint32_t IntCnt, FloatCnt, SIMDCnt;
      for (Instruction &Inst : BB) {
        switch (isComputeDense(Inst)) {
          case INT:
            IntCnt++;
            break;
          case FLOAT:
            FloatCnt++;
            break;
          case SIMD:
            SIMDCnt++;
            break;
          default: // NONE
            break;
        }
      }
      if (IntCnt * 1.0 / BB.size() >= 0.7) {
        // INT 密集
        ResultMap[&BB] = INT;
      } else if (FloatCnt * 1.0 / BB.size() >= 0.7) {
        // FLOAT 密集
        ResultMap[&BB] = FLOAT;
      } else if (SIMDCnt * 1.0 / BB.size() >= 0.7) {
        // SIMD 密集
        ResultMap[&BB] = SIMD;
      }
  }
  return ResultMap;
}

PreservedAnalyses SchedTagPass::run(Function &F, FunctionAnalysisManager &FAM) {
  // TODO: Insert code for tags
  auto ComputeDenseMap = FAM.getResult<ComputeDense>(F);
  return PreservedAnalyses::all();
}

} // namespace sched_tag

//===----------------------------------------------------------------------===//
// New PM (Pass Manager) plugin registration
//===----------------------------------------------------------------------===//

// This is the entry point for the pass plugin.
// `opt` will call this function to register the pass when loading the plugin.
llvm::PassPluginLibraryInfo getSchedTagPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "SchedTag", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            // Register the pass so it can be used via:
            //   opt -passes=sched-tag ...
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &FPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "sched-tag") {
                    FPM.addPass(sched_tag::SchedTagPass());
                    return true;
                  }
                  return false;
                });
            PB.registerAnalysisRegistrationCallback(
                [](FunctionAnalysisManager &FAM) {
                  FAM.registerPass([&] { return sched_tag::ComputeDense(); });
                });
          }};
}

// The public entry point for a pass plugin.
extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getSchedTagPluginInfo();
}
