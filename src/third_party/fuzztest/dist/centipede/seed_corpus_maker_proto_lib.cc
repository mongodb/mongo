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
#include "./centipede/seed_corpus_maker_proto_lib.h"

#include <filesystem>  // NOLINT
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "./centipede/seed_corpus_config.pb.h"
#include "./centipede/seed_corpus_maker_lib.h"
#include "./centipede/workdir.h"
#include "./common/logging.h"
#include "./common/remote_file.h"
#include "./common/status_macros.h"
#include "google/protobuf/text_format.h"

namespace fuzztest::internal {
namespace {

namespace fs = std::filesystem;

absl::StatusOr<proto::SeedCorpusConfig> ResolveSeedCorpusConfigProto(  //
    std::string_view config_spec,                                      //
    std::string_view override_out_dir) {
  std::string config_str;
  std::string base_dir;

  if (config_spec.empty()) {
    return absl::InvalidArgumentError(
        "Unable to ResolveSeedCorpusConfig() with empty config_spec");
  }

  if (RemotePathExists(config_spec)) {
    LOG(INFO) << "Config spec points at an existing file; trying to parse "
                 "textproto config from it: "
              << VV(config_spec);
    RETURN_IF_NOT_OK(RemoteFileGetContents(config_spec, config_str));
    LOG(INFO) << "Raw config read from file:\n" << config_str;
    base_dir = std::filesystem::path{config_spec}.parent_path();
  } else {
    LOG(INFO) << "Config spec is not a file, or file doesn't exist; trying to "
                 "parse textproto config verbatim: "
              << VV(config_spec);
    config_str = config_spec;
    base_dir = fs::current_path();
  }

  proto::SeedCorpusConfig config;
  if (!google::protobuf::TextFormat::ParseFromString(config_str, &config)) {
    return absl::InvalidArgumentError(
        absl::StrCat("Unable to parse config_str: ", config_str));
  }
  if (config.sources_size() > 0 != config.has_destination()) {
    return absl::InvalidArgumentError(
        absl::StrCat("Non-empty config must have both source(s) and "
                     "destination, config_spec: ",
                     config_spec, ", config: ", config));
  }
  LOG(INFO) << "Parsed config:\n" << config;

  // Resolve relative `source.dir_glob`s in the config to absolute ones.
  for (auto& src : *config.mutable_sources()) {
    auto* dir = src.mutable_dir_glob();
    if (dir->empty() || !fs::path{*dir}.is_absolute()) {
      *dir = fs::path{base_dir} / *dir;
    }
  }

  // Set `destination.dir_path` to `override_out_dir`, if the latter is
  // non-empty, or resolve a relative `destination.dir_path` to an absolute one.
  if (config.has_destination()) {
    auto* dir = config.mutable_destination()->mutable_dir_path();
    if (!override_out_dir.empty()) {
      *dir = override_out_dir;
    } else if (dir->empty() || !fs::path{*dir}.is_absolute()) {
      *dir = fs::path{base_dir} / *dir;
    }
  }

  if (config.destination().shard_index_digits() == 0) {
    config.mutable_destination()->set_shard_index_digits(
        WorkDir::kDigitsInShardIndex);
  }

  LOG(INFO) << "Resolved config:\n" << config;

  return config;
}

SeedCorpusConfig CreateSeedCorpusConfigFromProto(
    const proto::SeedCorpusConfig& config_proto) {
  SeedCorpusConfig config;
  for (const auto& source_proto : config_proto.sources()) {
    SeedCorpusSource source;
    source.dir_glob = source_proto.dir_glob();
    source.num_recent_dirs = source_proto.num_recent_dirs();
    source.shard_rel_glob = source_proto.shard_rel_glob();
    switch (source_proto.sample_size_case()) {
      case proto::SeedCorpusSource::kSampledFraction:
        source.sampled_fraction_or_count = source_proto.sampled_fraction();
        break;
      case proto::SeedCorpusSource::kSampledCount:
        source.sampled_fraction_or_count = source_proto.sampled_count();
        break;
      case proto::SeedCorpusSource::SAMPLE_SIZE_NOT_SET:
        break;
    }
    config.sources.push_back(std::move(source));
  }
  config.destination.dir_path = config_proto.destination().dir_path();
  config.destination.shard_rel_glob =
      config_proto.destination().shard_rel_glob();
  config.destination.shard_index_digits =
      config_proto.destination().shard_index_digits();
  config.destination.num_shards = config_proto.destination().num_shards();
  return config;
}

absl::Status DumpConfigProtoToDebugDir(const proto::SeedCorpusConfig& config,
                                       std::string_view coverage_binary_name,
                                       std::string_view coverage_binary_hash) {
  if (!RemotePathExists(config.destination().dir_path())) {
    RETURN_IF_NOT_OK(RemoteMkdir(config.destination().dir_path()));
  }
  const WorkDir workdir{
      config.destination().dir_path(),
      coverage_binary_name,
      coverage_binary_hash,
      /*my_shard_index=*/0,
  };
  const std::filesystem::path debug_info_dir = workdir.DebugInfoDirPath();
  RETURN_IF_NOT_OK(RemoteMkdir(debug_info_dir.c_str()));
  RETURN_IF_NOT_OK(RemoteFileSetContents(
      (debug_info_dir / "seeding.cfg").c_str(), absl::StrCat(config)));
  return absl::OkStatus();
}

}  // namespace

absl::Status GenerateSeedCorpusFromConfigProto(  //
    std::string_view config_spec,                //
    std::string_view coverage_binary_name,       //
    std::string_view coverage_binary_hash,       //
    std::string_view override_out_dir) {
  // Resolve the config.
  ASSIGN_OR_RETURN_IF_NOT_OK(
      const proto::SeedCorpusConfig config_proto,
      ResolveSeedCorpusConfigProto(config_spec, override_out_dir));
  if (config_proto.sources_size() == 0 || !config_proto.has_destination()) {
    LOG(WARNING) << "Config is empty: skipping seed corpus generation";
    return absl::OkStatus();
  }
  RETURN_IF_NOT_OK(DumpConfigProtoToDebugDir(config_proto, coverage_binary_name,
                                             coverage_binary_hash));
  RETURN_IF_NOT_OK(GenerateSeedCorpusFromConfig(  //
      CreateSeedCorpusConfigFromProto(config_proto), coverage_binary_name,
      coverage_binary_hash));
  return absl::OkStatus();
}

}  // namespace fuzztest::internal
