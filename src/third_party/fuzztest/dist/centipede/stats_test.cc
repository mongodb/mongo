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

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>  // NOLINT
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/log/check.h"
#include "absl/log/log_entry.h"
#include "absl/log/log_sink.h"
#include "absl/log/log_sink_registry.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "absl/time/civil_time.h"
#include "absl/time/time.h"
#include "./centipede/environment.h"
#include "./centipede/util.h"
#include "./common/logging.h"  // IWYU pragma: keep
#include "./common/test_util.h"

namespace fuzztest::internal {
namespace {

using ::testing::ElementsAreArray;

namespace {

class LogCapture : public absl::LogSink {
 public:
  LogCapture() { absl::AddLogSink(this); }
  ~LogCapture() override { absl::RemoveLogSink(this); }
  void Send(const absl::LogEntry &entry) override {
    captured_log_.emplace_back(entry.text_message());
  }
  std::vector<std::string> CapturedLogLines() const {
    // Join->Split normalizes multi-line messages.
    return absl::StrSplit(absl::StrJoin(captured_log_, "\n"), '\n');
  }

 private:
  std::vector<std::string> captured_log_;
};

uint64_t CivilTimeToUnixMicros(  //
    int64_t y, int64_t m, int64_t d, int64_t hh, int64_t mm, int64_t ss) {
  return absl::ToUnixMicros(absl::FromCivil(
      absl::CivilSecond{y, m, d, hh, mm, ss}, absl::LocalTimeZone()));
}

Environment CreateEnv(std::string_view workdir,
                      std::string_view experiment_name = "",
                      std::string_view experiment_flags = "") {
  Environment env;
  env.workdir = std::string(workdir);
  env.experiment_name = std::string(experiment_name);
  env.experiment_flags = std::string(experiment_flags);
  return env;
}

}  // namespace

TEST(Stats, PrintStatsToLog) {
  std::vector<std::atomic<Stats>> stats_vec(4);
  for (uint64_t i = 0; i < stats_vec.size(); ++i) {
    const uint64_t j = i + 1;

    CovStats cov_stats = {/*num_covered_pcs=*/21 * j,
                          /*num_8bit_counter_features=*/22 * j,
                          /*num_data_flow_features=*/23 * j,
                          /*num_cmp_features=*/24 * j,
                          /*num_call_stack_features=*/25 * j,
                          /*num_bounded_path_features=*/26 * j,
                          /*num_pc_pair_features=*/27 * j,
                          /*num_user_features=*/28 * j};
    // NOTE: These two don't add up to `num_user_features`, but that's
    // ok for the purposes of this test.
    cov_stats.num_user0_features = 28 * j + 1;
    cov_stats.num_user10_features = 28 * j + 2;
    cov_stats.num_unknown_features = 29 * j;
    cov_stats.num_funcs_in_frontier = 31 * j;

    stats_vec[i].store(Stats{
        StatsMeta{/*timestamp_unix_micros=*/CivilTimeToUnixMicros(1970, 1, 1, 0,
                                                                  0, i)},
        ExecStats{
            /*fuzz_time_sec=*/10 * j,
            /*num_executions=*/12 * j,
            /*num_target_crashes=*/13 * j,
        },
        cov_stats,
        CorpusStats{
            /*active_corpus_size=*/101 * j,
            /*total_corpus_size=*/102 * j,
            /*max_corpus_element_size=*/103 * j,
            /*avg_corpus_element_size=*/104 * j,
        },
        RusageStats{
            /*engine_rusage_avg_millicores=*/201 * j,
            /*engine_rusage_cpu_percent=*/202 * j,
            /*engine_rusage_rss_mb=*/203 * j,
            /*engine_rusage_vsize_mb=*/204 * j,
        },
    });
  }

  const std::vector<Environment> env_vec = {
      CreateEnv(
          /*workdir=*/"", /*experiment_name=*/"Experiment A",
          /*experiment_flags=*/"AAA"),
      CreateEnv(/*workdir=*/"", /*experiment_name=*/"Experiment B",
                /*experiment_flags=*/"BBB"),
      CreateEnv(
          /*workdir=*/"", /*experiment_name=*/"Experiment A",
          /*experiment_flags=*/"AAA"),
      CreateEnv(/*workdir=*/"", /*experiment_name=*/"Experiment B",
                /*experiment_flags=*/"BBB")};

  StatsLogger stats_logger{stats_vec, env_vec};

  {
    const std::vector<std::string_view> kExpectedLogLines = {
        "Current stats:",
        "Coverage:",
        "  Experiment A: min:\t21\tmax:\t63\tavg:\t42.0\t--\t21\t63",
        "  Experiment B: min:\t42\tmax:\t84\tavg:\t63.0\t--\t42\t84",
        "Number of executions:",
        "  Experiment A: min:\t12\tmax:\t36\tavg:\t24.0\t--\t12\t36",
        "  Experiment B: min:\t24\tmax:\t48\tavg:\t36.0\t--\t24\t48",
        "Active corpus size:",
        "  Experiment A: min:\t101\tmax:\t303\tavg:\t202.0\t--\t101\t303",
        "  Experiment B: min:\t202\tmax:\t404\tavg:\t303.0\t--\t202\t404",
        "Max element size:",
        "  Experiment A: min:\t103\tmax:\t309\tavg:\t206.0\t--\t103\t309",
        "  Experiment B: min:\t206\tmax:\t412\tavg:\t309.0\t--\t206\t412",
        "Avg element size:",
        "  Experiment A: min:\t104\tmax:\t312\tavg:\t208.0\t--\t104\t312",
        "  Experiment B: min:\t208\tmax:\t416\tavg:\t312.0\t--\t208\t416",
        "Fuzz time (sec):",
        "  Experiment A: min:\t10\tmax:\t30\tavg:\t20.0\t--\t10\t30",
        "  Experiment B: min:\t20\tmax:\t40\tavg:\t30.0\t--\t20\t40",
        "Num proxy crashes:",
        "  Experiment A: min:\t13\tmax:\t39\tsum:\t52\t--\t13\t39",
        "  Experiment B: min:\t26\tmax:\t52\tsum:\t78\t--\t26\t52",
        "Total corpus size:",
        "  Experiment A: min:\t102\tmax:\t306\tsum:\t408\t--\t102\t306",
        "  Experiment B: min:\t204\tmax:\t408\tsum:\t612\t--\t204\t408",
        "Num 8-bit counter features:",
        "  Experiment A: min:\t22\tmax:\t66\tavg:\t44.0\t--\t22\t66",
        "  Experiment B: min:\t44\tmax:\t88\tavg:\t66.0\t--\t44\t88",
        "Num data flow features:",
        "  Experiment A: min:\t23\tmax:\t69\tavg:\t46.0\t--\t23\t69",
        "  Experiment B: min:\t46\tmax:\t92\tavg:\t69.0\t--\t46\t92",
        "Num cmp features:",
        "  Experiment A: min:\t24\tmax:\t72\tavg:\t48.0\t--\t24\t72",
        "  Experiment B: min:\t48\tmax:\t96\tavg:\t72.0\t--\t48\t96",
        "Num call stack features:",
        "  Experiment A: min:\t25\tmax:\t75\tavg:\t50.0\t--\t25\t75",
        "  Experiment B: min:\t50\tmax:\t100\tavg:\t75.0\t--\t50\t100",
        "Num bounded path features:",
        "  Experiment A: min:\t26\tmax:\t78\tavg:\t52.0\t--\t26\t78",
        "  Experiment B: min:\t52\tmax:\t104\tavg:\t78.0\t--\t52\t104",
        "Num PC pair features:",
        "  Experiment A: min:\t27\tmax:\t81\tavg:\t54.0\t--\t27\t81",
        "  Experiment B: min:\t54\tmax:\t108\tavg:\t81.0\t--\t54\t108",
        "Num user features:",
        "  Experiment A: min:\t28\tmax:\t84\tavg:\t56.0\t--\t28\t84",
        "  Experiment B: min:\t56\tmax:\t112\tavg:\t84.0\t--\t56\t112",
        "Num unknown features:",
        "  Experiment A: min:\t29\tmax:\t87\tavg:\t58.0\t--\t29\t87",
        "  Experiment B: min:\t58\tmax:\t116\tavg:\t87.0\t--\t58\t116",
        "Num funcs in frontier:",
        "  Experiment A: min:\t31\tmax:\t93\tavg:\t62.0\t--\t31\t93",
        "  Experiment B: min:\t62\tmax:\t124\tavg:\t93.0\t--\t62\t124",
        "Num user0 features:",
        "  Experiment A: min:\t29\tmax:\t85\tavg:\t57.0\t--\t29\t85",
        "  Experiment B: min:\t57\tmax:\t113\tavg:\t85.0\t--\t57\t113",
        "Num user1 features:",
        "  Experiment A: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "  Experiment B: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "Num user2 features:",
        "  Experiment A: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "  Experiment B: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "Num user3 features:",
        "  Experiment A: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "  Experiment B: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "Num user4 features:",
        "  Experiment A: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "  Experiment B: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "Num user5 features:",
        "  Experiment A: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "  Experiment B: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "Num user6 features:",
        "  Experiment A: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "  Experiment B: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "Num user7 features:",
        "  Experiment A: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "  Experiment B: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "Num user8 features:",
        "  Experiment A: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "  Experiment B: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "Num user9 features:",
        "  Experiment A: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "  Experiment B: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "Num user10 features:",
        "  Experiment A: min:\t30\tmax:\t86\tavg:\t58.0\t--\t30\t86",
        "  Experiment B: min:\t58\tmax:\t114\tavg:\t86.0\t--\t58\t114",
        "Num user11 features:",
        "  Experiment A: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "  Experiment B: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "Num user12 features:",
        "  Experiment A: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "  Experiment B: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "Num user13 features:",
        "  Experiment A: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "  Experiment B: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "Num user14 features:",
        "  Experiment A: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "  Experiment B: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "Num user15 features:",
        "  Experiment A: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "  Experiment B: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "Flags:",
        "  Experiment A: AAA",
        "  Experiment B: BBB",
    };

    LogCapture log_capture;
    stats_logger.ReportCurrStats();

    const auto log_lines = log_capture.CapturedLogLines();
    EXPECT_THAT(log_lines, ElementsAreArray(kExpectedLogLines))
        << "\nActual:" << absl::StrJoin(log_lines, "\n") << "\nExpected:\n"
        << absl::StrJoin(kExpectedLogLines, "\n");
  }

  {
    for (auto &stats : stats_vec) {
      auto new_stats = stats.load();

      new_stats.timestamp_unix_micros += 1000000;

      new_stats.fuzz_time_sec += 1;
      new_stats.num_executions += 1;
      new_stats.num_target_crashes += 1;

      new_stats.num_covered_pcs += 1;
      new_stats.num_8bit_counter_features += 1;
      new_stats.num_data_flow_features += 1;
      new_stats.num_cmp_features += 1;
      new_stats.num_call_stack_features += 1;
      new_stats.num_bounded_path_features += 1;
      new_stats.num_pc_pair_features += 1;
      new_stats.num_user_features += 1;
      new_stats.num_user0_features += 1;
      new_stats.num_user10_features += 1;
      new_stats.num_unknown_features += 1;
      new_stats.num_funcs_in_frontier += 1;

      new_stats.active_corpus_size += 1;
      new_stats.total_corpus_size += 1;
      new_stats.max_corpus_element_size += 1;
      new_stats.avg_corpus_element_size += 1;

      new_stats.engine_rusage_avg_millicores += 1;
      new_stats.engine_rusage_cpu_percent += 1;
      new_stats.engine_rusage_rss_mb += 1;
      new_stats.engine_rusage_vsize_mb += 1;

      stats.store(new_stats);
    }

    const std::vector<std::string_view> kExpectedLogLines = {
        "Current stats:",
        "Coverage:",
        "  Experiment A: min:\t22\tmax:\t64\tavg:\t43.0\t--\t22\t64",
        "  Experiment B: min:\t43\tmax:\t85\tavg:\t64.0\t--\t43\t85",
        "Number of executions:",
        "  Experiment A: min:\t13\tmax:\t37\tavg:\t25.0\t--\t13\t37",
        "  Experiment B: min:\t25\tmax:\t49\tavg:\t37.0\t--\t25\t49",
        "Active corpus size:",
        "  Experiment A: min:\t102\tmax:\t304\tavg:\t203.0\t--\t102\t304",
        "  Experiment B: min:\t203\tmax:\t405\tavg:\t304.0\t--\t203\t405",
        "Max element size:",
        "  Experiment A: min:\t104\tmax:\t310\tavg:\t207.0\t--\t104\t310",
        "  Experiment B: min:\t207\tmax:\t413\tavg:\t310.0\t--\t207\t413",
        "Avg element size:",
        "  Experiment A: min:\t105\tmax:\t313\tavg:\t209.0\t--\t105\t313",
        "  Experiment B: min:\t209\tmax:\t417\tavg:\t313.0\t--\t209\t417",
        "Fuzz time (sec):",
        "  Experiment A: min:\t11\tmax:\t31\tavg:\t21.0\t--\t11\t31",
        "  Experiment B: min:\t21\tmax:\t41\tavg:\t31.0\t--\t21\t41",
        "Num proxy crashes:",
        "  Experiment A: min:\t14\tmax:\t40\tsum:\t54\t--\t14\t40",
        "  Experiment B: min:\t27\tmax:\t53\tsum:\t80\t--\t27\t53",
        "Total corpus size:",
        "  Experiment A: min:\t103\tmax:\t307\tsum:\t410\t--\t103\t307",
        "  Experiment B: min:\t205\tmax:\t409\tsum:\t614\t--\t205\t409",
        "Num 8-bit counter features:",
        "  Experiment A: min:\t23\tmax:\t67\tavg:\t45.0\t--\t23\t67",
        "  Experiment B: min:\t45\tmax:\t89\tavg:\t67.0\t--\t45\t89",
        "Num data flow features:",
        "  Experiment A: min:\t24\tmax:\t70\tavg:\t47.0\t--\t24\t70",
        "  Experiment B: min:\t47\tmax:\t93\tavg:\t70.0\t--\t47\t93",
        "Num cmp features:",
        "  Experiment A: min:\t25\tmax:\t73\tavg:\t49.0\t--\t25\t73",
        "  Experiment B: min:\t49\tmax:\t97\tavg:\t73.0\t--\t49\t97",
        "Num call stack features:",
        "  Experiment A: min:\t26\tmax:\t76\tavg:\t51.0\t--\t26\t76",
        "  Experiment B: min:\t51\tmax:\t101\tavg:\t76.0\t--\t51\t101",
        "Num bounded path features:",
        "  Experiment A: min:\t27\tmax:\t79\tavg:\t53.0\t--\t27\t79",
        "  Experiment B: min:\t53\tmax:\t105\tavg:\t79.0\t--\t53\t105",
        "Num PC pair features:",
        "  Experiment A: min:\t28\tmax:\t82\tavg:\t55.0\t--\t28\t82",
        "  Experiment B: min:\t55\tmax:\t109\tavg:\t82.0\t--\t55\t109",
        "Num user features:",
        "  Experiment A: min:\t29\tmax:\t85\tavg:\t57.0\t--\t29\t85",
        "  Experiment B: min:\t57\tmax:\t113\tavg:\t85.0\t--\t57\t113",
        "Num unknown features:",
        "  Experiment A: min:\t30\tmax:\t88\tavg:\t59.0\t--\t30\t88",
        "  Experiment B: min:\t59\tmax:\t117\tavg:\t88.0\t--\t59\t117",
        "Num funcs in frontier:",
        "  Experiment A: min:\t32\tmax:\t94\tavg:\t63.0\t--\t32\t94",
        "  Experiment B: min:\t63\tmax:\t125\tavg:\t94.0\t--\t63\t125",
        "Num user0 features:",
        "  Experiment A: min:\t30\tmax:\t86\tavg:\t58.0\t--\t30\t86",
        "  Experiment B: min:\t58\tmax:\t114\tavg:\t86.0\t--\t58\t114",
        "Num user1 features:",
        "  Experiment A: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "  Experiment B: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "Num user2 features:",
        "  Experiment A: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "  Experiment B: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "Num user3 features:",
        "  Experiment A: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "  Experiment B: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "Num user4 features:",
        "  Experiment A: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "  Experiment B: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "Num user5 features:",
        "  Experiment A: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "  Experiment B: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "Num user6 features:",
        "  Experiment A: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "  Experiment B: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "Num user7 features:",
        "  Experiment A: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "  Experiment B: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "Num user8 features:",
        "  Experiment A: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "  Experiment B: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "Num user9 features:",
        "  Experiment A: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "  Experiment B: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "Num user10 features:",
        "  Experiment A: min:\t31\tmax:\t87\tavg:\t59.0\t--\t31\t87",
        "  Experiment B: min:\t59\tmax:\t115\tavg:\t87.0\t--\t59\t115",
        "Num user11 features:",
        "  Experiment A: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "  Experiment B: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "Num user12 features:",
        "  Experiment A: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "  Experiment B: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "Num user13 features:",
        "  Experiment A: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "  Experiment B: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "Num user14 features:",
        "  Experiment A: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "  Experiment B: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "Num user15 features:",
        "  Experiment A: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "  Experiment B: min:\t0\tmax:\t0\tavg:\t0.0\t--\t0\t0",
        "Flags:",
        "  Experiment A: AAA",
        "  Experiment B: BBB",
    };

    LogCapture log_capture;
    stats_logger.ReportCurrStats();

    const auto log_lines = log_capture.CapturedLogLines();
    EXPECT_THAT(log_lines, ElementsAreArray(kExpectedLogLines));
  }
}

TEST(Stats, DumpStatsToCsvFile) {
  const std::filesystem::path workdir = GetTestTempDir(test_info_->name());

  std::vector<std::atomic<Stats>> stats_vec(4);
  for (uint64_t i = 0; i < stats_vec.size(); ++i) {
    const uint64_t j = i + 1;

    CovStats cov_stats{/*num_covered_pcs=*/
                       21 * j,
                       /*num_8bit_counter_features=*/22 * j,
                       /*num_data_flow_features=*/23 * j,
                       /*num_cmp_features=*/24 * j,
                       /*num_call_stack_features=*/25 * j,
                       /*num_bounded_path_features=*/26 * j,
                       /*num_pc_pair_features=*/27 * j,
                       /*num_user_features=*/28 * j};
    // NOTE: These two don't add up to `num_user_features`, but that's
    // ok for the purposes of this test.
    cov_stats.num_user0_features = 28 * j + 1;
    cov_stats.num_user10_features = 28 * j + 2;
    cov_stats.num_unknown_features = 29 * j;
    cov_stats.num_funcs_in_frontier = 31 * j;

    stats_vec[i].store(Stats{
        StatsMeta{/*timestamp_unix_micros=*/1000000 * j},
        ExecStats{
            /*fuzz_time_sec=*/10 * j,
            /*.num_executions=*/12 * j,
            /*.num_target_crashes=*/13 * j,
        },
        cov_stats,
        CorpusStats{
            /*active_corpus_size=*/101 * j,
            /*total_corpus_size=*/102 * j,
            /*max_corpus_element_size=*/103 * j,
            /*avg_corpus_element_size=*/104 * j,
        },
        RusageStats{
            /*engine_rusage_avg_millicores=*/201 * j,
            /*engine_rusage_cpu_percent=*/202 * j,
            /*engine_rusage_rss_mb=*/203 * j,
            /*engine_rusage_vsize_mb=*/204 * j,
        },
    });
  }

  const std::vector<Environment> env_vec = {
      CreateEnv(workdir.c_str(), /*experiment_name=*/"ExperimentA",
                /*experiment_flags=*/"AAA"),
      CreateEnv(workdir.c_str(), /*experiment_name=*/"ExperimentB",
                /*experiment_flags=*/"BBB"),
      CreateEnv(workdir.c_str(), /*experiment_name=*/"ExperimentA",
                /*experiment_flags=*/"AAA"),
      CreateEnv(workdir.c_str(), /*experiment_name=*/"ExperimentB",
                /*experiment_flags=*/"BBB")};

  {
    StatsCsvFileAppender stats_csv_appender{stats_vec, env_vec};
    stats_csv_appender.ReportCurrStats();

    // Emulate progress in shard #2 of each experiment. In the second line of
    // each CSV, min's shouldn't change, max's should increase by 1, avg's
    // should increase by 0.5.
    for (int i = 2; i < stats_vec.size(); ++i) {
      auto &stats = stats_vec[i];
      auto new_stats = stats.load();

      new_stats.timestamp_unix_micros += 1;

      new_stats.fuzz_time_sec += 1;
      new_stats.num_executions += 1;
      new_stats.num_target_crashes += 1;

      new_stats.num_covered_pcs += 1;
      new_stats.num_8bit_counter_features += 1;
      new_stats.num_data_flow_features += 1;
      new_stats.num_cmp_features += 1;
      new_stats.num_call_stack_features += 1;
      new_stats.num_bounded_path_features += 1;
      new_stats.num_pc_pair_features += 1;
      new_stats.num_user_features += 1;
      new_stats.num_user0_features += 1;
      new_stats.num_user10_features += 1;
      new_stats.num_unknown_features += 1;
      new_stats.num_funcs_in_frontier += 1;

      new_stats.active_corpus_size += 1;
      new_stats.total_corpus_size += 1;
      new_stats.max_corpus_element_size += 1;
      new_stats.avg_corpus_element_size += 1;

      new_stats.engine_rusage_avg_millicores += 1;
      new_stats.engine_rusage_cpu_percent += 1;
      new_stats.engine_rusage_rss_mb += 1;
      new_stats.engine_rusage_vsize_mb += 1;

      stats.store(new_stats);
    }

    stats_csv_appender.ReportCurrStats();
  }

  const std::vector<std::string> kExpectedCsvs = {
      workdir / "fuzzing-stats-.000000.ExperimentA.csv",
      workdir / "fuzzing-stats-.000000.ExperimentB.csv",
  };
  const std::vector<std::vector<std::string>> kExpectedCsvLines = {
      // CSV #1.
      {
          // clang-format off
          // Header.
          "NumCoveredPcs_Min,NumCoveredPcs_Max,NumCoveredPcs_Avg,"
          "NumExecs_Min,NumExecs_Max,NumExecs_Avg,"
          "ActiveCorpusSize_Min,ActiveCorpusSize_Max,ActiveCorpusSize_Avg,"
          "MaxEltSize_Min,MaxEltSize_Max,MaxEltSize_Avg,"
          "AvgEltSize_Min,AvgEltSize_Max,AvgEltSize_Avg,"
          "UnixMicros_Min,UnixMicros_Max,"
          "FuzzTimeSec_Min,FuzzTimeSec_Max,FuzzTimeSec_Avg,"
          "NumProxyCrashes_Min,NumProxyCrashes_Max,NumProxyCrashes_Sum,"
          "TotalCorpusSize_Min,TotalCorpusSize_Max,TotalCorpusSize_Sum,"
          "Num8BitCounterFts_Min,Num8BitCounterFts_Max,Num8BitCounterFts_Avg,"
          "NumDataFlowFts_Min,NumDataFlowFts_Max,NumDataFlowFts_Avg,"
          "NumCmpFts_Min,NumCmpFts_Max,NumCmpFts_Avg,"
          "NumCallStackFts_Min,NumCallStackFts_Max,NumCallStackFts_Avg,"
          "NumBoundedPathFts_Min,NumBoundedPathFts_Max,NumBoundedPathFts_Avg,"
          "NumPcPairFts_Min,NumPcPairFts_Max,NumPcPairFts_Avg,"
          "NumUserFts_Min,NumUserFts_Max,NumUserFts_Avg,"
          "NumUnknownFts_Min,NumUnknownFts_Max,NumUnknownFts_Avg,"
          "NumFuncsInFrontier_Min,NumFuncsInFrontier_Max,NumFuncsInFrontier_Avg,"  // NOLINT
          "EngineRusageAvgCores_Max,EngineRusageCpuPct_Max,"
          "EngineRusageRssMb_Max,EngineRusageVSizeMb_Max,"
          "NumUser0Fts_Min,NumUser0Fts_Max,NumUser0Fts_Avg,"
          "NumUser1Fts_Min,NumUser1Fts_Max,NumUser1Fts_Avg,"
          "NumUser2Fts_Min,NumUser2Fts_Max,NumUser2Fts_Avg,"
          "NumUser3Fts_Min,NumUser3Fts_Max,NumUser3Fts_Avg,"
          "NumUser4Fts_Min,NumUser4Fts_Max,NumUser4Fts_Avg,"
          "NumUser5Fts_Min,NumUser5Fts_Max,NumUser5Fts_Avg,"
          "NumUser6Fts_Min,NumUser6Fts_Max,NumUser6Fts_Avg,"
          "NumUser7Fts_Min,NumUser7Fts_Max,NumUser7Fts_Avg,"
          "NumUser8Fts_Min,NumUser8Fts_Max,NumUser8Fts_Avg,"
          "NumUser9Fts_Min,NumUser9Fts_Max,NumUser9Fts_Avg,"
          "NumUser10Fts_Min,NumUser10Fts_Max,NumUser10Fts_Avg,"
          "NumUser11Fts_Min,NumUser11Fts_Max,NumUser11Fts_Avg,"
          "NumUser12Fts_Min,NumUser12Fts_Max,NumUser12Fts_Avg,"
          "NumUser13Fts_Min,NumUser13Fts_Max,NumUser13Fts_Avg,"
          "NumUser14Fts_Min,NumUser14Fts_Max,NumUser14Fts_Avg,"
          "NumUser15Fts_Min,NumUser15Fts_Max,NumUser15Fts_Avg,",
          // Line 1.
          "21,63,42.0,"
          "12,36,24.0,"
          "101,303,202.0,"
          "103,309,206.0,"
          "104,312,208.0,"
          "1000000,3000000,"
          "10,30,20.0,"
          "13,39,52,"
          "102,306,408,"
          "22,66,44.0,"
          "23,69,46.0,"
          "24,72,48.0,"
          "25,75,50.0,"
          "26,78,52.0,"
          "27,81,54.0,"
          "28,84,56.0,"
          "29,87,58.0,"
          "31,93,62.0,"
          "603,606,"
          "609,612,"
          "29,85,57.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "30,86,58.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,",
          // Line 2.
          "21,64,42.5,"
          "12,37,24.5,"
          "101,304,202.5,"
          "103,310,206.5,"
          "104,313,208.5,"
          "1000000,3000001,"
          "10,31,20.5,"
          "13,40,53,"
          "102,307,409,"
          "22,67,44.5,"
          "23,70,46.5,"
          "24,73,48.5,"
          "25,76,50.5,"
          "26,79,52.5,"
          "27,82,54.5,"
          "28,85,56.5,"
          "29,88,58.5,"
          "31,94,62.5,"
          "604,607,"
          "610,613,"
          "29,86,57.5,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "30,87,58.5,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,",
          "",  // empty line at EOF
          // clang-format on
      },
      // CSV #2.
      {
          // clang-format off
          // Header.
          "NumCoveredPcs_Min,NumCoveredPcs_Max,NumCoveredPcs_Avg,"
          "NumExecs_Min,NumExecs_Max,NumExecs_Avg,"
          "ActiveCorpusSize_Min,ActiveCorpusSize_Max,ActiveCorpusSize_Avg,"
          "MaxEltSize_Min,MaxEltSize_Max,MaxEltSize_Avg,"
          "AvgEltSize_Min,AvgEltSize_Max,AvgEltSize_Avg,"
          "UnixMicros_Min,UnixMicros_Max,"
          "FuzzTimeSec_Min,FuzzTimeSec_Max,FuzzTimeSec_Avg,"
          "NumProxyCrashes_Min,NumProxyCrashes_Max,NumProxyCrashes_Sum,"
          "TotalCorpusSize_Min,TotalCorpusSize_Max,TotalCorpusSize_Sum,"
          "Num8BitCounterFts_Min,Num8BitCounterFts_Max,Num8BitCounterFts_Avg,"
          "NumDataFlowFts_Min,NumDataFlowFts_Max,NumDataFlowFts_Avg,"
          "NumCmpFts_Min,NumCmpFts_Max,NumCmpFts_Avg,"
          "NumCallStackFts_Min,NumCallStackFts_Max,NumCallStackFts_Avg,"
          "NumBoundedPathFts_Min,NumBoundedPathFts_Max,NumBoundedPathFts_Avg,"
          "NumPcPairFts_Min,NumPcPairFts_Max,NumPcPairFts_Avg,"
          "NumUserFts_Min,NumUserFts_Max,NumUserFts_Avg,"
          "NumUnknownFts_Min,NumUnknownFts_Max,NumUnknownFts_Avg,"
          "NumFuncsInFrontier_Min,NumFuncsInFrontier_Max,NumFuncsInFrontier_Avg,"  // NOLINT
          "EngineRusageAvgCores_Max,EngineRusageCpuPct_Max,"
          "EngineRusageRssMb_Max,EngineRusageVSizeMb_Max,"
          "NumUser0Fts_Min,NumUser0Fts_Max,NumUser0Fts_Avg,"
          "NumUser1Fts_Min,NumUser1Fts_Max,NumUser1Fts_Avg,"
          "NumUser2Fts_Min,NumUser2Fts_Max,NumUser2Fts_Avg,"
          "NumUser3Fts_Min,NumUser3Fts_Max,NumUser3Fts_Avg,"
          "NumUser4Fts_Min,NumUser4Fts_Max,NumUser4Fts_Avg,"
          "NumUser5Fts_Min,NumUser5Fts_Max,NumUser5Fts_Avg,"
          "NumUser6Fts_Min,NumUser6Fts_Max,NumUser6Fts_Avg,"
          "NumUser7Fts_Min,NumUser7Fts_Max,NumUser7Fts_Avg,"
          "NumUser8Fts_Min,NumUser8Fts_Max,NumUser8Fts_Avg,"
          "NumUser9Fts_Min,NumUser9Fts_Max,NumUser9Fts_Avg,"
          "NumUser10Fts_Min,NumUser10Fts_Max,NumUser10Fts_Avg,"
          "NumUser11Fts_Min,NumUser11Fts_Max,NumUser11Fts_Avg,"
          "NumUser12Fts_Min,NumUser12Fts_Max,NumUser12Fts_Avg,"
          "NumUser13Fts_Min,NumUser13Fts_Max,NumUser13Fts_Avg,"
          "NumUser14Fts_Min,NumUser14Fts_Max,NumUser14Fts_Avg,"
          "NumUser15Fts_Min,NumUser15Fts_Max,NumUser15Fts_Avg,",
          // Line 1.
          "42,84,63.0,"
          "24,48,36.0,"
          "202,404,303.0,"
          "206,412,309.0,"
          "208,416,312.0,"
          "2000000,4000000,"
          "20,40,30.0,"
          "26,52,78,"
          "204,408,612,"
          "44,88,66.0,"
          "46,92,69.0,"
          "48,96,72.0,"
          "50,100,75.0,"
          "52,104,78.0,"
          "54,108,81.0,"
          "56,112,84.0,"
          "58,116,87.0,"
          "62,124,93.0,"
          "804,808,"
          "812,816,"
          "57,113,85.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "58,114,86.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,",
          // Line 2.
          "42,85,63.5,"
          "24,49,36.5,"
          "202,405,303.5,"
          "206,413,309.5,"
          "208,417,312.5,"
          "2000000,4000001,"
          "20,41,30.5,"
          "26,53,79,"
          "204,409,613,"
          "44,89,66.5,"
          "46,93,69.5,"
          "48,97,72.5,"
          "50,101,75.5,"
          "52,105,78.5,"
          "54,109,81.5,"
          "56,113,84.5,"
          "58,117,87.5,"
          "62,125,93.5,"
          "805,809,"
          "813,817,"
          "57,114,85.5,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "58,115,86.5,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,"
          "0,0,0.0,",
          "",  // empty line at EOF
          // clang-format on
      },
  };

  for (int i = 0; i < 2; ++i) {
    ASSERT_TRUE(std::filesystem::exists(kExpectedCsvs[i]));
    std::string csv_contents;
    ReadFromLocalFile(kExpectedCsvs[i], csv_contents);
    const std::vector<std::string_view> csv_lines =
        absl::StrSplit(csv_contents, '\n');
    ASSERT_EQ(csv_lines.size(), kExpectedCsvLines[i].size());
    for (size_t j = 0; j < csv_lines.size(); ++j) {
      EXPECT_EQ(csv_lines[j], kExpectedCsvLines[i][j]) << VV(i) << VV(j);
    }
  }
}

TEST(Stats, DumpStatsToExistingCsvFile) {
  class TestStatsCsvFileAppender : public StatsCsvFileAppender {
    using StatsCsvFileAppender::StatsCsvFileAppender;

   private:
    std::string GetBackupFilename(const std::string &filename) const override {
      return absl::StrCat(filename, ".bak");
    }
  };

  const std::filesystem::path workdir = GetTestTempDir(test_info_->name());
  const std::vector<Environment> env_vec = {CreateEnv(workdir.c_str()),
                                            CreateEnv(workdir.c_str())};

  std::vector<std::atomic<Stats>> stats_vec(2);
  stats_vec[0].store({StatsMeta{/*timestamp_unix_micros=*/1000000}});
  stats_vec[1].store({StatsMeta{/*timestamp_unix_micros=*/2000000}});

  const std::string kExpectedCsv = workdir / "fuzzing-stats-.000000.csv";
  const std::string kExpectedCsvBak = workdir / "fuzzing-stats-.000000.csv.bak";

  // `StatsCsvFileAppender` creates a brand-new fuzzing-stats file and writes
  // the CSV header and a 1st stats line to it.
  {
    TestStatsCsvFileAppender stats_csv_appender{stats_vec, env_vec};
    stats_csv_appender.ReportCurrStats();

    ASSERT_TRUE(std::filesystem::exists(kExpectedCsv));
    ASSERT_FALSE(std::filesystem::exists(kExpectedCsvBak));

    std::string contents;
    ReadFromLocalFile(kExpectedCsv, contents);
    const std::vector<std::string> lines = absl::StrSplit(contents, '\n');
    // Header + 1 stats line + empty line at EOF.
    EXPECT_EQ(lines.size(), 3);
  }

  // `StatsCsvFileAppender` finds an existing file with a CSV header matching
  // the current version and appends a 2nd stats line to it.
  {
    stats_vec[0].store({StatsMeta{/*timestamp_unix_micros=*/3000000}});
    stats_vec[1].store({StatsMeta{/*timestamp_unix_micros=*/4000000}});

    TestStatsCsvFileAppender stats_csv_appender{stats_vec, env_vec};
    stats_csv_appender.ReportCurrStats();

    ASSERT_TRUE(std::filesystem::exists(kExpectedCsv));
    ASSERT_FALSE(std::filesystem::exists(kExpectedCsvBak));

    std::string contents;
    ReadFromLocalFile(kExpectedCsv, contents);
    const std::vector<std::string> lines = absl::StrSplit(contents, '\n');
    // Header + 2 stats lines + empty line at EOF.
    EXPECT_EQ(lines.size(), 4);
  }

  // `StatsCsvFileAppender` finds an existing file with a CSV header not
  // matching the current version (ostensibly created by a previous version of
  // Centipede), creates a backup copy of it, and starts a new file scratch,
  // writing the new CSV header and a 1st line to it.
  {
    // Fake a CSV with an outdated header and contents.
    const std::vector<std::string> kFakeOldCsvLines = {
        "Old,Csv,Header,", "0,1,2,", "10,11,12,", "20,21,22,", "",
    };
    WriteToLocalFile(kExpectedCsv, absl::StrJoin(kFakeOldCsvLines, "\n"));

    stats_vec[0].store({StatsMeta{/*timestamp_unix_micros=*/5000000}});
    stats_vec[1].store({StatsMeta{/*timestamp_unix_micros=*/6000000}});

    TestStatsCsvFileAppender stats_csv_appender{stats_vec, env_vec};
    stats_csv_appender.ReportCurrStats();

    ASSERT_TRUE(std::filesystem::exists(kExpectedCsv));
    ASSERT_TRUE(std::filesystem::exists(kExpectedCsvBak));

    std::string contents;
    ReadFromLocalFile(kExpectedCsv, contents);
    const std::vector<std::string> lines = absl::StrSplit(contents, '\n');
    // Header + 1 stats line + empty line at EOF.
    EXPECT_EQ(lines.size(), 3);

    std::string contents_bak;
    ReadFromLocalFile(kExpectedCsvBak, contents_bak);
    const std::vector<std::string> lines_bak =
        absl::StrSplit(contents_bak, '\n');
    CHECK_NE(lines.size(), kFakeOldCsvLines.size());  // Prevent false positives
    EXPECT_EQ(lines_bak.size(), kFakeOldCsvLines.size());
  }
}

TEST(Stats, PrintRewardValues) {
  std::stringstream ss;
  std::vector<std::atomic<Stats>> stats_vec(4);
  stats_vec[0].store(
      {StatsMeta{}, ExecStats{}, CovStats{/*num_covered_pcs=*/15}});
  stats_vec[1].store(
      {StatsMeta{}, ExecStats{}, CovStats{/*num_covered_pcs=*/10}});
  stats_vec[2].store(
      {StatsMeta{}, ExecStats{}, CovStats{/*num_covered_pcs=*/40}});
  stats_vec[3].store(
      {StatsMeta{}, ExecStats{}, CovStats{/*num_covered_pcs=*/25}});
  PrintRewardValues(stats_vec, ss);
  const char *expected =
      "REWARD_MAX 40\n"
      "REWARD_SECOND_MAX 25\n"
      "REWARD_MIN 10\n"
      "REWARD_MEDIAN 25\n"
      "REWARD_AVERAGE 22.5\n";
  EXPECT_EQ(ss.str(), expected);
}

}  // namespace
}  // namespace fuzztest::internal
