// Copyright 2022 The Centipede Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "./centipede/analyze_corpora.h"

#include <algorithm>
#include <cstddef>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "./centipede/binary_info.h"
#include "./centipede/control_flow.h"
#include "./centipede/corpus.h"
#include "./centipede/corpus_io.h"
#include "./centipede/coverage.h"
#include "./centipede/feature.h"
#include "./centipede/pc_info.h"
#include "./centipede/workdir.h"
#include "./common/defs.h"
#include "./common/logging.h"
#include "./common/remote_file.h"

namespace fuzztest::internal {

namespace {

std::vector<CorpusRecord> ReadCorpora(std::string_view binary_name,
                                      std::string_view binary_hash,
                                      std::string_view workdir_path) {
  WorkDir workdir(std::string(workdir_path), std::string(binary_name),
                  std::string(binary_hash), /*my_shard_index=*/0);
  std::vector<std::string> corpus_paths;
  CHECK_OK(
      RemoteGlobMatch(workdir.CorpusFilePaths().AllShardsGlob(), corpus_paths));
  std::vector<std::string> features_paths;
  CHECK_OK(RemoteGlobMatch(workdir.FeaturesFilePaths().AllShardsGlob(),
                           features_paths));

  CHECK_EQ(corpus_paths.size(), features_paths.size());
  std::vector<CorpusRecord> corpus;
  for (int i = 0; i < corpus_paths.size(); ++i) {
    LOG(INFO) << "Reading corpus at: " << corpus_paths[i];
    LOG(INFO) << "Reading features at: " << features_paths[i];
    ReadShard(corpus_paths[i], features_paths[i],
              [&corpus](ByteArray input, FeatureVec features) {
                corpus.push_back({std::move(input), std::move(features)});
              });
  }
  return corpus;
}

BinaryInfo ReadBinaryInfo(std::string_view binary_name,
                          std::string_view binary_hash,
                          std::string_view workdir_path) {
  WorkDir workdir(std::string(workdir_path), std::string(binary_name),
                  std::string(binary_hash), /*my_shard_index=*/0);
  BinaryInfo ret;
  ret.Read(workdir.BinaryInfoDirPath());
  return ret;
}

AnalyzeCorporaResults AnalyzeCorpora(const BinaryInfo &binary_info,
                                     const std::vector<CorpusRecord> &a,
                                     const std::vector<CorpusRecord> &b) {
  // `a_pcs` will contain all PCs covered by `a`.
  absl::flat_hash_set<size_t> a_pcs;
  absl::flat_hash_map<size_t, CorpusRecord> a_pc_to_corpus;
  for (const auto &record : a) {
    for (const auto &feature : record.features) {
      if (!feature_domains::kPCs.Contains(feature)) continue;
      auto pc = ConvertPCFeatureToPcIndex(feature);
      a_pcs.insert(pc);
      a_pc_to_corpus.insert({pc, std::move(record)});
    }
  }

  // `b_only_pcs` will contain PCs covered by `b` but not by `a`.
  // `b_unique_indices` are indices of inputs that have PCs from `b_only_pcs`.
  // `b_shared_indices` are indices of all other inputs from `b`.
  absl::flat_hash_set<size_t> b_only_pcs;
  absl::flat_hash_set<size_t> b_pcs;
  absl::flat_hash_map<size_t, CorpusRecord> b_pc_to_corpus;
  std::vector<size_t> b_shared_indices, b_unique_indices;
  for (size_t i = 0; i < b.size(); ++i) {
    const auto &record = b[i];
    bool has_b_only = false;
    for (const auto &feature : record.features) {
      if (!feature_domains::kPCs.Contains(feature)) continue;
      auto pc = ConvertPCFeatureToPcIndex(feature);
      b_pcs.insert(pc);
      b_pc_to_corpus.insert({pc, std::move(record)});
      if (a_pcs.contains(pc)) continue;
      b_only_pcs.insert(pc);
      has_b_only = true;
    }
    if (has_b_only)
      b_unique_indices.push_back(i);
    else
      b_shared_indices.push_back(i);
  }

  absl::flat_hash_set<size_t> a_only_pcs;
  for (const auto &record : a) {
    for (const auto &feature : record.features) {
      if (!feature_domains::kPCs.Contains(feature)) continue;
      auto pc = ConvertPCFeatureToPcIndex(feature);
      if (b_pcs.contains(pc)) continue;
      a_only_pcs.insert(pc);
    }
  }
  LOG(INFO) << VV(a.size()) << VV(b.size()) << VV(a_pcs.size())
            << VV(a_only_pcs.size()) << VV(b_only_pcs.size())
            << VV(b_shared_indices.size()) << VV(b_unique_indices.size());

  // Sort PCs to put them in the canonical order, as in pc_table.
  AnalyzeCorporaResults ret;
  ret.a_pcs = std::vector<size_t>{a_pcs.begin(), a_pcs.end()};
  ret.b_pcs = std::vector<size_t>{b_pcs.begin(), b_pcs.end()};
  ret.a_only_pcs = std::vector<size_t>{a_only_pcs.begin(), a_only_pcs.end()};
  ret.b_only_pcs = std::vector<size_t>{b_only_pcs.begin(), b_only_pcs.end()};
  ret.a_pc_to_corpus_record = std::move(a_pc_to_corpus);
  ret.b_pc_to_corpus_record = std::move(b_pc_to_corpus);
  std::sort(ret.a_pcs.begin(), ret.a_pcs.end());
  std::sort(ret.b_pcs.begin(), ret.b_pcs.end());
  std::sort(ret.a_only_pcs.begin(), ret.a_only_pcs.end());
  std::sort(ret.b_only_pcs.begin(), ret.b_only_pcs.end());

  return ret;
}

}  // namespace

CoverageResults GetCoverage(const std::vector<CorpusRecord> &corpus_records,
                            BinaryInfo binary_info) {
  absl::flat_hash_set<size_t> pcs;
  for (const auto &record : corpus_records) {
    for (const auto &feature : record.features) {
      if (!feature_domains::kPCs.Contains(feature)) continue;
      auto pc = ConvertPCFeatureToPcIndex(feature);
      pcs.insert(pc);
    }
  }
  CoverageResults ret = {
      /*pcs=*/{pcs.begin(), pcs.end()},
      /*binary_info=*/std::move(binary_info),
  };
  // Sort PCs to put them in the canonical order, as in pc_table.
  std::sort(ret.pcs.begin(), ret.pcs.end());
  return ret;
}

CoverageResults GetCoverage(std::string_view binary_name,
                            std::string_view binary_hash,
                            std::string_view workdir) {
  const std::vector<CorpusRecord> corpus_records =
      ReadCorpora(binary_name, binary_hash, workdir);
  BinaryInfo binary_info = ReadBinaryInfo(binary_name, binary_hash, workdir);
  return GetCoverage(corpus_records, std::move(binary_info));
}

void DumpCoverageReport(const CoverageResults &coverage_results,
                        std::string_view coverage_report_path) {
  LOG(INFO) << "Dump coverage to file: " << coverage_report_path;

  const fuzztest::internal::PCTable &pc_table =
      coverage_results.binary_info.pc_table;
  const fuzztest::internal::SymbolTable &symbols =
      coverage_results.binary_info.symbols;

  fuzztest::internal::SymbolTable coverage_symbol_table;
  for (const PCIndex pc : coverage_results.pcs) {
    CHECK_LE(pc, symbols.size());
    if (!pc_table[pc].has_flag(fuzztest::internal::PCInfo::kFuncEntry))
      continue;
    const SymbolTable::Entry entry = symbols.entry(pc);
    coverage_symbol_table.AddEntry(entry.func, entry.file_line_col());
  }

  std::ostringstream symbol_table_stream;
  coverage_symbol_table.WriteToLLVMSymbolizer(symbol_table_stream);

  CHECK_OK(
      RemoteFileSetContents(coverage_report_path, symbol_table_stream.str()));
}

AnalyzeCorporaResults AnalyzeCorpora(std::string_view binary_name,
                                     std::string_view binary_hash,
                                     std::string_view workdir_a,
                                     std::string_view workdir_b) {
  BinaryInfo binary_info_a =
      ReadBinaryInfo(binary_name, binary_hash, workdir_a);
  BinaryInfo binary_info_b =
      ReadBinaryInfo(binary_name, binary_hash, workdir_b);

  CHECK_EQ(binary_info_a.pc_table.size(), binary_info_b.pc_table.size());
  CHECK_EQ(binary_info_a.symbols.size(), binary_info_b.symbols.size());

  const std::vector<CorpusRecord> a =
      ReadCorpora(binary_name, binary_hash, workdir_a);
  const std::vector<CorpusRecord> b =
      ReadCorpora(binary_name, binary_hash, workdir_b);

  AnalyzeCorporaResults ret = AnalyzeCorpora(binary_info_a, a, b);
  ret.binary_info = std::move(binary_info_a);
  return ret;
}

void AnalyzeCorporaToLog(std::string_view binary_name,
                         std::string_view binary_hash,
                         std::string_view workdir_a,
                         std::string_view workdir_b) {
  AnalyzeCorporaResults results =
      AnalyzeCorpora(binary_name, binary_hash, workdir_a, workdir_b);

  const auto &pc_table = results.binary_info.pc_table;
  const auto &symbols = results.binary_info.symbols;
  CoverageLogger coverage_logger(pc_table, symbols);

  // TODO(kcc): use frontier_a to show the most interesting b-only PCs.
  // TODO(kcc): these cause a CHECK-fail
  // CoverageFrontier frontier_a(results.binary_info);
  // frontier_a.Compute(a);

  // First, print the newly covered functions (including partially covered).
  LOG(INFO) << "B-only new functions:";
  absl::flat_hash_set<std::string_view> b_only_new_functions;
  for (const auto pc : results.b_only_pcs) {
    if (!pc_table[pc].has_flag(PCInfo::kFuncEntry)) continue;
    auto str = coverage_logger.ObserveAndDescribeIfNew(pc);
    if (!str.empty()) LOG(INFO).NoPrefix() << str;
    b_only_new_functions.insert(symbols.func(pc));
  }

  // Now, print newly covered edges in functions that were covered in `a`.
  LOG(INFO) << "B-only new edges:";
  for (const auto pc : results.b_only_pcs) {
    if (b_only_new_functions.contains(symbols.func(pc))) continue;
    auto str = coverage_logger.ObserveAndDescribeIfNew(pc);
    if (!str.empty()) LOG(INFO).NoPrefix() << str;
  }
}

}  // namespace fuzztest::internal
