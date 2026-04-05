#include "SchedQLParser.h"
#include "llvm/Support/raw_ostream.h"
#include <cctype>

using namespace llvm;

namespace sched_tag {

//===----------------------------------------------------------------------===//
// Utility functions
//===----------------------------------------------------------------------===//

const char *instrTypeToString(InstrType T) {
  switch (T) {
  case InstrType::AtomicRMW:
    return "atomicrmw";
  case InstrType::CmpXchg:
    return "cmpxchg";
  case InstrType::Call:
    return "call";
  case InstrType::Load:
    return "load";
  case InstrType::Store:
    return "store";
  case InstrType::Alloca:
    return "alloca";
  case InstrType::Br:
    return "br";
  case InstrType::Switch:
    return "switch";
  case InstrType::Ret:
    return "ret";
  case InstrType::Add:
    return "add";
  case InstrType::FAdd:
    return "fadd";
  case InstrType::Mul:
    return "mul";
  case InstrType::FMul:
    return "fmul";
  case InstrType::Sub:
    return "sub";
  case InstrType::FSub:
    return "fsub";
  case InstrType::Div:
    return "div";
  case InstrType::FDiv:
    return "fdiv";
  }
  return "unknown";
}

//===----------------------------------------------------------------------===//
// Parsing primitives
//===----------------------------------------------------------------------===//

void SchedQLParser::skipWhitespace() {
  while (Pos < Input.size() && std::isspace(Input[Pos]))
    Pos++;
}

bool SchedQLParser::consume(char C) {
  skipWhitespace();
  if (Pos < Input.size() && Input[Pos] == C) {
    Pos++;
    return true;
  }
  return false;
}

bool SchedQLParser::consume(StringRef Str) {
  skipWhitespace();
  if (Input.substr(Pos).starts_with(Str)) {
    Pos += Str.size();
    return true;
  }
  return false;
}

char SchedQLParser::peek() const {
  if (Pos >= Input.size())
    return '\0';
  size_t P = Pos;
  while (P < Input.size() && std::isspace(Input[P]))
    P++;
  return P < Input.size() ? Input[P] : '\0';
}

bool SchedQLParser::lookahead(StringRef Str) const {
  size_t P = Pos;
  while (P < Input.size() && std::isspace(Input[P]))
    P++;
  return Input.substr(P).starts_with(Str);
}

bool SchedQLParser::lookaheadAlnum(size_t Offset) const {
  size_t P = Pos;
  while (P < Input.size() && std::isspace(Input[P]))
    P++;
  P += Offset;
  return P < Input.size() && std::isalnum(Input[P]);
}

//===----------------------------------------------------------------------===//
// Grammar rules
//===----------------------------------------------------------------------===//

std::optional<Query> SchedQLParser::parse() {
  skipWhitespace();
  auto Q = parseQuery();
  if (!Q)
    return std::nullopt;
  skipWhitespace();
  if (!eof()) {
    setError("unexpected trailing characters");
    return std::nullopt;
  }
  return Q;
}

// Query ::= "@" FunctionSpec "/" Target
std::optional<Query> SchedQLParser::parseQuery() {
  if (!consume('@')) {
    setError("expected '@' at start of query");
    return std::nullopt;
  }

  auto Func = parseFunctionSpec();
  if (!Func)
    return std::nullopt;

  if (!consume('/')) {
    setError("expected '/' after function spec");
    return std::nullopt;
  }

  auto Tgt = parseTarget();
  if (!Tgt)
    return std::nullopt;

  Query Q;
  Q.Function = *Func;
  Q.Tgt = *Tgt;
  return Q;
}

// FunctionSpec ::= Identifier | Identifier "::" Identifier | Identifier "[" Signature "]"
// Supports C++ qualified names like "Result::run"
std::optional<FunctionSpec> SchedQLParser::parseFunctionSpec() {
  auto Name = parseIdentifier();
  if (!Name)
    return std::nullopt;

  FunctionSpec Spec;
  Spec.Name = *Name;

  // Check for :: (C++ qualified name)
  while (true) {
    skipWhitespace();
    if (Pos + 1 < Input.size() && Input[Pos] == ':' && Input[Pos + 1] == ':') {
      Pos += 2; // consume ::
      auto Part = parseIdentifier();
      if (!Part)
        return std::nullopt;
      Spec.Name += "::" + *Part;
    } else {
      break;
    }
  }

  // Check for signature
  if (consume('[')) {
    auto Sig = parseSignature();
    if (!Sig)
      return std::nullopt;
    if (!consume(']')) {
      setError("expected ']' after signature");
      return std::nullopt;
    }
    Spec.Sig = *Sig;
  }

  return Spec;
}

// Signature ::= Type ("," Type)*
std::optional<Signature> SchedQLParser::parseSignature() {
  Signature Sig;

  auto T = parseType();
  if (!T)
    return std::nullopt;
  Sig.Types.push_back(*T);

  while (consume(',')) {
    T = parseType();
    if (!T)
      return std::nullopt;
    Sig.Types.push_back(*T);
  }

  return Sig;
}

// Target ::= LoopQuery
//         | BasicBlockQuery  
//         | InstructionQuery
//
// Note: loop and bb queries do NOT take a trailing /InstructionQuery.
// - Loop: instrumented at preheader, base pointers auto-collected
// - BB: instrumented at BB entry (first non-PHI)
// - Direct instruction query: for BB-level labeling based on instruction presence
std::optional<Target> SchedQLParser::parseTarget() {
  Target Tgt;

  // Try loop query: must be "loop[" to distinguish from "load[" etc.
  if (lookahead("loop[")) {
    auto Loop = parseLoopQuery();
    if (!Loop)
      return std::nullopt;
    Tgt.Loop = *Loop;
    
    // Reject trailing /instr - it's not meaningful for loop queries
    if (peek() == '/') {
      setError("loop query does not accept trailing /instruction; "
               "instrumentation is always at preheader");
      return std::nullopt;
    }
    return Tgt;
  }

  // Try basic block query: must be "bb" followed by non-alnum (not "br[")
  if (lookahead("bb") && !lookaheadAlnum(2)) {
    auto BB = parseBasicBlockQuery();
    if (!BB)
      return std::nullopt;
    Tgt.BB = *BB;

    // Reject trailing /instr - it's not meaningful for bb queries
    if (peek() == '/') {
      setError("bb query does not accept trailing /instruction; "
               "instrumentation is always at BB entry");
      return std::nullopt;
    }
    return Tgt;
  }

  // Direct instruction query (for BB-level labeling)
  auto Instr = parseInstructionQuery();
  if (!Instr)
    return std::nullopt;
  Tgt.Instruction = *Instr;
  return Tgt;
}

// LoopQuery ::= "loop" "[" PatternList "]"
std::optional<LoopQuery> SchedQLParser::parseLoopQuery() {
  if (!consume("loop")) {
    setError("expected 'loop'");
    return std::nullopt;
  }

  if (!consume('[')) {
    setError("expected '[' after 'loop'");
    return std::nullopt;
  }

  auto Patterns = parsePatternList();
  if (!Patterns)
    return std::nullopt;

  if (!consume(']')) {
    setError("expected ']' after pattern list");
    return std::nullopt;
  }

  LoopQuery LQ;
  LQ.Patterns = *Patterns;
  return LQ;
}

// BasicBlockQuery ::= "bb" "[" BlockSpec "]"
std::optional<BasicBlockQuery> SchedQLParser::parseBasicBlockQuery() {
  if (!consume("bb")) {
    setError("expected 'bb'");
    return std::nullopt;
  }

  if (!consume('[')) {
    setError("expected '[' after 'bb'");
    return std::nullopt;
  }

  auto Spec = parseBlockSpec();
  if (!Spec)
    return std::nullopt;

  if (!consume(']')) {
    setError("expected ']' after block spec");
    return std::nullopt;
  }

  BasicBlockQuery BBQ;
  BBQ.Spec = *Spec;
  return BBQ;
}

// BlockSpec ::= Identifier | "entry" | "exit"
std::optional<BlockSpec> SchedQLParser::parseBlockSpec() {
  BlockSpec Spec;

  if (consume("entry")) {
    Spec.IsEntry = true;
    return Spec;
  }

  if (consume("exit")) {
    Spec.IsExit = true;
    return Spec;
  }

  auto Name = parseIdentifier();
  if (!Name)
    return std::nullopt;
  Spec.Name = *Name;
  return Spec;
}

// InstructionQuery ::= InstructionType "[" PredicateList "]"
std::optional<InstructionQuery> SchedQLParser::parseInstructionQuery() {
  auto Type = parseInstrType();
  if (!Type)
    return std::nullopt;

  if (!consume('[')) {
    setError("expected '[' after instruction type");
    return std::nullopt;
  }

  auto Predicates = parsePredicateList();
  if (!Predicates)
    return std::nullopt;

  if (!consume(']')) {
    setError("expected ']' after predicate list");
    return std::nullopt;
  }

  InstructionQuery IQ;
  IQ.Type = *Type;
  IQ.Predicates = *Predicates;
  return IQ;
}

// PatternList ::= Pattern (";" Pattern)*
std::optional<SmallVector<Pattern, 2>> SchedQLParser::parsePatternList() {
  SmallVector<Pattern, 2> Patterns;

  auto P = parsePattern();
  if (!P)
    return std::nullopt;
  Patterns.push_back(*P);

  while (consume(';')) {
    P = parsePattern();
    if (!P)
      return std::nullopt;
    Patterns.push_back(*P);
  }

  return Patterns;
}

// Pattern ::= Category "=" TypeSpec
// TypeSpec ::= Type | Type ":" Identifier
std::optional<Pattern> SchedQLParser::parsePattern() {
  auto Cat = parsePatternCategory();
  if (!Cat)
    return std::nullopt;

  if (!consume('=')) {
    setError("expected '=' after pattern category");
    return std::nullopt;
  }

  auto Type = parseInstrType();
  if (!Type)
    return std::nullopt;

  Pattern P;
  P.Category = *Cat;
  P.Type = *Type;

  if (consume(':')) {
    auto Id = parseIdentifier();
    if (!Id)
      return std::nullopt;
    P.Identifier = *Id;
  }

  return P;
}

// Category ::= "contains" | "in" | "not_in"
std::optional<PatternCategory> SchedQLParser::parsePatternCategory() {
  if (consume("contains"))
    return PatternCategory::Contains;
  if (consume("not_in"))
    return PatternCategory::NotIn;
  if (consume("in"))
    return PatternCategory::In;

  setError("expected pattern category (contains, in, not_in)");
  return std::nullopt;
}

// PredicateList ::= Predicate ("," Predicate)*
std::optional<SmallVector<Predicate, 2>>
SchedQLParser::parsePredicateList() {
  SmallVector<Predicate, 2> Predicates;

  auto P = parsePredicate();
  if (!P)
    return std::nullopt;
  Predicates.push_back(*P);

  while (consume(',')) {
    P = parsePredicate();
    if (!P)
      return std::nullopt;
    Predicates.push_back(*P);
  }

  return Predicates;
}

// Predicate ::= Position | "func" "=" Identifier | "var" "=" Identifier
std::optional<Predicate> SchedQLParser::parsePredicate() {
  Predicate P;

  // Try position
  if (auto Pos = parsePosition()) {
    P.Pos = *Pos;
    return P;
  }

  // Try func=...
  if (consume("func")) {
    if (!consume('=')) {
      setError("expected '=' after 'func'");
      return std::nullopt;
    }
    auto Id = parseIdentifier();
    if (!Id)
      return std::nullopt;
    P.FuncName = *Id;
    return P;
  }

  // Try var=...
  if (consume("var")) {
    if (!consume('=')) {
      setError("expected '=' after 'var'");
      return std::nullopt;
    }
    auto Id = parseIdentifier();
    if (!Id)
      return std::nullopt;
    P.VarName = *Id;
    return P;
  }

  setError("expected predicate (first, last, entry, func=..., var=...)");
  return std::nullopt;
}

// Position ::= "first" | "last" | "entry"
std::optional<Position> SchedQLParser::parsePosition() {
  size_t SavePos = Pos;
  if (consume("first"))
    return Position::First;
  if (consume("last"))
    return Position::Last;
  if (consume("entry"))
    return Position::Entry;
  Pos = SavePos; // Restore position on failure
  return std::nullopt;
}

// InstructionType ::= "atomicrmw" | "cmpxchg" | "call" | "load" | "store"
//                  | "alloca" | "br" | "switch" | "ret" | "add" | "fadd"
//                  | "mul" | "fmul" | "sub" | "fsub" | "div" | "fdiv"
std::optional<InstrType> SchedQLParser::parseInstrType() {
  // Try longest matches first to avoid ambiguity
  if (consume("atomicrmw"))
    return InstrType::AtomicRMW;
  if (consume("cmpxchg"))
    return InstrType::CmpXchg;
  if (consume("alloca"))
    return InstrType::Alloca;
  if (consume("switch"))
    return InstrType::Switch;
  if (consume("call"))
    return InstrType::Call;
  if (consume("load"))
    return InstrType::Load;
  if (consume("store"))
    return InstrType::Store;
  if (consume("fadd"))
    return InstrType::FAdd;
  if (consume("fmul"))
    return InstrType::FMul;
  if (consume("fsub"))
    return InstrType::FSub;
  if (consume("fdiv"))
    return InstrType::FDiv;
  if (consume("ret"))
    return InstrType::Ret;
  if (consume("add"))
    return InstrType::Add;
  if (consume("mul"))
    return InstrType::Mul;
  if (consume("sub"))
    return InstrType::Sub;
  if (consume("div"))
    return InstrType::Div;
  if (consume("br"))
    return InstrType::Br;

  setError("expected instruction type");
  return std::nullopt;
}

// Identifier ::= [a-zA-Z_][a-zA-Z0-9_]*
std::optional<std::string> SchedQLParser::parseIdentifier() {
  skipWhitespace();
  if (Pos >= Input.size() ||
      !(std::isalpha(Input[Pos]) || Input[Pos] == '_')) {
    setError("expected identifier");
    return std::nullopt;
  }

  size_t Start = Pos;
  while (Pos < Input.size() &&
         (std::isalnum(Input[Pos]) || Input[Pos] == '_'))
    Pos++;

  return Input.substr(Start, Pos - Start).str();
}

// Type ::= identifier or primitive type (e.g., "void*", "i32")
// For simplicity, we parse it as an identifier with optional '*' suffix
std::optional<std::string> SchedQLParser::parseType() {
  skipWhitespace();
  size_t Start = Pos;

  // Parse identifier-like type (e.g., "void", "i32", "int")
  if (Pos < Input.size() &&
      (std::isalpha(Input[Pos]) || Input[Pos] == '_')) {
    while (Pos < Input.size() &&
           (std::isalnum(Input[Pos]) || Input[Pos] == '_'))
      Pos++;
  } else {
    setError("expected type identifier");
    return std::nullopt;
  }

  // Parse optional pointer suffix(es)
  skipWhitespace();
  while (Pos < Input.size() && Input[Pos] == '*') {
    Pos++;
    skipWhitespace();
  }

  return Input.substr(Start, Pos - Start).str();
}

} // namespace sched_tag
