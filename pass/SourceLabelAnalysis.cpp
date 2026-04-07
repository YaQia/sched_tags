#include "SourceLabelAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ValueTracking.h"
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
      errs()
          << "[SourceLabel] warning: signature checking not yet implemented\n";
    }

    // Execute query — use ranged overload if EndQueryAST is present
    DensityResult MatchResult;
    if (Label.hasEndQuery()) {
      MatchResult = executeQuery(F, {Label.QueryAST, *Label.EndQueryAST}, AM,
                                 Label.Value, Label.MagicVars);
    } else {
      MatchResult =
          executeQuery(F, Label.QueryAST, AM, Label.Value, Label.MagicVars);
    }

    if (!MatchResult.empty()) {
      errs() << "[SourceLabel] matched query in " << F.getName()
             << ": type=" << Label.Type << ", value=" << (int)Label.Value;
      if (Label.hasEndQuery()) {
        errs() << " (ranged)";
      }
      errs() << "\n";

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
    // Match both CallInst and InvokeInst (for languages with exceptions like
    // C++/Rust)
    return isa<CallBase>(I);
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

/// Check if a call/invoke instruction calls a function with a given name.
/// Handles both direct calls and calls through pointer casts.
/// Supports CallInst and InvokeInst via CallBase.
static bool callsFunction(const Instruction &I, StringRef FuncName) {
  const CallBase *CB = dyn_cast<CallBase>(&I);
  if (!CB)
    return false;

  Value *CalleeV = CB->getCalledOperand()->stripPointerCasts();
  Function *Callee = dyn_cast<Function>(CalleeV);
  if (!Callee)
    return false;

  // Check exact match first
  if (Callee->getName() == FuncName)
    return true;

  // Check demangled name for C++ functions
  std::string Demangled = llvm::demangle(Callee->getName().str());
  if (Demangled.find(FuncName.str()) != std::string::npos)
    return true;

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
    // Handle func= predicate (for call instructions)
    if (Pred.FuncName.has_value()) {
      SmallVector<Instruction *, 4> Filtered;
      for (Instruction *I : Matches) {
        if (callsFunction(*I, *Pred.FuncName))
          Filtered.push_back(I);
      }
      Matches = std::move(Filtered);
    }

    // Handle var= predicate (for memory instructions)
    if (Pred.VarName.has_value()) {
      SmallVector<Instruction *, 4> Filtered;
      for (Instruction *I : Matches) {
        // Check if the instruction uses a variable with the given name
        // This includes global variables, alloca instructions, and arguments
        for (Use &U : I->operands()) {
          // Use getUnderlyingObject to strip all pointer casts and GEPs
          Value *V = getUnderlyingObject(U.get());

          // Check global variable name
          if (auto *GV = dyn_cast<GlobalVariable>(V)) {
            if (GV->getName() == *Pred.VarName) {
              Filtered.push_back(I);
              break;
            }
          }
          // Check instruction name (e.g., alloca with name)
          if (V->hasName() && V->getName() == *Pred.VarName) {
            Filtered.push_back(I);
            break;
          }
        }
      }
      Matches = std::move(Filtered);
    }

    // Handle position predicates
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
      }
    }
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
        errs()
            << "[SourceLabel] warning: 'not_in' pattern not yet implemented\n";
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

// Note: extractBasePointers is removed - we now use collectBasePointers from
// SchedTagCommon.

//===----------------------------------------------------------------------===//
// Magic variable lookup
//===----------------------------------------------------------------------===//

