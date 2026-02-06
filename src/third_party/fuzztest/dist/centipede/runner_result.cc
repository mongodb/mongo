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

#include "./centipede/runner_result.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

#include "./centipede/execution_metadata.h"
#include "./centipede/feature.h"
#include "./centipede/shared_memory_blob_sequence.h"
#include "./common/defs.h"

namespace fuzztest::internal {

namespace {

// Tags used for both the execution and mutation results. We use the same enum
// to make the sets of tags disjoint.
enum Tags : Blob::SizeAndTagT {
  kTagInvalid,  // 0 is an invalid tag.

  // Execution result tags.
  kTagFeatures,
  kTagDispatcher32BitFeatures,
  kTagInputBegin,
  kTagInputEnd,
  kTagStats,
  kTagMetadata,

  // Mutation result tags.
  kTagHasCustomMutator,
  kTagMutant,
};

}  // namespace

bool BatchResult::WriteOneFeatureVec(const feature_t *vec, size_t size,
                                     BlobSequence &blobseq) {
  return blobseq.Write({kTagFeatures, size * sizeof(vec[0]),
                        reinterpret_cast<const uint8_t *>(vec)});
}

bool BatchResult::WriteDispatcher32BitFeatures(const uint32_t *features,
                                               size_t num_features,
                                               BlobSequence &blobseq) {
  return blobseq.Write({kTagDispatcher32BitFeatures,
                        num_features * sizeof(features[0]),
                        reinterpret_cast<const uint8_t *>(features)});
}

bool BatchResult::WriteInputBegin(BlobSequence &blobseq) {
  return blobseq.Write({kTagInputBegin, 0, nullptr});
}

bool BatchResult::WriteInputEnd(BlobSequence &blobseq) {
  return blobseq.Write({kTagInputEnd, 0, nullptr});
}

bool BatchResult::WriteStats(const ExecutionResult::Stats &stats,
                             BlobSequence &blobseq) {
  return blobseq.Write(
      {kTagStats, sizeof(stats), reinterpret_cast<const uint8_t *>(&stats)});
}

bool BatchResult::WriteMetadata(const ExecutionMetadata &metadata,
                                BlobSequence &blobseq) {
  return metadata.Write(kTagMetadata, blobseq);
}

bool BatchResult::WriteMetadata(ByteSpan bytes, BlobSequence &blobseq) {
  return blobseq.Write({kTagMetadata, bytes.size(), bytes.data()});
}

// The sequence we expect to receive is
// InputBegin, Features, Stats, InputEnd, InputBegin, ...
// with a total of results().size() tuples (InputBegin ... InputEnd).
// Blobs between InputBegin/InputEnd may go in any order.
// If the execution failed on some input, we will see InputBegin,
// but will not see all or some other blobs.
bool BatchResult::Read(BlobSequence &blobseq) {
  size_t num_begins = 0;
  size_t num_ends = 0;
  const size_t num_expected_tuples = results().size();
  ExecutionResult *current_execution_result = nullptr;
  while (true) {
    auto blob = blobseq.Read();
    if (!blob.IsValid()) break;
    if (blob.tag == kTagInputBegin) {
      if (num_begins != num_ends) return false;
      ++num_begins;
      if (num_begins > num_expected_tuples) return false;
      current_execution_result = &results()[num_ends];
      current_execution_result->clear();
      continue;
    }
    if (blob.tag == kTagInputEnd) {
      ++num_ends;
      if (num_ends != num_begins) return false;
      current_execution_result = nullptr;
      continue;
    }
    if (blob.tag == kTagMetadata) {
      if (current_execution_result == nullptr) return false;
      current_execution_result->metadata().Read(blob);
      continue;
    }
    if (blob.tag == kTagStats) {
      if (current_execution_result == nullptr) return false;
      if (blob.size != sizeof(ExecutionResult::Stats)) return false;
      memcpy(&current_execution_result->stats(), blob.data, blob.size);
      continue;
    }
    if (blob.tag == kTagFeatures) {
      if (current_execution_result == nullptr) return false;
      const size_t features_size = blob.size / sizeof(feature_t);
      FeatureVec &features = current_execution_result->mutable_features();
      features.resize(features_size);
      std::memcpy(features.data(), blob.data,
                  features_size * sizeof(feature_t));
    }
    if (blob.tag == kTagDispatcher32BitFeatures) {
      if (current_execution_result == nullptr) return false;
      const size_t size = blob.size / sizeof(uint32_t);
      std::vector<uint32_t> copied_features;
      copied_features.resize(size);
      std::memcpy(copied_features.data(), blob.data, size * sizeof(uint32_t));
      auto &features = current_execution_result->mutable_features();
      features.reserve(features.size() + size);
      for (uint32_t feature : copied_features) {
        features.push_back((feature & 0x7fffffff) +
                           feature_domains::kUserDomains[0].begin());
      }
    }
  }
  num_outputs_read_ = num_ends;
  return true;
}

bool BatchResult::IsIgnoredFailure() const {
  constexpr std::string_view kIgnoredFailurePrefix = "IGNORED FAILURE:";
  return exit_code_ != EXIT_SUCCESS &&
         std::string_view(failure_description_)
                 .substr(0, kIgnoredFailurePrefix.size()) ==
             kIgnoredFailurePrefix;
}

bool BatchResult::IsSetupFailure() const {
  constexpr std::string_view kSetupFailurePrefix = "SETUP FAILURE:";
  return exit_code_ != EXIT_SUCCESS &&
         std::string_view(failure_description_)
                 .substr(0, kSetupFailurePrefix.size()) == kSetupFailurePrefix;
}

bool BatchResult::IsSkippedTest() const {
  constexpr std::string_view kSkippedTestPrefix = "SKIPPED TEST:";
  return exit_code_ != EXIT_SUCCESS &&
         std::string_view(failure_description_)
                 .substr(0, kSkippedTestPrefix.size()) == kSkippedTestPrefix;
}

bool MutationResult::WriteHasCustomMutator(bool has_custom_mutator,
                                           BlobSequence &blobseq) {
  return blobseq.Write(
      {kTagHasCustomMutator, sizeof(has_custom_mutator),
       reinterpret_cast<const uint8_t *>(&has_custom_mutator)});
}

bool MutationResult::WriteMutant(ByteSpan mutant, BlobSequence &blobseq) {
  return blobseq.Write({kTagMutant, mutant.size(), mutant.data()});
}

bool MutationResult::Read(size_t num_mutants, BlobSequence &blobseq) {
  const Blob blob = blobseq.Read();
  if (blob.tag != kTagHasCustomMutator) return false;
  if (blob.size != sizeof(has_custom_mutator_)) return false;
  std::memcpy(&has_custom_mutator_, blob.data, blob.size);
  if (!has_custom_mutator_) return true;

  mutants_.clear();
  mutants_.reserve(num_mutants);
  for (size_t i = 0; i < num_mutants; ++i) {
    const Blob blob = blobseq.Read();
    if (blob.tag != kTagMutant) return false;
    if (blob.size == 0) break;
    mutants_.emplace_back(blob.data, blob.data + blob.size);
  }
  return true;
}

}  // namespace fuzztest::internal
