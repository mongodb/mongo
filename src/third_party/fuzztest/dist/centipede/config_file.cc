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

#include "./centipede/config_file.h"

#include <cstdlib>
#include <filesystem>  // NOLINT
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/base/nullability.h"
#include "absl/flags/declare.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/reflection.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/substitute.h"
#include "./centipede/config_init.h"
#include "./centipede/config_util.h"
#include "./centipede/util.h"
#include "./common/logging.h"
#include "./common/remote_file.h"

// TODO(ussuri): Move these flags next to main() ASAP. They are here
//  only temporarily to simplify the APIs and implementation in V1.

ABSL_FLAG(std::string, config, "",
          "Read flags from the specified file. The file can be either local or "
          "remote. Relative paths are referenced from the CWD. The format "
          "should be:\n"
          "--flag=value\n"
          "--another_flag=value\n"
          "...\n"
          "Lines that start with '#' or '//' are comments. Note that this "
          "format is compatible with the built-in --flagfile flag (defined by "
          "Abseil Flags library); however, unlike this flag, --flagfile "
          "supports only local files.\n"
          "Nested --load_config's won't work (but nested --flagfile's will,"
          "provided they point at a local file, e.g. $HOME/.centipede_rc).\n"
          "The flag is position-sensitive: flags read from it override (or "
          "append, in case of std::vector flags) any previous occurrences of "
          "the same flags on the command line, and vice versa.");
ABSL_FLAG(std::string, save_config, "",
          "Saves Centipede flags to the specified file and exits the program."
          "The file can be either local or remote. Relative paths are "
          "referenced from the CWD. Both the command-line flags and defaulted "
          "flags are saved (the defaulted flags are commented out). The format "
          "is:\n"
          "# --flag's help string.\n"
          "# --flag's default value.\n"
          "--flag=value\n"
          "...\n"
          "This format can be parsed back by both --config and --flagfile. "
          "Unlike those two flags, this flag is not position-sensitive and "
          "always saves the final resolved config.\n"
          "Special case: if the file's extension is .sh, a runnable shell "
          "script is saved instead.");
ABSL_FLAG(bool, update_config, false,
          "Must be used in combination with --config=<file>. Writes the final "
          "resolved config back to the same file.");
ABSL_FLAG(bool, print_config, false,
          "Print the config to stderr upon starting Centipede.");

// Declare --flagfile defined by the Abseil Flags library. The flag should point
// at a _local_ file is always automatically parsed by Abseil Flags.
ABSL_DECLARE_FLAG(std::vector<std::string>, flagfile);

#define DASHED_FLAG_NAME(name) "--" << FLAGS_##name.Name()