/// Find SSA values by name within a set of basic blocks.
/// Searches for:
///   1. Instructions with matching names (e.g., %mutex_ptr)
///   2. Function arguments with matching names
///   3. Global variables with matching names
/// Returns a vector of pointer-typed values for bloom filter computation.
static SmallVector<Value *, 4> findValuesByName(ArrayRef<BasicBlock *> BBs,
                                                ArrayRef<std::string> VarNames,
                                                Function &F) {
  SmallPtrSet<Value *, 4> Seen;
  SmallVector<Value *, 4> Results;

  // Helper to add a value if it's pointer-typed and not already seen
  auto TryAdd = [&](Value *V) {
    if (!V || !V->getType()->isPointerTy())
      return;
    if (Seen.insert(V).second)
      Results.push_back(V);
  };

  for (const std::string &Name : VarNames) {
    StringRef NameRef(Name);

    // 1. Search function arguments
    for (Argument &Arg : F.args()) {
      if (Arg.getName() == NameRef) {
        TryAdd(&Arg);
      }
    }

    // 2. Search global variables
    if (GlobalVariable *GV = F.getParent()->getGlobalVariable(Name)) {
      TryAdd(GV);
    }

    // 3. Search instructions in the specified BBs
    for (BasicBlock *BB : BBs) {
      for (Instruction &I : *BB) {
        if (I.getName() == NameRef) {
          TryAdd(&I);
        }
        // Also check if any operand is a named value we're looking for
        // (useful for finding alloca'd variables used in loads/stores)
        for (Use &U : I.operands()) {
          if (U->getName() == NameRef) {
            TryAdd(U.get());
          }
        }
      }
    }
  }

  return Results;
}

/// Collect base pointers based on magic_vars or fallback to automatic
/// detection. If MagicVars is non-empty, searches for named variables.
/// Otherwise, falls back to collectBasePointers (automatic atomic op
/// detection).
static SmallVector<Value *, 4> collectMagicPointers(
    ArrayRef<BasicBlock *> BBs, ArrayRef<std::string> MagicVars, Function &F,
    const DominatorTree *DT = nullptr, BasicBlock *InsertionPoint = nullptr) {
  if (!MagicVars.empty()) {
    // Use explicit variable names
    auto Ptrs = findValuesByName(BBs, MagicVars, F);
    if (Ptrs.empty()) {
      errs() << "[SourceLabel] warning: no values found for magic_vars [";
      for (size_t i = 0; i < MagicVars.size(); ++i) {
        if (i > 0)
          errs() << ", ";
        errs() << MagicVars[i];
      }
      errs() << "], falling back to automatic detection\n";
      return collectBasePointers(BBs, DT, InsertionPoint);
    }
    return Ptrs;
  }
  // Fallback: automatic detection of atomic operations
  return collectBasePointers(BBs, DT, InsertionPoint);
}

//===----------------------------------------------------------------------===//
// Query execution
//===----------------------------------------------------------------------===//

/// Find matching instructions for a direct instruction query across all BBs.
static SmallVector<Instruction *, 8>
findMatchingInstructions(Function &F, const InstructionQuery &IQ) {
  SmallVector<Instruction *, 8> AllMatches;
  for (BasicBlock &BB : F) {
    auto Matches = findInstructionsInBB(BB, IQ);
    AllMatches.append(Matches.begin(), Matches.end());
  }
  return AllMatches;
}

