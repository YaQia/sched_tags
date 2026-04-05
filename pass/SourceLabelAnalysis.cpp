#include "SourceLabelAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include <fstream>

using namespace llvm;

namespace sched_tag {

// Global list of source labels (set once per module)
static SmallVector<SourceLabel, 4> GlobalLabels;

llvm::AnalysisKey SourceLabelAnalysis::Key;

//===----------------------------------------------------------------------===//
// Label management
//===----------------------------------------------------------------------===//

void SourceLabelAnalysis::setLabels(SmallVector<SourceLabel, 4> Labels) {
  GlobalLabels = std::move(Labels);
}

//===----------------------------------------------------------------------===//
// Analysis entry point
//===----------------------------------------------------------------------===//

/// Check if a function name matches the query specification.
/// Supports both exact match and demangled substring match.
static bool matchesFunctionName(StringRef FuncName, const FunctionSpec &Spec) {
  // Try exact match first (fast path)
  if (FuncName == Spec.Name)
    return true;

  // Try demangled match if the query contains ::
  if (Spec.Name.find("::") != std::string::npos) {
    std::string Demangled = llvm::demangle(FuncName.str());
    // Check if demangled name contains the query as substring
    if (Demangled.find(Spec.Name) != std::string::npos) {
      return true;
    }
  }

  return false;
}

SourceLabelResults SourceLabelAnalysis::run(Function &F,
                                            FunctionAnalysisManager &AM) {
  SourceLabelResults Result;

  for (const auto &Label : GlobalLabels) {
    // Check if function name matches (exact or demangled)
    if (!matchesFunctionName(F.getName(), Label.QueryAST.Function))
      continue;

    // TODO: Check signature if specified
    if (Label.QueryAST.Function.Sig.has_value()) {
      // For minimum implementation, skip signature checking
      errs() << "[SourceLabel] warning: signature checking not yet implemented\n";
    }

    // Execute query with the label's value
    auto MatchResult = executeQuery(F, Label.QueryAST, AM, Label.Value);

    if (!MatchResult.empty()) {
      errs() << "[SourceLabel] matched query in " << F.getName() << ": type="
             << Label.Type << ", value=" << (int)Label.Value << "\n";
      
      // Add this label's result to the combined results
      Result.Labels.push_back({Label.Type, std::move(MatchResult)});
    }
  }

  return Result;
}

//===----------------------------------------------------------------------===//
// Query execution helpers
//===----------------------------------------------------------------------===//

/// Check if an LLVM instruction matches the given InstrType.
static bool matchesInstrType(const Instruction &I, InstrType T) {
  switch (T) {
  case InstrType::AtomicRMW:
    return isa<AtomicRMWInst>(I);
  case InstrType::CmpXchg:
    return isa<AtomicCmpXchgInst>(I);
  case InstrType::Call:
    return isa<CallInst>(I);
  case InstrType::Load:
    return isa<LoadInst>(I);
  case InstrType::Store:
    return isa<StoreInst>(I);
  case InstrType::Alloca:
    return isa<AllocaInst>(I);
  case InstrType::Br:
    return isa<BranchInst>(I);
  case InstrType::Switch:
    return isa<SwitchInst>(I);
  case InstrType::Ret:
    return isa<ReturnInst>(I);
  case InstrType::Add:
    if (auto *BO = dyn_cast<BinaryOperator>(&I))
      return BO->getOpcode() == Instruction::Add;
    return false;
  case InstrType::FAdd:
    if (auto *BO = dyn_cast<BinaryOperator>(&I))
      return BO->getOpcode() == Instruction::FAdd;
    return false;
  case InstrType::Mul:
    if (auto *BO = dyn_cast<BinaryOperator>(&I))
      return BO->getOpcode() == Instruction::Mul;
    return false;
  case InstrType::FMul:
    if (auto *BO = dyn_cast<BinaryOperator>(&I))
      return BO->getOpcode() == Instruction::FMul;
    return false;
  case InstrType::Sub:
    if (auto *BO = dyn_cast<BinaryOperator>(&I))
      return BO->getOpcode() == Instruction::Sub;
    return false;
  case InstrType::FSub:
    if (auto *BO = dyn_cast<BinaryOperator>(&I))
      return BO->getOpcode() == Instruction::FSub;
    return false;
  case InstrType::Div:
    if (auto *BO = dyn_cast<BinaryOperator>(&I))
      return BO->getOpcode() == Instruction::SDiv ||
             BO->getOpcode() == Instruction::UDiv;
    return false;
  case InstrType::FDiv:
    if (auto *BO = dyn_cast<BinaryOperator>(&I))
      return BO->getOpcode() == Instruction::FDiv;
    return false;
  }
  return false;
}

/// Find matching instructions in a basic block.
static SmallVector<Instruction *, 4>
findInstructionsInBB(BasicBlock &BB, const InstructionQuery &IQ) {
  SmallVector<Instruction *, 4> Matches;

  for (auto &I : BB) {
    if (matchesInstrType(I, IQ.Type))
      Matches.push_back(&I);
  }

  // Apply predicates
  if (Matches.empty())
    return Matches;

  for (const auto &Pred : IQ.Predicates) {
    if (Pred.Pos.has_value()) {
      switch (*Pred.Pos) {
      case Position::First:
        if (!Matches.empty())
          Matches = {Matches.front()};
        break;
      case Position::Last:
        if (!Matches.empty())
          Matches = {Matches.back()};
        break;
      case Position::Entry:
        // Entry means first instruction in entry block
        // For simplicity, treat as "first"
        if (!Matches.empty())
          Matches = {Matches.front()};
        break;
      }
    }
    // TODO: Handle func= and var= predicates
  }

  return Matches;
}

/// Check if a loop contains instructions matching the pattern.
static bool loopMatchesPattern(Loop *L, const Pattern &P) {
  for (BasicBlock *BB : L->blocks()) {
    for (Instruction &I : *BB) {
      if (matchesInstrType(I, P.Type))
        return true;
    }
  }
  return false;
}

/// Find loops matching the loop query.
static SmallVector<Loop *, 4> findMatchingLoops(LoopInfo &LI,
                                                 const LoopQuery &LQ) {
  SmallVector<Loop *, 4> Matches;

  for (Loop *L : LI) {
    bool AllPatternsMatch = true;

    for (const auto &P : LQ.Patterns) {
      switch (P.Category) {
      case PatternCategory::Contains:
        if (!loopMatchesPattern(L, P))
          AllPatternsMatch = false;
        break;
      case PatternCategory::In:
        // TODO: Implement "in" semantics (instruction is in this loop)
        errs() << "[SourceLabel] warning: 'in' pattern not yet implemented\n";
        break;
      case PatternCategory::NotIn:
        // TODO: Implement "not_in" semantics
        errs() << "[SourceLabel] warning: 'not_in' pattern not yet implemented\n";
        break;
      }

      if (!AllPatternsMatch)
        break;
    }

    if (AllPatternsMatch)
      Matches.push_back(L);
  }

  return Matches;
}

// Note: extractBasePointers is removed - we now use collectBasePointers from SchedTagCommon.

//===----------------------------------------------------------------------===//
// Query execution
//===----------------------------------------------------------------------===//

DensityResult executeQuery(Function &F, const Query &Q,
                           FunctionAnalysisManager &AM,
                           uint8_t TypeMask) {
  DensityResult Result;

  // Get LoopInfo and DominatorTree if we need them
  LoopInfo *LI = nullptr;
  const DominatorTree *DT = nullptr;
  if (Q.Tgt.Loop.has_value()) {
    LI = &AM.getResult<LoopAnalysis>(F);
    DT = &AM.getResult<DominatorTreeAnalysis>(F);
  }

  // Handle loop-based queries
  // Simplified: loop[contains=X] is sufficient; trailing /instr is optional
  // and only serves as an additional filter (not for positioning).
  // Base pointers for bloom filter are automatically collected from all
  // atomic RMW/CAS instructions in the loop.
  if (Q.Tgt.Loop.has_value()) {
    auto Loops = findMatchingLoops(*LI, *Q.Tgt.Loop);

    for (Loop *L : Loops) {
      LoopRegion LR;
      LR.Preheader = L->getLoopPreheader();
      if (!LR.Preheader) {
        errs() << "[SourceLabel] warning: loop has no preheader in "
               << L->getHeader()->getParent()->getName()
               << ", cannot instrument (consider using opt -loop-simplify first)\n";
        continue;
      }

      // Loop matching is based on contains= patterns in findMatchingLoops().
      // The trailing /instr query (if any) is now ignored for loop-level labels.
      // Base pointers for bloom filter are automatically collected from all
      // atomic RMW/CAS instructions in the loop.

      LR.TypeMask = TypeMask;
      
      // Collect base pointers from ALL atomic operations in the loop
      // (using the shared utility from SchedTagCommon).
      SmallVector<BasicBlock *, 8> LoopBBs(L->blocks().begin(), L->blocks().end());
      LR.BasePointers = collectBasePointers(LoopBBs, DT, LR.Preheader);
      
      Result.Loops.push_back(LR);

      errs() << "[SourceLabel] matched loop at "
             << LR.Preheader->getName() 
             << " (bases=" << LR.BasePointers.size() << ")\n";
    }

    return Result;
  }

  // Handle basic block queries (bb entry | bb exit | bb <name>)
  // No instruction filtering - just mark the specified BB.
  // Instrumentation is at BB entry (first non-PHI instruction).
  if (Q.Tgt.BB.has_value()) {
    const auto &Spec = Q.Tgt.BB->Spec;
    BasicBlock *TargetBB = nullptr;

    if (Spec.IsEntry) {
      TargetBB = &F.getEntryBlock();
    } else if (Spec.IsExit) {
      // Find exit blocks (blocks with return)
      for (BasicBlock &BB : F) {
        if (isa<ReturnInst>(BB.getTerminator())) {
          TargetBB = &BB;
          break; // For simplicity, just take first exit
        }
      }
    } else if (Spec.Name.has_value()) {
      // Find BB by name
      for (BasicBlock &BB : F) {
        if (BB.getName() == *Spec.Name) {
          TargetBB = &BB;
          break;
        }
      }
    }

    if (TargetBB) {
      BBRegion BR;
      BR.BB = TargetBB;
      BR.TypeMask = TypeMask;
      // Collect base pointers from this single BB
      BasicBlock *BBPtr = TargetBB;
      BR.BasePointers = collectBasePointers(ArrayRef<BasicBlock *>(&BBPtr, 1));
      Result.StandaloneBBs.push_back(BR);

      errs() << "[SourceLabel] matched BB " << TargetBB->getName()
             << " (bases=" << BR.BasePointers.size() << ")\n";
    }

    return Result;
  }

  // Handle direct instruction queries (search all BBs for matching instructions)
  // This is for BB-level labeling: find BBs containing the specified instruction.
  for (BasicBlock &BB : F) {
    auto Matches = findInstructionsInBB(BB, Q.Tgt.Instruction);
    if (!Matches.empty()) {
      BBRegion BR;
      BR.BB = &BB;
      BR.TypeMask = TypeMask;
      // Collect base pointers from this single BB
      BasicBlock *BBPtr = &BB;
      BR.BasePointers = collectBasePointers(ArrayRef<BasicBlock *>(&BBPtr, 1));
      Result.StandaloneBBs.push_back(BR);

      errs() << "[SourceLabel] found " << Matches.size()
             << " matching instructions in BB " << BB.getName() << "\n";
    }
  }

  return Result;
}

//===----------------------------------------------------------------------===//
// JSON parsing
//===----------------------------------------------------------------------===//

// Forward declaration
static uint8_t parseSymbolicValue(StringRef Type, StringRef ValueStr);

SmallVector<SourceLabel, 4> parseSchedTagsJSON(StringRef Path) {
  SmallVector<SourceLabel, 4> Labels;

  // Read file
  auto BufferOrErr = MemoryBuffer::getFile(Path);
  if (!BufferOrErr) {
    errs() << "[SourceLabel] warning: could not read " << Path << "\n";
    return Labels;
  }

  // Parse JSON
  auto JsonOrErr = json::parse(BufferOrErr.get()->getBuffer());
  if (!JsonOrErr) {
    errs() << "[SourceLabel] error: invalid JSON in " << Path << "\n";
    return Labels;
  }

  const json::Object *Root = JsonOrErr->getAsObject();
  if (!Root) {
    errs() << "[SourceLabel] error: JSON root is not an object\n";
    return Labels;
  }

  const json::Array *LabelsArray = Root->getArray("labels");
  if (!LabelsArray) {
    errs() << "[SourceLabel] error: 'labels' field missing or not an array\n";
    return Labels;
  }

  // Parse each label
  for (const auto &LabelVal : *LabelsArray) {
    const json::Object *LabelObj = LabelVal.getAsObject();
    if (!LabelObj) {
      errs() << "[SourceLabel] warning: label is not an object, skipping\n";
      continue;
    }

    auto Type = LabelObj->getString("type");
    auto QueryStr = LabelObj->getString("query");

    if (!Type || !QueryStr) {
      errs() << "[SourceLabel] warning: label missing required fields (type, query)\n";
      continue;
    }

    // Parse the SchedQL query
    SchedQLParser Parser(*QueryStr);
    auto QueryAST = Parser.parse();
    if (!QueryAST) {
      errs() << "[SourceLabel] error parsing query '" << *QueryStr
             << "': " << Parser.getError() << "\n";
      continue;
    }

    // Parse the "value" field (required)
    // Supports: integer, boolean, or string (for symbolic names)
    uint8_t Value = 1; // default value for boolean fields
    
    if (auto ValInt = LabelObj->getInteger("value")) {
      // Direct integer value
      Value = static_cast<uint8_t>(*ValInt);
    } else if (auto ValBool = LabelObj->getBoolean("value")) {
      // Boolean: true=1, false=0
      Value = *ValBool ? 1 : 0;
    } else if (auto ValStr = LabelObj->getString("value")) {
      // String: parse symbolic names
      Value = parseSymbolicValue(*Type, *ValStr);
    } else {
      // No value field: use default (1 for boolean, warn for others)
      if (*Type == "compute-dense" || *Type == "memory-dense") {
        errs() << "[SourceLabel] warning: label type '" << *Type
               << "' requires a 'value' field, using default 1\n";
      }
    }

    SourceLabel L;
    L.Type = Type->str();
    L.QueryAST = *QueryAST;
    L.Value = Value;
    Labels.push_back(L);

    errs() << "[SourceLabel] loaded label: type=" << L.Type
           << ", func=" << L.QueryAST.Function.Name
           << ", value=" << (int)L.Value << "\n";
  }

  return Labels;
}

//===----------------------------------------------------------------------===//
// Symbolic value parsing for compute-dense and memory-dense
//===----------------------------------------------------------------------===//

static uint8_t parseSymbolicValue(StringRef Type, StringRef ValueStr) {
  if (Type == "compute-dense") {
    // Parse compute type: "INT", "FLOAT", "SIMD", "INT|FLOAT", etc.
    uint8_t Mask = 0;
    SmallVector<StringRef, 3> Parts;
    ValueStr.split(Parts, '|', -1, false);
    
    for (StringRef Part : Parts) {
      Part = Part.trim();
      if (Part == "INT")
        Mask |= SCHED_COMPUTE_INT;
      else if (Part == "FLOAT")
        Mask |= SCHED_COMPUTE_FLOAT;
      else if (Part == "SIMD")
        Mask |= SCHED_COMPUTE_SIMD;
      else if (Part == "NONE")
        Mask = SCHED_COMPUTE_NONE;
      else
        errs() << "[SourceLabel] warning: unknown compute type '" << Part << "'\n";
    }
    return Mask ? Mask : SCHED_COMPUTE_INT; // default to INT if nothing parsed
  }
  
  if (Type == "memory-dense") {
    // Parse memory type: "STREAM", "RANDOM", "NONE"
    ValueStr = ValueStr.trim();
    if (ValueStr == "STREAM")
      return 1; // SCHED_MEMORY_STREAM
    else if (ValueStr == "RANDOM")
      return 2; // SCHED_MEMORY_RANDOM
    else if (ValueStr == "NONE")
      return 0;
    else {
      errs() << "[SourceLabel] warning: unknown memory type '" << ValueStr
             << "', using STREAM\n";
      return 1;
    }
  }
  
  // For other types, try to parse as integer
  unsigned Val;
  if (ValueStr.getAsInteger(0, Val)) {
    errs() << "[SourceLabel] warning: could not parse value '" << ValueStr
           << "', using 1\n";
    return 1;
  }
  return static_cast<uint8_t>(Val);
}

} // namespace sched_tag
