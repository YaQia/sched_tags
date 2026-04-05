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
  std::string Type;           // e.g., "atomic-dense", "compute-dense"
  Query QueryAST;             // Parsed SchedQL query
  uint8_t Value;              // Value to store in the tag field:
                              // - For compute-dense: bitmask (INT|FLOAT|SIMD)
                              // - For memory-dense: STREAM(1) or RANDOM(2)
                              // - For boolean fields: 1 (or 0 to disable)
                              // Default: 1
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

/// Execute a SchedQL query on a function.
/// Returns a DensityResult containing matching instructions/regions.
/// TypeMask is the value to store in the tag field (from label's "value" field).
DensityResult executeQuery(llvm::Function &F, const Query &Q,
                           llvm::FunctionAnalysisManager &AM,
                           uint8_t TypeMask);

//===----------------------------------------------------------------------===//
// JSON parsing
//===----------------------------------------------------------------------===//

/// Parse sched_tags.json from the given file path.
/// Returns empty vector on error (with diagnostic to stderr).
llvm::SmallVector<SourceLabel, 4> parseSchedTagsJSON(llvm::StringRef Path);

} // namespace sched_tag

#endif // SOURCE_LABEL_ANALYSIS_H