namespace fuzztest::internal {

AugmentedArgvWithCleanup::AugmentedArgvWithCleanup(
    const std::vector<std::string>& orig_argv,
    const Replacements& flag_replacements, const Replacements& replacements,
    BackingResourcesCleanup&& cleanup)
    : was_augmented_{false}, cleanup_{cleanup} {
  argv_.reserve(orig_argv.size());
  for (const auto& old_arg : orig_argv) {
    const auto flag_replaced_arg = [&]() -> std::optional<std::string> {
      if (old_arg.empty() || old_arg[0] != '-') return std::nullopt;
      std::string_view contents = old_arg;
      std::string_view dashes =
          (contents.size() > 1 && contents[1] == '-') ? "--" : "-";
      contents = contents.substr(dashes.size());
      for (const auto& flag_replacement : flag_replacements) {
        if (absl::StartsWith(contents, flag_replacement.first) &&
            (contents.size() == flag_replacement.first.size() ||
             contents[flag_replacement.first.size()] == '=')) {
          return absl::StrCat(dashes, flag_replacement.second,
                              contents.substr(flag_replacement.first.size()));
        }
      }
      return std::nullopt;
    }();
    const std::string& new_arg = argv_.emplace_back(
        absl::StrReplaceAll(flag_replaced_arg.value_or(old_arg), replacements));
    if (new_arg != old_arg) {
      VLOG(1) << "Augmented argv arg:\n" << VV(old_arg) << "\n" << VV(new_arg);
      was_augmented_ = true;
    }
  }
}

AugmentedArgvWithCleanup::AugmentedArgvWithCleanup(
    AugmentedArgvWithCleanup&& rhs) noexcept {
  *this = std::move(rhs);
}

AugmentedArgvWithCleanup& AugmentedArgvWithCleanup::operator=(
    AugmentedArgvWithCleanup&& rhs) noexcept {
  argv_ = std::move(rhs.argv_);
  was_augmented_ = rhs.was_augmented_;
  cleanup_ = std::move(rhs.cleanup_);
  // Prevent rhs from calling the cleanup in dtor (moving an std::function
  // leaves the moved object in a valid, but undefined, state).
  rhs.cleanup_ = {};
  return *this;
}

AugmentedArgvWithCleanup::~AugmentedArgvWithCleanup() {
  if (cleanup_) cleanup_();
}

AugmentedArgvWithCleanup LocalizeConfigFilesInArgv(
    const std::vector<std::string>& argv) {
  const std::filesystem::path path = absl::GetFlag(FLAGS_config);

  if (!path.empty()) {
    CHECK_NE(path, absl::GetFlag(FLAGS_save_config))
        << "To update config in place, use " << DASHED_FLAG_NAME(update_config);
  }

  // Always need these (--config=<path> can be passed with a local <path>).
  const AugmentedArgvWithCleanup::Replacements flag_replacements = {
      {std::string{FLAGS_config.Name()}, std::string{FLAGS_flagfile.Name()}},
  };
  AugmentedArgvWithCleanup::Replacements replacements;
  AugmentedArgvWithCleanup::BackingResourcesCleanup cleanup;

  // Copy the remote config file to a temporary local mirror.
  if (!path.empty() && !std::filesystem::exists(path)) {  // assume remote
    // Read the remote file.
    std::string contents;
    CHECK_OK(RemoteFileGetContents(path.c_str(), contents));

    // Save a temporary local copy.
    const std::filesystem::path tmp_dir = TemporaryLocalDirPath();
    const std::filesystem::path local_path = tmp_dir / path.filename();
    LOG(INFO) << "Localizing remote config: " << VV(path) << VV(local_path);
    // NOTE: Ignore "Remote" in the API names here: the paths are always local.
    CHECK_OK(RemoteMkdir(tmp_dir.c_str()));
    CHECK_OK(RemoteFileSetContents(local_path.c_str(), contents));

    // Augment the argv to point at the local copy and ensure it is cleaned up.
    replacements.emplace_back(path.c_str(), local_path.c_str());
    cleanup = [local_path]() { std::filesystem::remove(local_path); };
  }

  return AugmentedArgvWithCleanup{argv, flag_replacements, replacements,
                                  std::move(cleanup)};
}

std::filesystem::path MaybeSaveConfigToFile(
    const std::vector<std::string>& leftover_argv) {
  std::filesystem::path path;

  // Initialize `path` if --save_config or --update_config is passed.
  if (!absl::GetFlag(FLAGS_save_config).empty()) {
    path = absl::GetFlag(FLAGS_save_config);
    CHECK_NE(path, absl::GetFlag(FLAGS_config))
        << "To update config in place, use " << DASHED_FLAG_NAME(update_config);
    CHECK(!absl::GetFlag(FLAGS_update_config))
        << DASHED_FLAG_NAME(save_config) << " and "
        << DASHED_FLAG_NAME(update_config) << " are mutually exclusive";
  } else if (absl::GetFlag(FLAGS_update_config)) {
    path = absl::GetFlag(FLAGS_config);
    CHECK(!path.empty()) << DASHED_FLAG_NAME(update_config)
                         << " must be used in combination with "
                         << DASHED_FLAG_NAME(config);
  }

  // Save or update the config file.
  if (!path.empty()) {
    const std::set<std::string_view> excluded_flags = {
        FLAGS_config.Name(),
        FLAGS_save_config.Name(),
        FLAGS_update_config.Name(),
        FLAGS_print_config.Name(),
    };
    const FlagInfosPerSource flags =
        GetFlagsPerSource("centipede", excluded_flags);
    const std::string flags_str = FormatFlagfileString(
        flags, DefaultedFlags::kCommentedOut, FlagComments::kHelpAndDefault);
    std::string file_contents;
    if (path.extension() == ".sh") {
      // NOTES: 1) The first element of `leftover_argv` is expected to be the
      // /path/to/centipede, so the $1 in the stub will run it.
      // 2) absl::Substitute() replaces the escaped $$ with a $.
      constexpr std::string_view kScriptStub =
          R"(#!/bin/bash -eu

declare -ra flags=(
$0)

if [[ -n "$1" ]]; then
  wd=$1
else
  wd=$$PWD
fi
read -e -p "Clear workdir (which is '$$wd') [y/N]? " yn
# Tip: To default to 'y', change 'yY' to 'nN' below.
if [[ "$${yn}" =~ [yY] ]]; then
  rm -rf "$$wd"/corpus* "$$wd"/*report*.txt "$$wd"/*/features*
fi

set -x
$2 "$${flags[@]}"
)";
      const auto workdir = absl::GetAllFlags()["workdir"]->CurrentValue();
      const auto argv_str = absl::StrJoin(leftover_argv, " ");
      file_contents =
          absl::Substitute(kScriptStub, flags_str, workdir, argv_str);
    } else {
      file_contents = flags_str;
    }
    CHECK_OK(RemoteFileSetContents(path.c_str(), file_contents));
  }

