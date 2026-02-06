// Copyright 2023 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "./fuzztest/internal/configuration.h"

#include <cstddef>
#include <cstring>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"

namespace fuzztest::internal {

bool AbslParseFlag(absl::string_view text, TimeBudgetType* mode,
                   std::string* error) {
  if (text == "per-test") {
    *mode = TimeBudgetType::kPerTest;
    return true;
  }
  if (text == "total") {
    *mode = TimeBudgetType::kTotal;
    return true;
  }
  *error = "unknown value for enumeration";
  return false;
}

absl::StatusOr<TimeBudgetType> ParseTimeBudgetType(absl::string_view text) {
  TimeBudgetType mode;
  std::string error;
  if (!AbslParseFlag(text, &mode, &error)) {
    return absl::InvalidArgumentError(error);
  }
  return mode;
}

std::string AbslUnparseFlag(TimeBudgetType mode) {
  switch (mode) {
    case TimeBudgetType::kPerTest:
      return "per-test";
    case TimeBudgetType::kTotal:
      return "total";
    default:
      return absl::StrCat(mode);
  }
}

namespace {

template <typename T>
size_t SpaceFor(const T&) {
  return sizeof(T);
}

template <>
size_t SpaceFor(const absl::string_view& str) {
  return SpaceFor(str.size()) + str.size();
}

template <>
size_t SpaceFor(const std::string& str) {
  return SpaceFor(absl::string_view(str));
}

template <>
size_t SpaceFor(const std::optional<std::string>& obj) {
  return SpaceFor(obj.has_value()) + (obj.has_value() ? SpaceFor(*obj) : 0);
}

template <>
size_t SpaceFor(const std::vector<std::string>& vec) {
  size_t space_for_strings = 0;
  for (const std::string& str : vec) {
    space_for_strings += SpaceFor(str);
  }
  return SpaceFor(vec.size()) + space_for_strings;
}

template <int&... ExplicitArgumentBarrier, typename IntT,
          typename = std::enable_if_t<std::is_integral_v<IntT>>>
size_t WriteIntegral(std::string& out, size_t offset, IntT val) {
  ABSL_CHECK_GE(out.size(), offset + SpaceFor(val));
  std::memcpy(out.data() + offset, &val, SpaceFor(val));
  offset += SpaceFor(val);
  return offset;
}

size_t WriteString(std::string& out, size_t offset, absl::string_view str) {
  ABSL_CHECK_GE(out.size(), offset + SpaceFor(str));
  offset = WriteIntegral(out, offset, str.size());
  std::memcpy(out.data() + offset, str.data(), str.size());
  offset += str.size();
  return offset;
}

size_t WriteOptionalString(std::string& out, size_t offset,
                           const std::optional<std::string>& str) {
  ABSL_CHECK_GE(out.size(), offset + SpaceFor(str));
  offset = WriteIntegral(out, offset, str.has_value());
  if (str.has_value()) {
    offset = WriteString(out, offset, *str);
  }
  return offset;
}

size_t WriteVectorOfStrings(std::string& out, size_t offset,
                            const std::vector<std::string>& vec) {
  ABSL_CHECK_GE(out.size(), offset + SpaceFor(vec));
  offset = WriteIntegral(out, offset, vec.size());
  for (const std::string& str : vec) {
    offset = WriteString(out, offset, str);
  }
  return offset;
}

#define ASSIGN_OR_RETURN(var, expr)              \
  auto var = expr;                               \
  if (!var.ok()) return std::move(var).status(); \
  static_assert(true, "")  // Swallow semicolon

template <typename IntT, int&... ExplicitArgumentBarrier,
          typename = std::enable_if_t<std::is_integral_v<IntT>>>
absl::StatusOr<IntT> Consume(absl::string_view& buffer) {
  IntT val = 0;
  if (buffer.size() < SpaceFor(val)) {
    return absl::InvalidArgumentError(
        "Couldn't consume a value from a buffer.");
  }
  std::memcpy(&val, buffer.data(), SpaceFor(val));
  buffer.remove_prefix(SpaceFor(val));
  return val;
}

absl::StatusOr<std::string> ConsumeString(absl::string_view& buffer) {
  ASSIGN_OR_RETURN(size, Consume<size_t>(buffer));
  if (buffer.size() < *size) {
    return absl::InvalidArgumentError(
        "Couldn't consume a value from a buffer.");
  }
  std::string str(buffer.data(), *size);
  buffer.remove_prefix(*size);
  return str;
}

absl::StatusOr<std::optional<std::string>> ConsumeOptionalString(
    absl::string_view& buffer) {
  ASSIGN_OR_RETURN(has_value, Consume<bool>(buffer));
  if (!*has_value) return std::nullopt;
  return ConsumeString(buffer);
}

absl::StatusOr<std::vector<std::string>> ConsumeVectorOfStrings(
    absl::string_view& buffer) {
  ASSIGN_OR_RETURN(size, Consume<size_t>(buffer));
  if (buffer.size() < *size) {
    return absl::InvalidArgumentError(
        "Couldn't consume a value from a buffer.");
  }
  std::vector<std::string> vec;
  vec.reserve(*size);
  for (size_t i = 0; i < *size; ++i) {
    ASSIGN_OR_RETURN(str, ConsumeString(buffer));
    vec.push_back(*std::move(str));
  }
  return vec;
}

absl::StatusOr<absl::Duration> ParseDuration(absl::string_view duration) {
  absl::Duration result;
  if (!absl::ParseDuration(duration, &result)) {
    return absl::InvalidArgumentError(
        absl::StrCat("Couldn't parse a duration: ", duration));
  }
  return result;
}

}  // namespace

std::string Configuration::Serialize() const {
  std::string time_limit_per_input_str =
      absl::FormatDuration(time_limit_per_input);
  std::string time_limit_str = absl::FormatDuration(time_limit);
  std::string time_budget_type_str = AbslUnparseFlag(time_budget_type);
  std::string out;
  out.resize(SpaceFor(corpus_database) + SpaceFor(stats_root) +
             SpaceFor(workdir_root) + SpaceFor(binary_identifier) +
             SpaceFor(fuzz_tests) + SpaceFor(fuzz_tests_in_current_shard) +
             SpaceFor(reproduce_findings_as_separate_tests) +
             SpaceFor(replay_coverage_inputs) + SpaceFor(only_replay) +
             SpaceFor(replay_in_single_process) + SpaceFor(execution_id) +
             SpaceFor(print_subprocess_log) + SpaceFor(stack_limit) +
             SpaceFor(rss_limit) + SpaceFor(time_limit_per_input_str) +
             SpaceFor(time_limit_str) + SpaceFor(time_budget_type_str) +
             SpaceFor(jobs) + SpaceFor(centipede_command) +
             SpaceFor(crashing_input_to_reproduce) +
             SpaceFor(reproduction_command_template));
  size_t offset = 0;
  offset = WriteString(out, offset, corpus_database);
  offset = WriteString(out, offset, stats_root);
  offset = WriteString(out, offset, workdir_root);
  offset = WriteString(out, offset, binary_identifier);
  offset = WriteVectorOfStrings(out, offset, fuzz_tests);
  offset = WriteVectorOfStrings(out, offset, fuzz_tests_in_current_shard);
  offset = WriteIntegral(out, offset, reproduce_findings_as_separate_tests);
  offset = WriteIntegral(out, offset, replay_coverage_inputs);
  offset = WriteIntegral(out, offset, only_replay);
  offset = WriteIntegral(out, offset, replay_in_single_process);
  offset = WriteOptionalString(out, offset, execution_id);
  offset = WriteIntegral(out, offset, print_subprocess_log);
  offset = WriteIntegral(out, offset, stack_limit);
  offset = WriteIntegral(out, offset, rss_limit);
  offset = WriteString(out, offset, time_limit_per_input_str);
  offset = WriteString(out, offset, time_limit_str);
  offset = WriteString(out, offset, time_budget_type_str);
  offset = WriteIntegral(out, offset, jobs);
  offset = WriteOptionalString(out, offset, centipede_command);
  offset = WriteOptionalString(out, offset, crashing_input_to_reproduce);
  offset = WriteOptionalString(out, offset, reproduction_command_template);
  ABSL_CHECK_EQ(offset, out.size());
  return out;
}

absl::StatusOr<Configuration> Configuration::Deserialize(
    absl::string_view serialized) {
  return [=]() mutable -> absl::StatusOr<Configuration> {
    ASSIGN_OR_RETURN(corpus_database, ConsumeString(serialized));
    ASSIGN_OR_RETURN(stats_root, ConsumeString(serialized));
    ASSIGN_OR_RETURN(workdir_root, ConsumeString(serialized));
    ASSIGN_OR_RETURN(binary_identifier, ConsumeString(serialized));
    ASSIGN_OR_RETURN(fuzz_tests, ConsumeVectorOfStrings(serialized));
    ASSIGN_OR_RETURN(fuzz_tests_in_current_shard,
                     ConsumeVectorOfStrings(serialized));
    ASSIGN_OR_RETURN(reproduce_findings_as_separate_tests,
                     Consume<bool>(serialized));
    ASSIGN_OR_RETURN(replay_coverage_inputs, Consume<bool>(serialized));
    ASSIGN_OR_RETURN(only_replay, Consume<bool>(serialized));
    ASSIGN_OR_RETURN(replay_in_single_process, Consume<bool>(serialized));
    ASSIGN_OR_RETURN(execution_id, ConsumeOptionalString(serialized));
    ASSIGN_OR_RETURN(print_subprocess_log, Consume<bool>(serialized));
    ASSIGN_OR_RETURN(stack_limit, Consume<size_t>(serialized));
    ASSIGN_OR_RETURN(rss_limit, Consume<size_t>(serialized));
    ASSIGN_OR_RETURN(time_limit_per_input_str, ConsumeString(serialized));
    ASSIGN_OR_RETURN(time_limit_str, ConsumeString(serialized));
    ASSIGN_OR_RETURN(time_budget_type_str, ConsumeString(serialized));
    ASSIGN_OR_RETURN(jobs, Consume<size_t>(serialized));
    ASSIGN_OR_RETURN(centipede_command, ConsumeOptionalString(serialized));
    ASSIGN_OR_RETURN(crashing_input_to_reproduce,
                     ConsumeOptionalString(serialized));
    ASSIGN_OR_RETURN(reproduction_command_template,
                     ConsumeOptionalString(serialized));
    if (!serialized.empty()) {
      return absl::InvalidArgumentError(
          "Buffer is not empty after consuming a serialized configuration.");
    }
    ASSIGN_OR_RETURN(time_limit_per_input,
                     ParseDuration(*time_limit_per_input_str));
    ASSIGN_OR_RETURN(time_limit, ParseDuration(*time_limit_str));
    ASSIGN_OR_RETURN(time_budget_type,
                     ParseTimeBudgetType(*time_budget_type_str));
    return Configuration{*std::move(corpus_database),
                         *std::move(stats_root),
                         *std::move(workdir_root),
                         *std::move(binary_identifier),
                         *std::move(fuzz_tests),
                         *std::move(fuzz_tests_in_current_shard),
                         *reproduce_findings_as_separate_tests,
                         *replay_coverage_inputs,
                         *only_replay,
                         *replay_in_single_process,
                         *std::move(execution_id),
                         *print_subprocess_log,
                         *stack_limit,
                         *rss_limit,
                         *time_limit_per_input,
                         *time_limit,
                         *time_budget_type,
                         *jobs,
                         *std::move(centipede_command),
                         *std::move(crashing_input_to_reproduce),
                         *std::move(reproduction_command_template)};
  }();
}

#undef ASSIGN_OR_RETURN

absl::Duration Configuration::GetTimeLimitPerTest() const {
  switch (time_budget_type) {
    case TimeBudgetType::kPerTest:
      return time_limit;
    case TimeBudgetType::kTotal:
      return time_limit / fuzz_tests_in_current_shard.size();
    default:
      return absl::InfiniteDuration();
  }
}

}  // namespace fuzztest::internal
