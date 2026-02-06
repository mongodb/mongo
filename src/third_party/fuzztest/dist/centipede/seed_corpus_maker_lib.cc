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

// The Centipede seed corpus maker. It selects a sample of fuzzing inputs from N
// Centipede workdirs and writes them out to a new set of Centipede corpus file
// shards.

#include "./centipede/seed_corpus_maker_lib.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>  // NOLINT
#include <functional>
#include <iterator>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/random/random.h"
#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_replace.h"
#include "absl/time/time.h"
#include "./centipede/corpus_io.h"
#include "./centipede/feature.h"
#include "./centipede/rusage_profiler.h"
#include "./centipede/thread_pool.h"
#include "./centipede/util.h"
#include "./centipede/workdir.h"
#include "./common/blob_file.h"
#include "./common/defs.h"
#include "./common/logging.h"
#include "./common/remote_file.h"
#include "./common/status_macros.h"

// TODO(ussuri): Implement a smarter on-the-fly sampling to avoid having to
//  load all of a source's elements into RAM only to pick some of them. That
//  would be trivial if the number of elements in a corpus file could be
//  determined without reading all of it.

namespace fuzztest::internal {

namespace fs = std::filesystem;

namespace {

std::string ShardPathsForLogging(  //
    const std::string& corpus_fname, const std::string& features_fname) {
  if (ABSL_VLOG_IS_ON(3)) {
    return absl::StrCat(  //
        ":\nCorpus:  ", corpus_fname, "\nFeatures:", features_fname);
  }
  return "";
}

}  // namespace

// TODO(ussuri): Refactor into smaller functions.
absl::Status SampleSeedCorpusElementsFromSource(  //
    const SeedCorpusSource& source,               //
    std::string_view coverage_binary_name,        //
    std::string_view coverage_binary_hash,        //
    InputAndFeaturesVec& elements) {
  if (coverage_binary_name.empty() != coverage_binary_hash.empty()) {
    return absl::InvalidArgumentError(
        absl::StrCat("coverage_binary and coverage_hash should either both be "
                     "provided or empty, got ",
                     coverage_binary_name, ", and ", coverage_binary_hash));
  }

  RPROF_THIS_FUNCTION_WITH_TIMELAPSE(                                      //
      /*enable=*/ABSL_VLOG_IS_ON(1),                                       //
      /*timelapse_interval=*/absl::Seconds(ABSL_VLOG_IS_ON(2) ? 10 : 60),  //
      /*also_log_timelapses=*/ABSL_VLOG_IS_ON(10));

  LOG(INFO) << "Reading/sampling seed corpus elements from source glob: "
            << source.dir_glob;

  // Find `source.dir_glob()`-matching dirs and pick at most
  // `source.num_recent_dirs()` most recent ones.

  std::vector<std::string> src_dirs;
  if (const auto match_status = RemoteGlobMatch(source.dir_glob, src_dirs);
      !match_status.ok() && !absl::IsNotFound(match_status)) {
    return match_status;
  }
  LOG(INFO) << "Found " << src_dirs.size() << " corpus dir(s) matching "
            << source.dir_glob;
  // Sort in the ascending lexicographical order. We expect that dir names
  // contain timestamps and therefore will be sorted from oldest to newest.
  std::sort(src_dirs.begin(), src_dirs.end(), std::less<std::string>());
  if (source.num_recent_dirs < src_dirs.size()) {
    src_dirs.erase(src_dirs.begin(), src_dirs.end() - source.num_recent_dirs);
    LOG(INFO) << "Selected " << src_dirs.size() << " corpus dir(s)";
  }

  // Find all the corpus shard and individual input files in the found dirs.

  std::vector<std::string> corpus_shard_fnames;
  std::vector<std::string> individual_input_fnames;
  for (const auto& dir : src_dirs) {
    absl::flat_hash_set<std::string> current_corpus_shard_fnames;
    if (!source.shard_rel_glob.empty()) {
      std::vector<std::string> matched_fnames;
      const std::string glob = fs::path{dir} / source.shard_rel_glob;
      const auto match_status = RemoteGlobMatch(glob, matched_fnames);
      if (!match_status.ok() && !absl::IsNotFound(match_status)) {
        LOG(ERROR) << "Got error when glob-matching in " << dir << ": "
                   << match_status;
      } else {
        current_corpus_shard_fnames.insert(matched_fnames.begin(),
                                           matched_fnames.end());
        corpus_shard_fnames.insert(corpus_shard_fnames.end(),
                                   matched_fnames.begin(),
                                   matched_fnames.end());
        LOG(INFO) << "Found " << matched_fnames.size() << " shard(s) matching "
                  << glob;
      }
    }
    if (!source.individual_input_rel_glob.empty()) {
      std::vector<std::string> matched_fnames;
      const std::string glob = fs::path{dir} / source.individual_input_rel_glob;
      const auto match_status = RemoteGlobMatch(glob, matched_fnames);
      if (!match_status.ok() && !absl::IsNotFound(match_status)) {
        LOG(ERROR) << "Got error when glob-matching in " << dir << ": "
                   << match_status;
      } else {
        size_t num_added_individual_inputs = 0;
        for (auto& fname : matched_fnames) {
          if (current_corpus_shard_fnames.contains(fname)) continue;
          if (RemotePathIsDirectory(fname)) continue;
          ++num_added_individual_inputs;
          individual_input_fnames.push_back(std::move(fname));
        }
        LOG(INFO) << "Found " << num_added_individual_inputs
                  << " individual input(s) with glob " << glob;
      }
    }
  }
  LOG(INFO) << "Found " << corpus_shard_fnames.size() << " shard(s) and "
            << individual_input_fnames.size()
            << " individual input(s) total in source " << source.dir_glob;

  if (corpus_shard_fnames.empty() && individual_input_fnames.empty()) {
    LOG(WARNING) << "Skipping empty source " << source.dir_glob;
    return absl::OkStatus();
  }

  // Read all the elements from the found corpus shard files using parallel I/O
  // threads.

  const auto num_shards = corpus_shard_fnames.size();
  std::vector<InputAndFeaturesVec> src_elts_per_shard(num_shards);
  std::vector<size_t> src_elts_with_features_per_shard(num_shards, 0);
  InputAndFeaturesVec src_elts;

  {
    constexpr int kMaxReadThreads = 32;
    ThreadPool threads{std::min<int>(
        kMaxReadThreads, std::max(num_shards, individual_input_fnames.size()))};

    for (int shard = 0; shard < num_shards; ++shard) {
      const auto& corpus_fname = corpus_shard_fnames[shard];
      auto& shard_elts = src_elts_per_shard[shard];
      auto& shard_elts_with_features = src_elts_with_features_per_shard[shard];

      const auto read_shard = [shard, corpus_fname, coverage_binary_name,
                               coverage_binary_hash, &shard_elts,
                               &shard_elts_with_features]() {
        // NOTE: The deduced matching `features_fname` may not exist if the
        // source corpus was generated for a coverage binary that is different
        // from the one we need, but `ReadShard()` can tolerate that, passing
        // empty `FeatureVec`s to the callback if that's the case.
        const auto work_dir = WorkDir::FromCorpusShardPath(  //
            corpus_fname, coverage_binary_name, coverage_binary_hash);
        const std::string features_fname =
            work_dir.CorpusFilePaths().IsShard(corpus_fname)
                ? work_dir.FeaturesFilePaths().MyShard()
            : work_dir.DistilledCorpusFilePaths().IsShard(corpus_fname)
                ? work_dir.DistilledFeaturesFilePaths().MyShard()
                : "";

        VLOG(2) << "Reading elements from source shard " << shard
                << ShardPathsForLogging(corpus_fname, features_fname);

        ReadShard(corpus_fname, features_fname,
                  [shard, &shard_elts, &shard_elts_with_features](  //
                      ByteArray input, FeatureVec features) {
                    // `ReadShard()` indicates "features not computed/found" as
                    // `{}` and "features computed/found, but empty" as
                    // `{feature_domains::kNoFeature}`. We're interested in how
                    // many precomputed features we find, even if empty.
                    if (!features.empty()) {
                      ++shard_elts_with_features;
                    }
                    shard_elts.emplace_back(input, std::move(features));
                    VLOG_EVERY_N(10, 100000)
                        << "Read " << shard_elts.size()
                        << " elements from shard " << shard << " so far";
                  });

        LOG(INFO) << "Read " << shard_elts.size() << " elements ("
                  << shard_elts_with_features
                  << " with computed features) from source shard " << shard
                  << ShardPathsForLogging(corpus_fname, features_fname);
      };

      threads.Schedule(read_shard);
    }

    RPROF_SNAPSHOT_AND_LOG("Done reading shards");

    src_elts.resize(individual_input_fnames.size());
    for (size_t index = 0; index < individual_input_fnames.size(); ++index) {
      threads.Schedule([index, &individual_input_fnames, &src_elts] {
        ByteArray input;
        const auto& path = individual_input_fnames[index];
        const auto read_status = RemoteFileGetContents(path, input);
        if (!read_status.ok()) {
          LOG(WARNING) << "Skipping individual input path " << path
                       << " due to read error: " << read_status;
          return;
        }
        src_elts[index] = {std::move(input), {}};
      });
    }
  }

  RPROF_SNAPSHOT_AND_LOG("Done reading");

  size_t src_num_features = 0;

  for (int s = 0; s < num_shards; ++s) {
    auto& shard_elts = src_elts_per_shard[s];
    for (auto& elt : shard_elts) {
      src_elts.emplace_back(std::move(elt));
    }
    shard_elts.clear();
    shard_elts.shrink_to_fit();
    src_num_features += src_elts_with_features_per_shard[s];
  }

  src_elts_per_shard.clear();
  src_elts_per_shard.shrink_to_fit();
  src_elts_with_features_per_shard.clear();
  src_elts_with_features_per_shard.shrink_to_fit();

  RPROF_SNAPSHOT_AND_LOG("Done merging");

  // Remove empty inputs possibly due to read errors.
  auto remove_it =
      std::remove_if(src_elts.begin(), src_elts.end(),
                     [](const auto& elt) { return std::get<0>(elt).empty(); });
  if (remove_it != src_elts.end()) {
    LOG(WARNING) << "Removed " << std::distance(remove_it, src_elts.end())
                 << " empty inputs";
    src_elts.erase(remove_it, src_elts.end());
  }

  LOG(INFO) << "Read total of " << src_elts.size() << " elements ("
            << src_num_features << " with features) from source "
            << source.dir_glob;

  // Extract a sample of the elements of the size specified in
  // `source.sample_size`.

  size_t sample_size = 0;
  if (std::holds_alternative<float>(source.sampled_fraction_or_count)) {
    const auto fraction = std::get<float>(source.sampled_fraction_or_count);
    if (fraction <= 0.0 || fraction > 1 || !std::isfinite(fraction)) {
      return absl::InvalidArgumentError(
          absl::StrCat("sampled_fraction must be in (0, 1], got ", fraction));
    }
    sample_size = std::llrint(src_elts.size() * fraction);
  } else if (std::holds_alternative<uint32_t>(
                 source.sampled_fraction_or_count)) {
    const auto count = std::get<uint32_t>(source.sampled_fraction_or_count);
    sample_size = std::min<size_t>(src_elts.size(), count);
  } else {
    sample_size = src_elts.size();
  }

  if (sample_size < src_elts.size()) {
    LOG(INFO) << "Sampling " << sample_size << " elements out of "
              << src_elts.size();
  } else {
    LOG(INFO) << "Using all " << src_elts.size() << " elements";
  }

  // Extract a sample by shuffling the elements' indices and resizing to the
  // requested sample size. We do this, rather than std::sampling the elements
  // themselves and associated inserting into `elements`, to avoid a spike in
  // peak RAM usage.
  std::vector<size_t> src_sample_idxs(src_elts.size());
  std::iota(src_sample_idxs.begin(), src_sample_idxs.end(), 0);
  std::shuffle(src_sample_idxs.begin(), src_sample_idxs.end(), absl::BitGen{});
  src_sample_idxs.resize(sample_size);

  RPROF_SNAPSHOT_AND_LOG("Done sampling");

  // Now move each sampled element from `src_elts` to `elements`.
  elements.reserve(elements.size() + sample_size);
  for (size_t idx : src_sample_idxs) {
    elements.emplace_back(std::move(src_elts[idx]));
  }

  RPROF_SNAPSHOT_AND_LOG("Done appending");
  return absl::OkStatus();
}

// TODO(ussuri): Refactor into smaller functions.
absl::Status WriteSeedCorpusElementsToDestination(  //
    const InputAndFeaturesVec& elements,            //
    std::string_view coverage_binary_name,          //
    std::string_view coverage_binary_hash,          //
    const SeedCorpusDestination& destination) {
  if (coverage_binary_name.empty() != coverage_binary_hash.empty()) {
    return absl::InvalidArgumentError(
        absl::StrCat("coverage_binary and coverage_hash should either both be "
                     "provided or empty, got ",
                     coverage_binary_name, ", and ", coverage_binary_hash));
  }

  if (elements.empty()) {
    return absl::InvalidArgumentError(
        "Collected seed corpus turned out to be empty: verify config / "
        "sources");
  }
  if (destination.dir_path.empty()) {
    return absl::InvalidArgumentError(
        "Unable to write seed corpus to empty destination path");
  }

  RPROF_THIS_FUNCTION_WITH_TIMELAPSE(                                      //
      /*enable=*/ABSL_VLOG_IS_ON(1),                                       //
      /*timelapse_interval=*/absl::Seconds(ABSL_VLOG_IS_ON(2) ? 10 : 60),  //
      /*also_log_timelapses=*/ABSL_VLOG_IS_ON(10));

  LOG(INFO) << "Writing " << elements.size()
            << " seed corpus elements to destination: " << destination.dir_path;

  if (destination.num_shards <= 0) {
    return absl::InvalidArgumentError(
        "Requested number of destination shards must be > 0");
  }
  if (!absl::StrContains(destination.shard_rel_glob, "*")) {
    return absl::InvalidArgumentError(
        absl::StrCat("Destination shard pattern must contain '*', got ",
                     destination.shard_rel_glob));
  }

  // Compute shard sizes. If the elements can't be evenly divided between the
  // requested number of shards, distribute the N excess elements between the
  // first N shards.
  const size_t num_shards =
      std::min<size_t>(destination.num_shards, elements.size());
  CHECK_GT(num_shards, 0);
  const size_t shard_size = elements.size() / num_shards;
  std::vector<size_t> shard_sizes(num_shards, shard_size);
  const size_t excess_elts = elements.size() % num_shards;
  for (size_t i = 0; i < excess_elts; ++i) {
    ++shard_sizes[i];
  }
  std::atomic<size_t> dst_elts_with_features = 0;

  // Write the elements to the shard files using parallel I/O threads.
  std::vector<absl::Status> write_shard_status(shard_sizes.size());
  {
    constexpr int kMaxWriteThreads = 1000;
    ThreadPool threads{std::min<int>(kMaxWriteThreads, num_shards)};

    auto shard_elt_it = elements.cbegin();

    for (size_t shard = 0; shard < shard_sizes.size(); ++shard) {
      // Compute this shard's range of the input elements to write.
      const auto shard_size = shard_sizes[shard];
      const auto elt_range_begin = shard_elt_it;
      std::advance(shard_elt_it, shard_size);
      const auto elt_range_end = shard_elt_it;
      CHECK(shard_elt_it <= elements.cend()) << VV(shard);

      const auto write_shard = [shard, elt_range_begin, elt_range_end,
                                coverage_binary_name, coverage_binary_hash,
                                &destination, &dst_elts_with_features]() {
        // Generate the output shard's filename.
        // TODO(ussuri): Use more of `WorkDir` APIs here (possibly extend
        // them, and possibly retire
        // `SeedCorpusDestination::shard_index_digits`).
        const std::string shard_idx =
            absl::StrFormat("%0*d", destination.shard_index_digits, shard);
        const std::string corpus_rel_fname =
            absl::StrReplaceAll(destination.shard_rel_glob, {{"*", shard_idx}});
        const std::string corpus_fname =
            fs::path{destination.dir_path} / corpus_rel_fname;

        const auto work_dir = WorkDir::FromCorpusShardPath(  //
            corpus_fname, coverage_binary_name, coverage_binary_hash);

        if (corpus_fname != work_dir.CorpusFilePaths().MyShard() &&
            corpus_fname != work_dir.DistilledCorpusFilePaths().MyShard()) {
          return absl::InvalidArgumentError(absl::StrCat(
              "Bad config: generated destination corpus filename '",
              corpus_fname, "' doesn't match one of two expected forms '",
              work_dir.CorpusFilePaths().MyShard(), "' or '",
              work_dir.DistilledCorpusFilePaths().MyShard(),
              "'; make sure binary name in config matches explicitly passed '",
              coverage_binary_name, "'"));
        }

        const std::string features_fname =
            work_dir.CorpusFilePaths().IsShard(corpus_fname)
                ? work_dir.FeaturesFilePaths().MyShard()
                : work_dir.DistilledFeaturesFilePaths().MyShard();
        CHECK(!features_fname.empty());

        VLOG(2) << "Writing " << std::distance(elt_range_begin, elt_range_end)
                << " elements to destination shard " << shard
                << ShardPathsForLogging(corpus_fname, features_fname);

        // Features files are always saved in a subdir of the workdir
        // (== `destination.dir_path` here), which might not exist yet, so we
        // create it. Corpus files are saved in the workdir directly, but we
        // also create it in case `destination.shard_rel_glob` contains some
        // dirs (not really intended for that, but the end-user may do that).
        for (const auto& fname : {corpus_fname, features_fname}) {
          if (!fname.empty()) {
            const auto dir = fs::path{fname}.parent_path().string();
            if (!RemotePathExists(dir)) {
              RETURN_IF_NOT_OK(RemoteMkdir(dir));
            }
          }
        }

        // Create writers for the corpus and features shard files.

        // TODO(ussuri): Wrap corpus/features writing in a similar API to
        // `ReadShard()`.

        const std::unique_ptr<BlobFileWriter> corpus_writer =
            DefaultBlobFileWriterFactory();
        CHECK(corpus_writer != nullptr);
        RETURN_IF_NOT_OK(corpus_writer->Open(corpus_fname, "w"));

        const std::unique_ptr<BlobFileWriter> features_writer =
            DefaultBlobFileWriterFactory();
        CHECK(features_writer != nullptr);
        RETURN_IF_NOT_OK(features_writer->Open(features_fname, "w"));

        // Write the shard's elements to the corpus and features shard files.

        size_t shard_elts_with_features = 0;
        for (auto elt_it = elt_range_begin; elt_it != elt_range_end; ++elt_it) {
          const ByteArray& input = elt_it->first;
          RETURN_IF_NOT_OK(corpus_writer->Write(input));
          const FeatureVec& features = elt_it->second;
          if (!features.empty()) {
            ++shard_elts_with_features;
            const ByteArray packed_features =
                PackFeaturesAndHash(input, features);
            RETURN_IF_NOT_OK(features_writer->Write(packed_features));
          }
        }

        LOG(INFO) << "Wrote " << std::distance(elt_range_begin, elt_range_end)
                  << " elements (" << shard_elts_with_features
                  << " with features) to destination shard " << shard
                  << ShardPathsForLogging(corpus_fname, features_fname);

        dst_elts_with_features += shard_elts_with_features;

        RETURN_IF_NOT_OK(corpus_writer->Close());
        RETURN_IF_NOT_OK(features_writer->Close());
        return absl::OkStatus();
      };
      threads.Schedule([&write_shard_status, write_shard, shard]() {
        write_shard_status[shard] = write_shard();
      });
    }
  }
  for (const absl::Status& write_status : write_shard_status) {
    RETURN_IF_NOT_OK(write_status);
  }

  LOG(INFO) << "Wrote total of " << elements.size() << " elements ("
            << dst_elts_with_features
            << " with precomputed features) to destination "
            << destination.dir_path;
  return absl::OkStatus();
}

absl::Status GenerateSeedCorpusFromConfig(  //
    const SeedCorpusConfig& config,         //
    std::string_view coverage_binary_name,  //
    std::string_view coverage_binary_hash) {
  InputAndFeaturesVec elements;

  // Read and sample elements from the sources.
  for (const auto& source : config.sources) {
    RETURN_IF_NOT_OK(SampleSeedCorpusElementsFromSource(  //
        source, coverage_binary_name, coverage_binary_hash, elements));
  }
  LOG(INFO) << "Sampled " << elements.size() << " elements from "
            << config.sources.size() << " seed corpus source(s)";

  // Write the sampled elements to the destination.
  if (elements.empty()) {
    LOG(WARNING)
        << "No elements to write to seed corpus destination - doing nothing";
  } else {
    RETURN_IF_NOT_OK(WriteSeedCorpusElementsToDestination(  //
        elements, coverage_binary_name, coverage_binary_hash,
        config.destination));
    LOG(INFO) << "Wrote " << elements.size()
              << " elements to seed corpus destination";
  }
  return absl::OkStatus();
}

}  // namespace fuzztest::internal