/// Execute a ranged query (start/end pair) for precise range labeling.
DensityResult executeQuery(Function &F,
                           std::pair<const Query &, const Query &> Range,
                           FunctionAnalysisManager &AM, uint8_t TypeMask,
                           ArrayRef<std::string> MagicVars) {
  DensityResult Result;
  const auto &[StartQ, EndQ] = Range;

  // Ranged queries only support direct instruction queries (not loop/bb)
  if (StartQ.Tgt.Loop.has_value() || StartQ.Tgt.BB.has_value()) {
    errs() << "[SourceLabel] warning: ranged queries only support direct "
           << "instruction queries for start position\n";
    return Result;
  }
  if (EndQ.Tgt.Loop.has_value() || EndQ.Tgt.BB.has_value()) {
    errs() << "[SourceLabel] warning: ranged queries only support direct "
           << "instruction queries for end position\n";
    return Result;
  }

  auto StartInsts = findMatchingInstructions(F, StartQ.Tgt.Instruction);
  auto EndInsts = findMatchingInstructions(F, EndQ.Tgt.Instruction);

  // Pair start and end instructions by order
  size_t NumPairs = std::min(StartInsts.size(), EndInsts.size());
  if (StartInsts.size() != EndInsts.size()) {
    errs() << "[SourceLabel] warning: mismatched start/end counts ("
           << StartInsts.size() << " starts, " << EndInsts.size() << " ends), "
           << "pairing " << NumPairs << "\n";
  }

  for (size_t i = 0; i < NumPairs; ++i) {
    RangedRegion RR;
    RR.StartInst = StartInsts[i];
    RR.EndInst = EndInsts[i];
    RR.TypeMask = TypeMask;

    // Collect base pointers from both start and end BBs
    SmallVector<BasicBlock *, 2> BBs;
    BBs.push_back(RR.StartInst->getParent());
    if (RR.EndInst->getParent() != RR.StartInst->getParent()) {
      BBs.push_back(RR.EndInst->getParent());
    }
    RR.BasePointers = collectMagicPointers(BBs, MagicVars, F);

    Result.RangedRegions.push_back(RR);

    errs() << "[SourceLabel] matched ranged region: start="
           << RR.StartInst->getParent()->getName() << "/"
           << RR.StartInst->getOpcodeName()
           << ", end=" << RR.EndInst->getParent()->getName() << "/"
           << RR.EndInst->getOpcodeName()
           << " (bases=" << RR.BasePointers.size() << ")\n";
  }

  return Result;
}

