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

#include "./centipede/stats.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT: C++17
#include <initializer_list>
#include <iomanip>
#include <ios>
#include <iosfwd>
#include <limits>
#include <numeric>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "./centipede/environment.h"
#include "./centipede/workdir.h"
#include "./common/logging.h"
#include "./common/remote_file.h"

namespace fuzztest::internal {

namespace fs = std::filesystem;

using TraitBits = Stats::TraitBits;

// -----------------------------------------------------------------------------
//                               StatsReporter

StatsReporter::StatsReporter(const std::vector<std::atomic<Stats>> &stats_vec,
                             const std::vector<Environment> &env_vec)
    : stats_vec_{stats_vec}, env_vec_{env_vec} {
  CHECK_EQ(stats_vec.size(), env_vec.size());
  for (size_t i = 0; i < env_vec.size(); ++i) {
    const auto &env = env_vec[i];
    group_to_indices_[env.experiment_name].push_back(i);
    // NOTE: This will overwrite repeatedly for all indices of each group,
    // but the value will be the same by construction in environment.cc.
    group_to_flags_[env.experiment_name] = env.experiment_flags;
  }
}

void StatsReporter::ReportCurrStats() {
  // Collect snapshots of the current elements of `stats_vec_`: the elements
  // are `std::atomic`s; snapshotting them is required, and also provides
  // temporal consistency between the fields of each `Stats` object, even
  // as it is being modified by a different thread.
  std::vector<Stats> stats_snapshots;
  stats_snapshots.reserve(stats_vec_.size());
  for (const auto &stats : stats_vec_) {
    stats_snapshots.push_back(stats.load());
  }

  PreAnnounceFields(Stats::kFieldInfos);
  for (const Stats::FieldInfo &field_info : Stats::kFieldInfos) {
    if (!ShouldReportThisField(field_info)) continue;
    SetCurrField(field_info);
    for (const auto &[group_name, group_indices] : group_to_indices_) {
      SetCurrGroup(env_vec_[group_indices.at(0)]);
      // Get the required stat fields into a vector `stat_values`.
      std::vector<uint64_t> stat_values;
      stat_values.reserve(group_indices.size());
      for (const auto idx : group_indices) {
        stat_values.push_back(stats_snapshots.at(idx).*(field_info.field));
      }
      ReportCurrFieldSample(std::move(stat_values));
    }
  }
  ReportFlags(group_to_flags_);
  DoneFieldSamplesBatch();
}

// -----------------------------------------------------------------------------
//                               StatsLogger

bool StatsLogger::ShouldReportThisField(const Stats::FieldInfo &field) {
  // Skip timestamps and rusage stats: the former because timestamps are
  // not very useful in these logs (only in CSVs), the latter because rusage is
  // (at least currently) measured for the whole process, not per shard or
  // experiment, so reporting nearly identical numbers would be useless and
  // confusing.
  return (field.traits & TraitBits::kFuzzStat) != 0;
}

void StatsLogger::PreAnnounceFields(
    std::initializer_list<Stats::FieldInfo> fields) {
  // Nothing to do: field names are logged together with every sample's values.
}

void StatsLogger::SetCurrGroup(const Environment &master_env) {
  curr_experiment_name_ = master_env.experiment_name;
}

void StatsLogger::SetCurrField(const Stats::FieldInfo &field_info) {
  curr_field_info_ = field_info;
  os_ << curr_field_info_.description << ":\n";
}

void StatsLogger::ReportCurrFieldSample(std::vector<uint64_t> &&values) {
  if (!curr_experiment_name_.empty())
    os_ << "  " << curr_experiment_name_ << ": ";

  // Print the requested aggregate stats as well as the full sorted contents of
  // `values`.
  std::sort(values.begin(), values.end());
  const uint64_t min = values.front();
  const uint64_t max = values.back();
  const uint64_t sum = std::accumulate(values.begin(), values.end(), 0.);
  const double avg = !values.empty() ? (1.0 * sum / values.size()) : 0;

  os_ << std::fixed << std::setprecision(1);
  if (curr_field_info_.traits & TraitBits::kMin) os_ << "min:\t" << min << "\t";
  if (curr_field_info_.traits & TraitBits::kMax) os_ << "max:\t" << max << "\t";
  if (curr_field_info_.traits & TraitBits::kAvg) os_ << "avg:\t" << avg << "\t";
  if (curr_field_info_.traits & TraitBits::kSum) os_ << "sum:\t" << sum << "\t";

  os_ << "--";
  for (const auto value : values) {
    os_ << "\t" << value;
  }
  os_ << "\n";
}

void StatsLogger::ReportFlags(const GroupToFlags &group_to_flags) {
  std::stringstream fos;
  for (const auto &[group_name, group_flags] : group_to_flags) {
    if (!group_name.empty() || !group_flags.empty()) {
      fos << "  " << group_name << ": " << group_flags << "\n";
    }
  }
  if (fos.tellp() != std::streampos{0}) os_ << "Flags:\n" << fos.rdbuf();
}

void StatsLogger::DoneFieldSamplesBatch() {
  LOG(INFO) << "Current stats:\n" << absl::StripAsciiWhitespace(os_.str());
  // Reset the stream for the next round of logging.
  os_.str("");
}

// -----------------------------------------------------------------------------
//                           StatsCsvFileAppender

StatsCsvFileAppender::~StatsCsvFileAppender() {
  if (files_ == nullptr) return;
  for (const auto &[group_name, file] : *files_) {
    CHECK_OK(RemoteFileClose(file.file));
  }
}

void StatsCsvFileAppender::PreAnnounceFields(
    std::initializer_list<Stats::FieldInfo> fields) {
  if (!csv_header_.empty()) return;

  for (const auto &field : fields) {
    if (field.traits & TraitBits::kMin)
      absl::StrAppend(&csv_header_, field.name, "_Min,");
    if (field.traits & TraitBits::kMax)
      absl::StrAppend(&csv_header_, field.name, "_Max,");
    if (field.traits & TraitBits::kAvg)
      absl::StrAppend(&csv_header_, field.name, "_Avg,");
    if (field.traits & TraitBits::kSum)
      absl::StrAppend(&csv_header_, field.name, "_Sum,");
  }
  absl::StrAppend(&csv_header_, "\n");
}

void StatsCsvFileAppender::SetCurrGroup(const Environment &master_env) {
  CHECK(files_ != nullptr);
  BufferedRemoteFile &file = (*files_)[master_env.experiment_name];
  if (file.file == nullptr) {
    const std::string filename =
        WorkDir{master_env}.FuzzingStatsPath(master_env.experiment_name);
    // If a non-empty file already exists and has the same CVS header, then
    // keep appending new CSV lines to the file. If the file exists, but has a
    // different CSV header (ostensibly because it was created by a different
    // version of Centipede), then make a backup copy of the file and start a
    // a new one from scratch.
    bool append = false;
    if (RemotePathExists(filename)) {
      std::string contents;
      CHECK_OK(RemoteFileGetContents(filename, contents));
      // NOTE: `csv_header_` ends with '\n', so the match is exact.
      if (absl::StartsWith(contents, csv_header_)) {
        append = true;
      } else {
        append = false;
        CHECK_OK(RemoteFileSetContents(GetBackupFilename(filename), contents));
      }
    }
    file.file = *RemoteFileOpen(filename, append ? "a" : "w");
    CHECK(file.file != nullptr) << VV(filename);
    if (!append) {
      CHECK_OK(RemoteFileAppend(file.file, csv_header_));
      CHECK_OK(RemoteFileFlush(file.file));
    }
  }
  // This is OK even though hash maps provide no pointer stability because the
  // field is always updated immediately after the map is modified.
  curr_file_ = &file;
}

void StatsCsvFileAppender::SetCurrField(const Stats::FieldInfo &field_info) {
  curr_field_info_ = field_info;
}

void StatsCsvFileAppender::ReportCurrFieldSample(
    std::vector<uint64_t> &&values) {
  uint64_t min = std::numeric_limits<uint64_t>::max();
  uint64_t max = std::numeric_limits<uint64_t>::min();
  uint64_t sum = 0;
  for (const auto value : values) {
    min = std::min(min, value);
    max = std::max(max, value);
    sum += value;
  }
  double avg = !values.empty() ? (1.0 * sum / values.size()) : 0;

  CHECK(curr_file_ != nullptr);
  std::string &values_str = curr_file_->buffer;
  if (curr_field_info_.traits & TraitBits::kMin)
    absl::StrAppendFormat(&values_str, "%" PRIu64 ",", min);
  if (curr_field_info_.traits & TraitBits::kMax)
    absl::StrAppendFormat(&values_str, "%" PRIu64 ",", max);
  if (curr_field_info_.traits & TraitBits::kAvg)
    absl::StrAppendFormat(&values_str, "%.1lf,", avg);
  if (curr_field_info_.traits & TraitBits::kSum)
    absl::StrAppendFormat(&values_str, "%" PRIu64 ",", sum);
}

void StatsCsvFileAppender::ReportFlags(const GroupToFlags &group_to_flags) {
  // Do nothing: can't write to CSV, as it has no concept of comments.
  // TODO(ussuri): Consider writing to a sidecar file.
}

void StatsCsvFileAppender::DoneFieldSamplesBatch() {
  CHECK(files_ != nullptr);
  for (auto &[group_name, file] : *files_) {
    CHECK_OK(RemoteFileAppend(file.file, absl::StrCat(file.buffer, "\n")));
    CHECK_OK(RemoteFileFlush(file.file));
    file.buffer.clear();
  }
}

std::string StatsCsvFileAppender::GetBackupFilename(
    const std::string &filename) const {
  fs::path path{filename};
  const auto timestamp = absl::ToUnixSeconds(absl::Now());
  const auto new_extension =
      absl::StrCat(path.extension().string(), ".", timestamp);
  path.replace_extension(new_extension);
  return path.string();
}

// -----------------------------------------------------------------------------

void PrintRewardValues(absl::Span<const std::atomic<Stats>> stats_vec,
                       std::ostream &os) {
  size_t n = stats_vec.size();
  CHECK_GT(n, 0);
  std::vector<size_t> num_covered_pcs(n);
  for (size_t i = 0; i < n; ++i) {
    num_covered_pcs[i] = stats_vec[i].load().num_covered_pcs;
  }
  std::sort(num_covered_pcs.begin(), num_covered_pcs.end());
  os << "REWARD_MAX " << num_covered_pcs.back() << "\n";
  os << "REWARD_SECOND_MAX " << num_covered_pcs[n == 1 ? 1 : n - 2] << "\n";
  os << "REWARD_MIN " << num_covered_pcs.front() << "\n";
  os << "REWARD_MEDIAN " << num_covered_pcs[n / 2] << "\n";
  os << "REWARD_AVERAGE "
     << (std::accumulate(num_covered_pcs.begin(), num_covered_pcs.end(), 0.) /
         n)
     << "\n";
}

}  // namespace fuzztest::internal
