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

#include "./centipede/command.h"

#include <signal.h>
#include <sys/wait.h>  // NOLINT(for WTERMSIG)

#include <cstdlib>
#include <filesystem>  // NOLINT
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "gtest/gtest.h"
#include "absl/strings/substitute.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "./centipede/stop.h"
#include "./centipede/util.h"
#include "./common/test_util.h"

namespace fuzztest::internal {
namespace {

TEST(CommandTest, ToString) {
  EXPECT_EQ(Command{"x"}.ToString(), "env \\\nx");
  {
    Command::Options cmd_options;
    cmd_options.args = {"arg1", "arg2"};
    EXPECT_EQ((Command{"path", std::move(cmd_options)}.ToString()),
              "env \\\npath \\\narg1 \\\narg2");
  }
  {
    Command::Options cmd_options;
    cmd_options.env_add = {"K1=V1", "K2=V2"};
    cmd_options.env_remove = {"K3"};
    EXPECT_EQ((Command{"x", std::move(cmd_options)}.ToString()),
              "env \\\n-u K3 \\\nK1=V1 \\\nK2=V2 \\\nx");
  }
  {
    Command::Options cmd_options;
    cmd_options.stdout_file = "out";
    EXPECT_EQ((Command{"x", std::move(cmd_options)}.ToString()),
              "env \\\nx \\\n> out");
  }
  {
    Command::Options cmd_options;
    cmd_options.stderr_file = "err";
    EXPECT_EQ((Command{"x", std::move(cmd_options)}.ToString()),
              "env \\\nx \\\n2> err");
  }
  {
    Command::Options cmd_options;
    cmd_options.stdout_file = "out";
    cmd_options.stderr_file = "err";
    EXPECT_EQ((Command{"x", std::move(cmd_options)}.ToString()),
              "env \\\nx \\\n> out \\\n2> err");
  }
  {
    Command::Options cmd_options;
    cmd_options.stdout_file = "out";
    cmd_options.stderr_file = "out";
    EXPECT_EQ((Command{"x", std::move(cmd_options)}.ToString()),
              "env \\\nx \\\n> out \\\n2>&1");
  }
}

TEST(CommandTest, Execute) {
  // Check for default exit code.
  Command echo{"echo"};
  EXPECT_EQ(echo.Execute(), 0);
  EXPECT_FALSE(ShouldStop());

  // Check for exit code 7.
  Command exit7{"bash -c 'exit 7'"};
  EXPECT_EQ(exit7.Execute(), 7);
  EXPECT_FALSE(ShouldStop());
}

TEST(CommandTest, HandlesInterruptedCommand) {
  Command self_sigint{"bash -c 'kill -SIGINT $$'"};
  self_sigint.ExecuteAsync();
  self_sigint.Wait(absl::InfiniteFuture());
  EXPECT_TRUE(ShouldStop());
  ClearEarlyStopRequestAndSetStopTime(absl::InfiniteFuture());
}

TEST(CommandTest, InputFileWildCard) {
  Command::Options cmd_options;
  cmd_options.temp_file_path = "TEMP_FILE";
  Command cmd{"foo bar @@ baz", std::move(cmd_options)};
  EXPECT_EQ(cmd.ToString(), "env \\\nfoo bar TEMP_FILE baz");
}

TEST(CommandTest, ForkServer) {
  const std::string test_tmpdir = GetTestTempDir(test_info_->name());
  const std::string helper =
      GetDataDependencyFilepath("centipede/command_test_helper");

  // TODO(ussuri): Dedupe these testcases.

  {
    const std::string input = "success";
    const std::string log = std::filesystem::path{test_tmpdir} / input;
    Command::Options cmd_options;
    cmd_options.args = {input};
    cmd_options.stdout_file = log;
    cmd_options.stderr_file = log;
    Command cmd{helper, std::move(cmd_options)};
    EXPECT_TRUE(cmd.StartForkServer(test_tmpdir, "ForkServer"));
    EXPECT_EQ(cmd.Execute(), EXIT_SUCCESS);
    std::string log_contents;
    ReadFromLocalFile(log, log_contents);
    EXPECT_EQ(log_contents, absl::Substitute("Got input: $0", input));
  }

  {
    const std::string input = "fail";
    const std::string log = std::filesystem::path{test_tmpdir} / input;
    Command::Options cmd_options;
    cmd_options.args = {input};
    cmd_options.stdout_file = log;
    cmd_options.stderr_file = log;
    Command cmd{helper, std::move(cmd_options)};
    EXPECT_TRUE(cmd.StartForkServer(test_tmpdir, "ForkServer"));
    EXPECT_EQ(cmd.Execute(), EXIT_FAILURE);
    std::string log_contents;
    ReadFromLocalFile(log, log_contents);
    EXPECT_EQ(log_contents, absl::Substitute("Got input: $0", input));
  }

  {
    const std::string input = "ret42";
    const std::string log = std::filesystem::path{test_tmpdir} / input;
    Command::Options cmd_options;
    cmd_options.args = {input};
    cmd_options.stdout_file = log;
    cmd_options.stderr_file = log;
    Command cmd{helper, std::move(cmd_options)};
    EXPECT_TRUE(cmd.StartForkServer(test_tmpdir, "ForkServer"));
    EXPECT_EQ(cmd.Execute(), 42);
    std::string log_contents;
    ReadFromLocalFile(log, log_contents);
    EXPECT_EQ(log_contents, absl::Substitute("Got input: $0", input));
  }

  {
    const std::string input = "abort";
    const std::string log = std::filesystem::path{test_tmpdir} / input;
    Command::Options cmd_options;
    cmd_options.args = {input};
    cmd_options.stdout_file = log;
    cmd_options.stderr_file = log;
    Command cmd{helper, std::move(cmd_options)};
    EXPECT_TRUE(cmd.StartForkServer(test_tmpdir, "ForkServer"));
    // WTERMSIG() needs an lvalue on some platforms.
    const int ret = cmd.Execute();
    EXPECT_EQ(WTERMSIG(ret), SIGABRT);
    std::string log_contents;
    ReadFromLocalFile(log, log_contents);
    EXPECT_EQ(log_contents, absl::Substitute("Got input: $0", input));
  }

  {
    const std::string input = "hang";
    const std::string log = std::filesystem::path{test_tmpdir} / input;
    Command::Options cmd_options;
    cmd_options.args = {input};
    cmd_options.stdout_file = log;
    cmd_options.stderr_file = log;
    Command cmd{helper, std::move(cmd_options)};
    ASSERT_TRUE(cmd.StartForkServer(test_tmpdir, "ForkServer"));
    ASSERT_TRUE(cmd.ExecuteAsync());
    EXPECT_EQ(cmd.Wait(absl::Now() + absl::Seconds(2)), std::nullopt);
    std::string log_contents;
    ReadFromLocalFile(log, log_contents);
    EXPECT_EQ(log_contents, absl::Substitute("Got input: $0", input));
  }

  // TODO(kcc): [impl] test what happens if the child is interrupted.
}

}  // namespace
}  // namespace fuzztest::internal
