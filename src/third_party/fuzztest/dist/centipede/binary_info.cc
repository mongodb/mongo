// Copyright 2023 The Centipede Authors.
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

#include "./centipede/binary_info.h"

#include <cstdlib>
#include <filesystem>  // NOLINT
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "./centipede/command.h"
#include "./centipede/control_flow.h"
#include "./centipede/pc_info.h"
#include "./centipede/util.h"
#include "./common/remote_file.h"

namespace fuzztest::internal {

namespace {
constexpr std::string_view kSymbolTableFileName = "symbol-table";
constexpr std::string_view kPCTableFileName = "pc-table";
constexpr std::string_view kCfTableFileName = "cf-table";
}  // namespace

void BinaryInfo::InitializeFromSanCovBinary(
    std::string_view binary_path_with_args, std::string_view objdump_path,
    std::string_view symbolizer_path, std::string_view tmp_dir_path) {
  if (binary_path_with_args.empty()) {
    // This usually happens in tests.
    LOG(INFO) << __func__ << ": binary_path_with_args is empty";
    return;
  }
  // Compute names for temp files.
  const std::filesystem::path tmp_dir = tmp_dir_path;
  CHECK(std::filesystem::exists(tmp_dir) &&
        std::filesystem::is_directory(tmp_dir));
  ScopedFile pc_table_path(tmp_dir_path, "pc_table_tmp");
  ScopedFile cf_table_path(tmp_dir_path, "cf_table_tmp");
  ScopedFile dso_table_path(tmp_dir_path, "dso_table_tmp");
  ScopedFile log_path(tmp_dir_path, "binary_info_log_tmp");
  LOG(INFO) << __func__ << ": tmp_dir: " << tmp_dir;

  Command::Options cmd_options;
  cmd_options.env_add = {absl::StrCat(
      "CENTIPEDE_RUNNER_FLAGS=:dump_binary_info:arg1=", pc_table_path.path(),
      ":arg2=", cf_table_path.path(), ":arg3=", dso_table_path.path(), ":")};
  cmd_options.stdout_file = std::string(log_path.path());
  Command cmd{binary_path_with_args, std::move(cmd_options)};
  int exit_code = cmd.Execute();
  if (exit_code != EXIT_SUCCESS) {
    LOG(INFO) << __func__ << ": exit_code: " << exit_code;
  }

  // Load PC Table.
  pc_table = ReadPcTableFromFile(pc_table_path.path());

  // Load CF Table.
  if (std::filesystem::exists(cf_table_path.path()))
    cf_table = ReadCfTable(cf_table_path.path());

  // Load the DSO Table.
  dso_table = ReadDsoTableFromFile(dso_table_path.path());

  if (pc_table.empty()) {
    CHECK(dso_table.empty());
    // Fallback to GetPcTableFromBinaryWithTracePC().
    LOG(WARNING)
        << "Failed to dump PC table directly from binary using linked-in "
           "runner; see target execution logs above; falling back to legacy PC "
           "table extraction using trace-pc and objdump";
    pc_table = GetPcTableFromBinaryWithTracePC(
        binary_path_with_args, objdump_path, pc_table_path.path());
    if (pc_table.empty()) {
      LOG(ERROR) << "Failed to extract PC table from binary using objdump; see "
                    "objdump execution logs above";
    }
    // For the legacy trace-pc instrumentation, set the dso_table
    // to 1-element array consisting of the binary name
    const std::vector<std::string> args =
        absl::StrSplit(binary_path_with_args, absl::ByAnyChar{" \t\n"},
                       absl::SkipWhitespace{});
    CHECK(!args.empty());
    dso_table.push_back({args[0], pc_table.size()});
    uses_legacy_trace_pc_instrumentation = true;
  } else {
    uses_legacy_trace_pc_instrumentation = false;
  }

  if (!uses_legacy_trace_pc_instrumentation) {
    // The number of instrumented PCs in the DSO table should match pc_table.
    size_t num_instrumened_pcs_in_all_dsos = 0;
    for (const auto& dso : dso_table) {
      num_instrumened_pcs_in_all_dsos += dso.num_instrumented_pcs;
    }
    CHECK_EQ(num_instrumened_pcs_in_all_dsos, pc_table.size());
  }

  // Load symbols, if there is a PC table.
  if (!pc_table.empty()) {
    ScopedFile sym_tmp1_path(tmp_dir_path, "symbols_tmp1");
    ScopedFile sym_tmp2_path(tmp_dir_path, "symbols_tmp2");
    symbols.GetSymbolsFromBinary(pc_table, dso_table, symbolizer_path,
                                 tmp_dir_path);
  }
}

void BinaryInfo::Read(std::string_view dir) {
  std::string symbol_table_contents;
  // TODO(b/295978603): move calculation of paths into WorkDir class.
  CHECK_OK(RemoteFileGetContents(
      (std::filesystem::path(dir) / kSymbolTableFileName).c_str(),
      symbol_table_contents));
  std::istringstream symbol_table_stream(symbol_table_contents);
  symbols.ReadFromLLVMSymbolizer(symbol_table_stream);

  std::string pc_table_contents;
  CHECK_OK(RemoteFileGetContents(
      (std::filesystem::path(dir) / kPCTableFileName).c_str(),
      pc_table_contents));
  std::istringstream pc_table_stream(pc_table_contents);
  pc_table = ReadPcTable(pc_table_stream);

  cf_table =
      ReadCfTable((std::filesystem::path(dir) / kCfTableFileName).c_str());
}

void BinaryInfo::Write(std::string_view dir) {
  std::ostringstream symbol_table_stream;
  symbols.WriteToLLVMSymbolizer(symbol_table_stream);
  // TODO(b/295978603): move calculation of paths into WorkDir class.
  CHECK_OK(RemoteFileSetContents(
      (std::filesystem::path(dir) / kSymbolTableFileName).c_str(),
      symbol_table_stream.str()));

  std::ostringstream pc_table_stream;
  WritePcTable(pc_table, pc_table_stream);
  CHECK_OK(RemoteFileSetContents(
      (std::filesystem::path(dir) / kPCTableFileName).c_str(),
      pc_table_stream.str()));

  std::ostringstream cf_table_stream;
  WriteCfTable(cf_table, cf_table_stream);
  CHECK_OK(RemoteFileSetContents(
      (std::filesystem::path(dir) / kCfTableFileName).c_str(),
      cf_table_stream.str()));
}

}  // namespace fuzztest::internal