/// Execute a single query for loop/BB/instruction labeling.
DensityResult executeQuery(Function &F, const Query &Q,
                           FunctionAnalysisManager &AM, uint8_t TypeMask,
                           ArrayRef<std::string> MagicVars) {
  DensityResult Result;

  // Handle loop-based queries
  if (Q.Tgt.Loop.has_value()) {
    auto &LI = AM.getResult<LoopAnalysis>(F);
    const auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
    auto Loops = findMatchingLoops(LI, *Q.Tgt.Loop);

    for (Loop *L : Loops) {
      LoopRegion LR;
      LR.Preheader = L->getLoopPreheader();
      if (!LR.Preheader) {
        errs() << "[SourceLabel] warning: loop has no preheader in "
               << L->getHeader()->getParent()->getName()
               << ", cannot instrument (consider using opt -loop-simplify "
                  "first)\n";
        continue;
      }
      LR.TypeMask = TypeMask;

      SmallVector<BasicBlock *, 8> LoopBBs(L->blocks().begin(),
                                           L->blocks().end());
      LR.BasePointers =
          collectMagicPointers(LoopBBs, MagicVars, F, &DT, LR.Preheader);
      Result.Loops.push_back(LR);

      errs() << "[SourceLabel] matched loop at " << LR.Preheader->getName()
             << " (bases=" << LR.BasePointers.size() << ")\n";
    }
    return Result;
  }

  // Handle basic block queries
  if (Q.Tgt.BB.has_value()) {
    const auto &Spec = Q.Tgt.BB->Spec;
    BasicBlock *TargetBB = nullptr;

    if (Spec.IsEntry) {
      TargetBB = &F.getEntryBlock();
    } else if (Spec.IsExit) {
      for (BasicBlock &BB : F) {
        if (isa<ReturnInst>(BB.getTerminator())) {
          TargetBB = &BB;
          break;
        }
      }
    } else if (Spec.Name.has_value()) {
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
      BasicBlock *BBPtr = TargetBB;
      BR.BasePointers =
          collectMagicPointers(ArrayRef<BasicBlock *>(&BBPtr, 1), MagicVars, F);
      Result.StandaloneBBs.push_back(BR);
      errs() << "[SourceLabel] matched BB " << TargetBB->getName()
             << " (bases=" << BR.BasePointers.size() << ")\n";
    }
    return Result;
  }

  // Handle direct instruction queries (BB-level labeling)
  for (BasicBlock &BB : F) {
    auto Matches = findInstructionsInBB(BB, Q.Tgt.Instruction);
    if (!Matches.empty()) {
      BBRegion BR;
      BR.BB = &BB;
      BR.TypeMask = TypeMask;
      BasicBlock *BBPtr = &BB;
      BR.BasePointers =
          collectMagicPointers(ArrayRef<BasicBlock *>(&BBPtr, 1), MagicVars, F);
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

/// Parsed query result: start query and optional end query.
struct ParsedQuery {
  Query Start;
  std::optional<Query> End;
};

/// Parse the "query" field from JSON.
/// Supports both string form ("@func/...") and object form ({ "start": "...",
/// "end": "..." }). Returns nullopt on parse error (with diagnostics to
/// stderr).
static std::optional<ParsedQuery> parseQueryField(const json::Value *QueryVal) {
  if (!QueryVal) {
    errs() << "[SourceLabel] warning: label missing 'query' field\n";
    return std::nullopt;
  }

  ParsedQuery Result;

  if (auto QueryStr = QueryVal->getAsString()) {
    // String form: single query
    SchedQLParser Parser(*QueryStr);
    auto Parsed = Parser.parse();
    if (!Parsed) {
      errs() << "[SourceLabel] error parsing query '" << *QueryStr
             << "': " << Parser.getError() << "\n";
      return std::nullopt;
    }
    Result.Start = *Parsed;
    return Result;
  }

  if (const json::Object *QueryObj = QueryVal->getAsObject()) {
    // Object form: { "start": "...", "end": "..." }
    auto StartStr = QueryObj->getString("start");
    if (!StartStr) {
      errs() << "[SourceLabel] error: query object missing 'start' field\n";
      return std::nullopt;
    }

    SchedQLParser StartParser(*StartStr);
    auto StartParsed = StartParser.parse();
    if (!StartParsed) {
      errs() << "[SourceLabel] error parsing start query '" << *StartStr
             << "': " << StartParser.getError() << "\n";
      return std::nullopt;
    }
    Result.Start = *StartParsed;

    // Parse end query if present
    if (auto EndStr = QueryObj->getString("end")) {
      SchedQLParser EndParser(*EndStr);
      auto EndParsed = EndParser.parse();
      if (!EndParsed) {
        errs() << "[SourceLabel] error parsing end query '" << *EndStr
               << "': " << EndParser.getError() << "\n";
        return std::nullopt;
      }
      Result.End = *EndParsed;
    }

    return Result;
  }

  errs() << "[SourceLabel] error: 'query' must be string or object\n";
  return std::nullopt;
}

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
    if (!Type) {
      errs() << "[SourceLabel] warning: label missing 'type' field\n";
      continue;
    }

    // Parse query field
    auto ParsedQ = parseQueryField(LabelObj->get("query"));
    if (!ParsedQ)
      continue;

    // Validate: unshared requires both start and end
    if (*Type == "unshared" && !ParsedQ->End) {
      errs()
          << "[SourceLabel] error: 'unshared' label requires query with both "
          << "'start' and 'end' fields for precise range control\n";
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
    L.QueryAST = ParsedQ->Start;
    L.EndQueryAST = ParsedQ->End;
    L.Value = Value;

    // Parse magic_vars array (optional)
    if (const json::Array *MagicVarsArray = LabelObj->getArray("magic_vars")) {
      for (const auto &VarVal : *MagicVarsArray) {
        if (auto VarStr = VarVal.getAsString()) {
          L.MagicVars.push_back(VarStr->str());
        }
      }
    }

    Labels.push_back(L);

    errs() << "[SourceLabel] loaded label: type=" << L.Type
           << ", func=" << L.QueryAST.Function.Name;
    if (L.hasEndQuery()) {
      errs() << " (ranged: start→end)";
    }
    errs() << ", value=" << (int)L.Value;
    if (!L.MagicVars.empty()) {
      errs() << ", magic_vars=[";
      for (size_t i = 0; i < L.MagicVars.size(); ++i) {
        if (i > 0)
          errs() << ", ";
        errs() << L.MagicVars[i];
      }
      errs() << "]";
    }
    errs() << "\n";
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
        errs() << "[SourceLabel] warning: unknown compute type '" << Part
               << "'\n";
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
