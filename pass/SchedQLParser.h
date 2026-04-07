#ifndef SCHEDQL_PARSER_H
#define SCHEDQL_PARSER_H

//===----------------------------------------------------------------------===//
// SchedQLParser.h — Parser for SchedQL query language
//
// SchedQL is a DSL for locating specific IR instruction patterns within
// functions, loops, or basic blocks. Used to apply source-level tags.
//
// Example query:
//   @lock_free_queue_push[void*, void*]/loop[contains=cmpxchg]/cmpxchg[first]
//
// This locates:
//   - Function: lock_free_queue_push with signature (void*, void*)
//   - Within: a loop that contains cmpxchg instructions
//   - Target: the first cmpxchg instruction in that loop
//===----------------------------------------------------------------------===//

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include <optional>
#include <string>
#include <variant>

namespace sched_tag {

//===----------------------------------------------------------------------===//
// AST nodes for SchedQL queries
//===----------------------------------------------------------------------===//

/// Instruction type (atomicrmw, cmpxchg, call, load, store, add, etc.)
enum class InstrType {
  AtomicRMW,
  CmpXchg,
  Call,
  Load,
  Store,
  Alloca,
  Br,
  Switch,
  Ret,
  Add,
  FAdd,
  Mul,
  FMul,
  Sub,
  FSub,
  Div,
  FDiv,
};

/// Predicate position (first, last)
enum class Position { First, Last };

/// Predicate in instruction query: first | last | func=foo | var=bar
struct Predicate {
  std::optional<Position> Pos;
  std::optional<std::string> FuncName;
  std::optional<std::string> VarName;
};

/// Instruction query: InstructionType[PredicateList]
struct InstructionQuery {
  InstrType Type;
  llvm::SmallVector<Predicate, 2> Predicates;
};

/// Pattern category: contains | in | not_in
enum class PatternCategory { Contains, In, NotIn };

/// Pattern: Category=Type or Category=Type:identifier
struct Pattern {
  PatternCategory Category;
  InstrType Type;
  std::optional<std::string> Identifier;
};

/// Loop query: loop[PatternList]
struct LoopQuery {
  llvm::SmallVector<Pattern, 2> Patterns;
};

/// BB pattern category: entry | exit | name | contains | in | not_in
enum class BBPatternCategory {
  Entry,    // entry block
  Exit,     // exit block (has return instruction)
  Name,     // specific block by name
  Contains, // contains instruction of type
  In,       // dominated by instruction
  NotIn     // not dominated by instruction
};

/// BB pattern: entry | exit | name=foo | contains=Type | in=Type | not_in=Type
struct BBPattern {
  BBPatternCategory Category;
  std::optional<InstrType> Type;     // For contains/in/not_in
  std::optional<std::string> Name;   // For name=
  std::optional<std::string> TypeId; // Type:identifier form
};

/// Basic block query: bb[BBPatternList]
struct BasicBlockQuery {
  llvm::SmallVector<BBPattern, 2> Patterns;
};

/// Target: loop/instruction | bb/instruction | instruction
struct Target {
  std::optional<LoopQuery> Loop;
  std::optional<BasicBlockQuery> BB;
  InstructionQuery Instruction;
};

/// Function signature: Type, Type, ...
struct Signature {
  llvm::SmallVector<std::string, 4> Types; // e.g., ["void*", "void*"]
};

/// Function specification: Identifier or Identifier[Signature]
/// Supports both mangled and demangled names (e.g., "Class::method")
struct FunctionSpec {
  std::string Name;
  std::optional<Signature> Sig;
};

/// Top-level query: @FunctionSpec/Target
struct Query {
  FunctionSpec Function;
  Target Tgt;
};

//===----------------------------------------------------------------------===//
// SchedQL parser
//===----------------------------------------------------------------------===//

class SchedQLParser {
public:
  explicit SchedQLParser(llvm::StringRef Input) : Input(Input), Pos(0) {}

  /// Parse a complete SchedQL query.
  /// Returns std::nullopt on parse error (with error message in getError()).
  std::optional<Query> parse();

  /// Get the last parse error message.
  llvm::StringRef getError() const { return ErrorMsg; }

private:
  llvm::StringRef Input;
  size_t Pos;
  std::string ErrorMsg;

  // Parsing primitives
  void skipWhitespace();
  bool consume(char C);
  bool consume(llvm::StringRef Str);
  char peek() const;
  bool lookahead(llvm::StringRef Str) const;
  bool lookaheadAlnum(size_t Offset) const;
  bool eof() const { return Pos >= Input.size(); }
  void setError(const std::string &Msg) { ErrorMsg = Msg; }

  // Grammar rules
  std::optional<Query> parseQuery();
  std::optional<FunctionSpec> parseFunctionSpec();
  std::optional<Signature> parseSignature();
  std::optional<Target> parseTarget();
  std::optional<LoopQuery> parseLoopQuery();
  std::optional<BasicBlockQuery> parseBasicBlockQuery();
  std::optional<llvm::SmallVector<BBPattern, 2>> parseBBPatternList();
  std::optional<BBPattern> parseBBPattern();
  std::optional<BBPatternCategory> parseBBPatternCategory();
  std::optional<InstructionQuery> parseInstructionQuery();
  std::optional<llvm::SmallVector<Pattern, 2>> parsePatternList();
  std::optional<Pattern> parsePattern();
  std::optional<PatternCategory> parsePatternCategory();
  std::optional<llvm::SmallVector<Predicate, 2>> parsePredicateList();
  std::optional<Predicate> parsePredicate();
  std::optional<Position> parsePosition();
  std::optional<InstrType> parseInstrType();
  std::optional<std::string> parseIdentifier();
  std::optional<std::string> parseType();
};

//===----------------------------------------------------------------------===//
// Utility functions
//===----------------------------------------------------------------------===//

/// Convert InstrType enum to string (for debugging).
const char *instrTypeToString(InstrType T);

} // namespace sched_tag

#endif // SCHEDQL_PARSER_H
