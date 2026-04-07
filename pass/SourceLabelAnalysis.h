#ifndef SOURCE_LABEL_ANALYSIS_H
#define SOURCE_LABEL_ANALYSIS_H

//===----------------------------------------------------------------------===//
// SourceLabelAnalysis.h — Source-level tag analysis using SchedQL queries
//
// Reads sched_tags.json from the module directory and executes SchedQL
// queries to locate specific IR instructions/regions that should be tagged.
// Complements the automatic density analyses (ComputeDense, AtomicDense).
//===----------------------------------------------------------------------===//

#include "SchedQLParser.h"
#include "SchedTagCommon.h"
#include "llvm/IR/PassManager.h"
#include <string>

namespace sched_tag {

//===----------------------------------------------------------------------===//
// Label specification from JSON
//===----------------------------------------------------------------------===//

struct SourceLabel {
  std::string Type;           // e.g., "atomic-dense", "compute-dense", "unshared"
  Query QueryAST;             // Parsed SchedQL query (start position for ranged labels)
  std::optional<Query> EndQueryAST;  // End position query (required for "unshared")
                              // When present, QueryAST specifies start position
                              // and EndQueryAST specifies end position.
                              // For non-ranged labels, EndQueryAST is ignored.
  uint8_t Value;              // Value to store in the tag field:
                              // - For compute-dense: bitmask (INT|FLOAT|SIMD)
                              // - For memory-dense: STREAM(1) or RANDOM(2)
                              // - For boolean fields: 1 (or 0 to disable)
                              // Default: 1
  std::optional<uint64_t> StaticMagic; // Hardcoded magic number (overrides bloom filter)
  llvm::SmallVector<std::string, 4> MagicVars;  // Variable names for bloom filter
                              // If non-empty, Pass will search for these variable
                              // names in the matched region and use their addresses
                              // for bloom filter computation (atomic_magic/unshared_magic).
                              // If empty, falls back to automatic detection (atomic ops).
  
  /// Check if this label requires precise range (start and end).
  /// Currently only "unshared" requires this.
  bool requiresRange() const { return Type == "unshared"; }
  
  /// Check if this label has an end query.
  bool hasEndQuery() const { return EndQueryAST.has_value(); }
};

//===----------------------------------------------------------------------===//
// Source label instrumentation result
//===----------------------------------------------------------------------===//

/// Result structure that preserves the label type for each region.
/// This allows the pass to correctly map label types to struct sched_hint fields.
struct SourceLabelResult {
  std::string LabelType;      // e.g., "atomic-dense", "branch-dense"
  DensityResult Regions;      // Regions matched by this label's query
};

/// Combined result from all source labels.
struct SourceLabelResults {
  llvm::SmallVector<SourceLabelResult, 4> Labels;
  
  bool empty() const { return Labels.empty(); }
};

//===----------------------------------------------------------------------===//
// SourceLabelAnalysis — per-function query execution
//===----------------------------------------------------------------------===//

/// Executes SchedQL queries on a function to produce instrumentation plans.
/// The analysis is stateless — it receives a list of queries and returns
/// a SourceLabelResults containing all matched labels with their types.
struct SourceLabelAnalysis : public llvm::AnalysisInfoMixin<SourceLabelAnalysis> {
public:
  using Result = SourceLabelResults;

  /// Set the list of source labels to query (called once per module).
  static void setLabels(llvm::SmallVector<SourceLabel, 4> Labels);

  Result run(llvm::Function &F, llvm::FunctionAnalysisManager &);

private:
  static llvm::AnalysisKey Key;
  friend struct llvm::AnalysisInfoMixin<SourceLabelAnalysis>;
};

//===----------------------------------------------------------------------===//
// Query execution
//===----------------------------------------------------------------------===//

/// Check if a label type requires bloom filter (base pointers).
/// Only atomic-dense and unshared need magic pointers.
inline bool labelNeedsBloomFilter(llvm::StringRef Type) {
  return Type == "atomic-dense" || Type == "unshared";
}

/// Execute a ranged query (start/end pair) for precise range labeling.
/// Used for labels like "unshared" where the scheduler needs exact boundaries.
DensityResult executeQueryRange(llvm::Function &F, const SourceLabel &Label);

/// Execute a loop-based query.
/// Finds all matching loops and instruments them.
DensityResult executeQueryLoop(llvm::Function &F, const SourceLabel &Label,
                               llvm::FunctionAnalysisManager &AM);

/// Execute a basic block query.
DensityResult executeQueryBB(llvm::Function &F, const SourceLabel &Label);

/// Execute a direct instruction query (BB-level labeling based on instruction presence).
DensityResult executeQueryInstruction(llvm::Function &F, const SourceLabel &Label);

//===----------------------------------------------------------------------===//
// JSON parsing
//===----------------------------------------------------------------------===//

/// Parse sched_tags.json from the given file path.
/// Returns empty vector on error (with diagnostic to stderr).
llvm::SmallVector<SourceLabel, 4> parseSchedTagsJSON(llvm::StringRef Path);

} // namespace sched_tag

#endif // SOURCE_LABEL_ANALYSIS_H