  return path;
}

std::unique_ptr<RuntimeState> InitCentipede(  //
    int argc, char** absl_nonnull argv) {
  std::vector<std::string> leftover_argv;

  // main_runtime_init() is allowed to remove recognized flags from `argv`, so
  // we need a copy.
  const std::vector<std::string> saved_argv = CastArgv(argc, argv);

  // Among other things, this performs the initial command line parsing.
  std::unique_ptr<RuntimeState> runtime_state = InitRuntime(argc, argv);

  // If --config=<path> was passed, replace it with the Abseil Flags' built-in
  // --flagfile=<localized_path> and reparse the command line. NOTE: It would be
  // incorrect to just parse the contents of <path>, because --config (and
  // --flagfile for that matter) are position-sensitive, i.e. they may override
  // flags that come before on the command line, and vice versa.
  const AugmentedArgvWithCleanup localized_argv =
      LocalizeConfigFilesInArgv(saved_argv);
  if (localized_argv.was_augmented()) {
    LOG(INFO) << "Command line was augmented; reparsing";
    runtime_state->leftover_argv() = CastArgv(absl::ParseCommandLine(
        localized_argv.argc(), CastArgv(localized_argv.argv()).data()));
  }

  // Log the final resolved config.
  if (absl::GetFlag(FLAGS_print_config)) {
    const FlagInfosPerSource flags = GetFlagsPerSource("centipede");
    const std::string flags_str = FormatFlagfileString(
        flags, DefaultedFlags::kCommentedOut, FlagComments::kNone);
    LOG(INFO) << "Final resolved config:\n" << flags_str;
  }

  // If --save_config was passed, save the final resolved flags to the requested
  // file and exit the program.
  const auto path = MaybeSaveConfigToFile(leftover_argv);
  if (!path.empty()) {
    LOG(INFO) << "Config written to file: " << VV(path);
    LOG(INFO) << "Nothing left to do; exiting";
    exit(EXIT_SUCCESS);
  }

  return runtime_state;
}

}  // namespace fuzztest::internal
