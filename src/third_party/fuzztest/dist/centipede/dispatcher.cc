// Copyright 2025 The Centipede Authors.
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

#include "./centipede/dispatcher.h"

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/base/nullability.h"
#include "./centipede/execution_metadata.h"
#include "./centipede/runner_request.h"
#include "./centipede/runner_result.h"
#include "./centipede/shared_memory_blob_sequence.h"
#include "./common/defs.h"

namespace fuzztest::internal {

namespace {

// Logging needs to be signal safe.

struct LogErrNo {};
struct LogLnSync {};

void DispatcherLog() {}

template <typename T, typename... Rest>
void DispatcherLog(const T& first, const Rest&... rest) {
  if constexpr (std::is_same_v<LogErrNo, T>) {
    auto saved_errno = errno;
    char err_buf[80];
    if (strerror_r(saved_errno, err_buf, sizeof(err_buf)) != 0) {
      constexpr std::string_view kFallbackMsg = "[strerror_r failed]";
      static_assert(kFallbackMsg.size() < sizeof(err_buf));
      std::memcpy(err_buf, kFallbackMsg.data(), kFallbackMsg.size());
      err_buf[kFallbackMsg.size()] = 0;
    }
    DispatcherLog(err_buf);
  } else if constexpr (std::is_same_v<LogLnSync, T>) {
    write(STDERR_FILENO, "\n", 1);
    fsync(STDERR_FILENO);
  } else {
    std::string_view sv = first;
    while (!sv.empty()) {
      const int r = write(STDERR_FILENO, sv.data(), sv.size());
      if (r <= 0) break;
      sv = sv.substr(r);
    }
  }
  DispatcherLog(rest...);
}

inline void DispatcherCheck(bool condition, std::string_view error) {
  if (!condition) {
    DispatcherLog(error, LogLnSync{});
    std::_Exit(1);
  }
}

const char* GetDispatcherFlags() {
  static auto dispatcher_flags = []() -> const char* {
    // TODO(xinhaoyuan): Rename the env name to FUZZTEST_DISPATCHER_FLAGS.
    const char* env_flags = std::getenv("CENTIPEDE_RUNNER_FLAGS");
    if (env_flags == nullptr) return nullptr;
    const char* result = strdup(env_flags);
    DispatcherCheck(result != nullptr, "Cannot copy the dispatcher flags");
    return result;
  }();
  return dispatcher_flags;
}

std::optional<std::string_view> GetDispatcherFlag(
    const char* absl_nonnull flag_header) {
  const char* dispatcher_flags = GetDispatcherFlags();
  if (dispatcher_flags == nullptr) return std::nullopt;
  // Extract "value" from ":flag=value:"
  const char* beg = std::strstr(dispatcher_flags, flag_header);
  if (!beg) return std::nullopt;
  const char* value_beg = beg + std::strlen(flag_header);
  const char* value_end = std::strstr(value_beg, ":");
  if (!value_end) return std::nullopt;
  return std::string_view{value_beg,
                          static_cast<size_t>(value_end - value_beg)};
}

bool HasDispatcherSwitchFlag(const char* absl_nonnull switch_flag) {
  const char* dispatcher_flags = GetDispatcherFlags();
  if (dispatcher_flags == nullptr) return false;
  return std::strstr(dispatcher_flags, switch_flag) != nullptr;
}

enum class DispatcherAction {
  kGetBinaryId,
  kListTests,
  kTestGetSeeds,
  kTestMutate,
  kTestExecute,
};

constexpr char kDispatcherBinaryIdOutputFlagHeader[] = ":binary_id_output=";
constexpr char kDispatcherTestNameFlagHeader[] = ":test=";
constexpr char kDispatcherTestListingPrefixFlagHeader[] =
    ":test_listing_prefix=";
constexpr char kDispatcherTestGetSeedsOutputDirFlagHeader[] =
    ":arg1=";  // TODO: Use better flag names when standardizing the protocol.
constexpr char kDispatcherFailureDescriptionPathFlagHeader[] =
    ":failure_description_path=";
constexpr char kDispatcherFailureSignaturePathFlagHeader[] =
    ":failure_signature_path=";
constexpr char kDispatcherInputsBlobSequencePathFlagHeader[] =
    ":arg1=";  // TODO: Use better flag names when standardizing the protocol.
constexpr char kDispatcherOutputsBlobSequencePathFlagHeader[] =
    ":arg2=";  // TODO: Use better flag names when standardizing the protocol.

BlobSequence* GetInputsBlobSequence() {
  static auto result = []() -> BlobSequence* {
    if (std::strstr(GetDispatcherFlags(), ":shmem:") == nullptr) {
      return nullptr;
    }
    auto input_path =
        GetDispatcherFlag(kDispatcherInputsBlobSequencePathFlagHeader);
    DispatcherCheck(input_path.has_value(), "inputs blob sequence is missing");
    return new SharedMemoryBlobSequence(std::string(*input_path).c_str());
  }();
  return result;
}

BlobSequence* GetOutputsBlobSequence() {
  static auto result = []() -> BlobSequence* {
    if (std::strstr(GetDispatcherFlags(), ":shmem:") == nullptr) {
      return nullptr;
    }
    auto output_path =
        GetDispatcherFlag(kDispatcherOutputsBlobSequencePathFlagHeader);
    DispatcherCheck(output_path.has_value(),
                    "outputs blob sequence is missing");
    return new SharedMemoryBlobSequence(std::string(*output_path).c_str());
  }();
  return result;
}

DispatcherAction GetDispatcherAction() {
  static DispatcherAction dispatcher_action = [] {
    if (HasDispatcherSwitchFlag(":dump_binary_id:")) {
      return DispatcherAction::kGetBinaryId;
    }
    if (HasDispatcherSwitchFlag(":list_tests:")) {
      return DispatcherAction::kListTests;
    }
    if (HasDispatcherSwitchFlag(":dump_seed_inputs:")) {
      return DispatcherAction::kTestGetSeeds;
    }
    auto* inputs_blobseq = GetInputsBlobSequence();
    DispatcherCheck(inputs_blobseq != nullptr,
                    "input blob sequence is not found");
    auto request_type_blob = inputs_blobseq->Read();
    if (IsMutationRequest(request_type_blob)) {
      inputs_blobseq->Reset();
      return DispatcherAction::kTestMutate;
    }
    if (IsExecutionRequest(request_type_blob)) {
      inputs_blobseq->Reset();
      return DispatcherAction::kTestExecute;
    }
    DispatcherCheck(false, "unknown dispatcher action from the flags");
    // should not reach here.
    std::abort();
  }();
  return dispatcher_action;
}

template <typename... C>
void TrySetFileContents(const char* absl_nonnull path, C... contents) {
  // Needs to be signal-safe.
  int f = open(path, O_CREAT | O_TRUNC | O_WRONLY, /*mode=*/0660);
  if (f == -1) {
    DispatcherLog("cannot open path ", path, ": ", LogErrNo{}, LogLnSync{});
    return;
  }
  ([&] {
    std::string_view sv = contents;
    while (!sv.empty()) {
      const int r = write(f, sv.data(), sv.size());
      if (r < 0) {
        DispatcherLog("write() failed on ", path, ": ", LogErrNo{},
                      LogLnSync{});
        return false;
      }
      if (r == 0) {
        DispatcherLog("write() on ", path,
                      " returns 0 unexpectedly. Stopping writing the file.");
        return false;
      }
      sv = sv.substr(r);
    }
    return true;
  }() &&
   ...);  // NOLINT - stop fighting with auto-fomatting.
  if (fsync(f) != 0) {
    DispatcherLog("fsync() failed on ", path, ": ", LogErrNo{}, LogLnSync{});
  }
  if (close(f) != 0) {
    DispatcherLog("close() failed on ", path, ": ", LogErrNo{}, LogLnSync{});
  }
}

static std::atomic<bool> in_test_callback = false;

class TestCallbackGuard {
 public:
  TestCallbackGuard() {
    DispatcherCheck(!in_test_callback.exchange(true),
                    "test callback is already activated");
  }

  ~TestCallbackGuard() { in_test_callback = false; }
};

void DispatcherDoGetBinaryId(const FuzzTestDispatcherCallbacks& callbacks) {
  const auto binary_id_output_path =
      GetDispatcherFlag(kDispatcherBinaryIdOutputFlagHeader);
  DispatcherCheck(binary_id_output_path.has_value(),
                  "binary ID output path is not set");
  std::string binary_id;
  {
    TestCallbackGuard guard;
    binary_id = callbacks.get_binary_id ? callbacks.get_binary_id() : "";
  }
  TrySetFileContents(std::string{*binary_id_output_path}.c_str(), binary_id);
}

void DispatcherDoListTests(const FuzzTestDispatcherCallbacks& callbacks) {
  DispatcherCheck(callbacks.list_tests != nullptr,
                  "list_tests callback must be set");
  TestCallbackGuard guard;
  callbacks.list_tests();
}

void DispatcherDoGetSeeds(const FuzzTestDispatcherCallbacks& callbacks) {
  if (callbacks.get_seeds == nullptr) {
    return;
  }
  TestCallbackGuard guard;
  callbacks.get_seeds();
}

int DispatcherDoMutate(const FuzzTestDispatcherCallbacks& callbacks) {
  auto* inputs_blobseq = GetInputsBlobSequence();
  auto* outputs_blobseq = GetOutputsBlobSequence();
  DispatcherCheck(inputs_blobseq != nullptr && outputs_blobseq != nullptr,
                  "inputs/outputs blob sequences must be specified");

  bool has_mutate = callbacks.mutate != nullptr;
  if (!MutationResult::WriteHasCustomMutator(has_mutate, *outputs_blobseq)) {
    std::fprintf(stderr, "Failed to write custom mutator indicator!\n");
    return EXIT_FAILURE;
  }
  if (!has_mutate) {
    return EXIT_SUCCESS;
  }

  // Read max_num_mutants.
  size_t num_mutants = 0;
  size_t num_inputs = 0;
  if (!IsMutationRequest(inputs_blobseq->Read())) {
    std::fprintf(stderr, "Not mutation request!\n");
    return EXIT_FAILURE;
  }
  if (!IsNumMutants(inputs_blobseq->Read(), num_mutants)) {
    std::fprintf(stderr, "No num mutants\n");
    return EXIT_FAILURE;
  }
  if (!IsNumInputs(inputs_blobseq->Read(), num_inputs)) {
    std::fprintf(stderr, "No num inputs\n");
    return EXIT_FAILURE;
  }

  struct OwningMutateInput {
    ByteArray data;
    ExecutionMetadata metadata;
  };
  // Note: unclear if we can continue using std::vector (or other STL)
  // in the runner. But for now use std::vector.
  //
  // Collect the inputs into a vector. We copy them instead of using pointers
  // into shared memory so that the user code doesn't touch the shared memory.
  std::vector<OwningMutateInput> owning_inputs;
  owning_inputs.reserve(num_inputs);
  std::vector<FuzzTestDispatcherInputForMutate> inputs;
  inputs.reserve(num_inputs);
  for (size_t i = 0; i < num_inputs; ++i) {
    // If inputs_blobseq have overflown in the engine, we still want to
    // handle the first few inputs.
    ExecutionMetadata metadata;
    if (!IsExecutionMetadata(inputs_blobseq->Read(), metadata)) {
      break;
    }
    auto blob = inputs_blobseq->Read();
    if (!IsDataInput(blob)) break;
    owning_inputs.push_back(
        OwningMutateInput{/*data=*/ByteArray{blob.data, blob.data + blob.size},
                          /*metadata=*/std::move(metadata)});
    inputs.push_back(FuzzTestDispatcherInputForMutate{
        /*input=*/owning_inputs.back().data.data(),
        /*input_size=*/owning_inputs.back().data.size(),
        /*metadata=*/owning_inputs.back().metadata.cmp_data.data(),
        /*metadata_size=*/owning_inputs.back().metadata.cmp_data.size()});
  }

  {
    TestCallbackGuard guard;
    fprintf(stderr, "calling custom mutator\n");
    // We ensure that:
    //  * `inputs` is a valid pointer to an array of
    //    `FuzzTestDispatcherInputForMutate` objects with length `num_inputs`.
    //  * Each object of the array contains a valid `input` pointer to
    //    `input_size` bytes, and a valid `metadata` pointer to `metadata_size`
    //    bytes.
    callbacks.mutate(inputs.data(), inputs.size(), num_mutants,
                     /*shrink=*/0);
  }
  return EXIT_SUCCESS;
}

int DispatcherDoExecute(const FuzzTestDispatcherCallbacks& callbacks) {
  DispatcherCheck(callbacks.execute != nullptr, "execute callback must be set");
  auto* inputs_blobseq = GetInputsBlobSequence();
  auto* outputs_blobseq = GetOutputsBlobSequence();
  DispatcherCheck(inputs_blobseq != nullptr && outputs_blobseq != nullptr,
                  "inputs/ouptuts blob sequence must exist");

  size_t num_inputs = 0;
  DispatcherCheck(IsExecutionRequest(inputs_blobseq->Read()),
                  "not an execution request");
  DispatcherCheck(IsNumInputs(inputs_blobseq->Read(), num_inputs),
                  "failed to read num_inputs");

  for (size_t i = 0; i < num_inputs; i++) {
    auto blob = inputs_blobseq->Read();
    if (!blob.IsValid()) return EXIT_SUCCESS;  // no more blobs to read.
    if (!IsDataInput(blob)) return EXIT_FAILURE;

    // Copy from blob to data so that to not pass the shared memory further.
    ByteArray data(blob.data, blob.data + blob.size);

    if (!BatchResult::WriteInputBegin(*outputs_blobseq)) {
      // TODO: This is to follow the previous behavior, but should we abort
      // here?
      break;
    }
    {
      TestCallbackGuard guard;
      // We ensure that `input` is a valid pointer to an array of `size` bytes.
      callbacks.execute(data.data(), data.size());
    }
    if (!BatchResult::WriteInputEnd(*outputs_blobseq)) {
      // TODO: This is to follow the previous behavior, but should we abort
      // here?
      break;
    }
  }

  return EXIT_SUCCESS;
}

void DispatcherEmitFailure(const char* absl_nonnull prefix,
                           const char* absl_nonnull description,
                           const char* signature, size_t signature_size) {
  bool success = false;
  [[maybe_unused]] static bool write_once = [=, &success] {
    if (const auto failure_description_path =
            GetDispatcherFlag(kDispatcherFailureDescriptionPathFlagHeader);
        failure_description_path.has_value()) {
      TrySetFileContents(std::string{*failure_description_path}.c_str(), prefix,
                         description);
    }
    if (const auto failure_signature_path =
            GetDispatcherFlag(kDispatcherFailureSignaturePathFlagHeader);
        failure_signature_path.has_value()) {
      TrySetFileContents(std::string{*failure_signature_path}.c_str(),
                         std::string_view{signature, signature_size});
    }
    success = true;
    return true;
  }();
  if (!success) {
    DispatcherLog("Failed to emit failure ", prefix, description, LogLnSync{});
  }
}

}  // namespace

}  // namespace fuzztest::internal

using fuzztest::internal::BatchResult;
using fuzztest::internal::DispatcherAction;
using fuzztest::internal::DispatcherCheck;
using fuzztest::internal::DispatcherDoExecute;
using fuzztest::internal::DispatcherDoGetBinaryId;
using fuzztest::internal::DispatcherDoGetSeeds;
using fuzztest::internal::DispatcherDoListTests;
using fuzztest::internal::DispatcherDoMutate;
using fuzztest::internal::DispatcherEmitFailure;
using fuzztest::internal::GetDispatcherAction;
using fuzztest::internal::GetDispatcherFlag;
using fuzztest::internal::GetDispatcherFlags;
using fuzztest::internal::GetOutputsBlobSequence;
using fuzztest::internal::HasDispatcherSwitchFlag;
using fuzztest::internal::in_test_callback;
using fuzztest::internal::kDispatcherTestGetSeedsOutputDirFlagHeader;
using fuzztest::internal::kDispatcherTestListingPrefixFlagHeader;
using fuzztest::internal::kDispatcherTestNameFlagHeader;
using fuzztest::internal::MutationResult;

int FuzzTestDispatcherIsEnabled() {
  const char* flags = GetDispatcherFlags();
  if (flags == nullptr) return 0;
  fprintf(stderr, "Dispatcher is enabled with flags: %s\n", flags);
  return 1;
}

const char* FuzzTestDispatcherGetTestName() {
  static auto test_name = []() -> const char* {
    const auto test_name = GetDispatcherFlag(kDispatcherTestNameFlagHeader);
    if (!test_name.has_value()) return nullptr;
    return strndup(test_name->data(), test_name->size());
  }();
  return test_name;
}

int FuzzTestDispatcherRun(const FuzzTestDispatcherCallbacks* callbacks) {
  DispatcherCheck(callbacks != nullptr, "callbacks must be set");
  if (HasDispatcherSwitchFlag(":dump_configuration:")) {
    return 0;
  }
  switch (GetDispatcherAction()) {
    case DispatcherAction::kGetBinaryId:
      DispatcherDoGetBinaryId(*callbacks);
      break;
    case DispatcherAction::kListTests:
      DispatcherDoListTests(*callbacks);
      break;
    case DispatcherAction::kTestGetSeeds:
      DispatcherDoGetSeeds(*callbacks);
      break;
    case DispatcherAction::kTestMutate:
      DispatcherDoMutate(*callbacks);
      break;
    case DispatcherAction::kTestExecute:
      DispatcherDoExecute(*callbacks);
      break;
    default:
      DispatcherCheck(false, "unknown dispatcher action to take");
  }
  return 0;
}

void FuzzTestDispatcherEmitTestName(const char* name) {
  DispatcherCheck(
      GetDispatcherAction() == DispatcherAction::kListTests && in_test_callback,
      "must be called inside test callback for listing tests");
  static auto test_listing_prefix =
      GetDispatcherFlag(kDispatcherTestListingPrefixFlagHeader);
  DispatcherCheck(test_listing_prefix.has_value(),
                  "test listing path prefix must be set");
  DispatcherCheck(name != nullptr, "test name must be set");
  auto test_output_path = std::string{*test_listing_prefix};
  test_output_path += name;
  FILE* f = std::fopen(test_output_path.c_str(), "w");
  if (f == nullptr) {
    std::perror("FAILURE: fopen()");
  }
  std::fclose(f);
}

void FuzzTestDispatcherEmitSeed(const void* data, size_t size) {
  DispatcherCheck(GetDispatcherAction() == DispatcherAction::kTestGetSeeds &&
                      in_test_callback,
                  "must be called inside test callback for getting seeds");
  DispatcherCheck(size > 0 && data != nullptr,
                  "seed must be non-empty with a valid pointer");
  static size_t seed_index = 0;
  static const char* output_dir = [] {
    const auto flag_value =
        GetDispatcherFlag(kDispatcherTestGetSeedsOutputDirFlagHeader);
    DispatcherCheck(flag_value.has_value(),
                    "seeds output path must be specified");
    const char* result = strndup(flag_value->data(), flag_value->size());
    DispatcherCheck(result != nullptr, "failed to copy the seeds output path");
    return result;
  }();
  // Cap seed index within 9 digits. If this was triggered, the dumping would
  // take forever..
  if (seed_index >= 1000000000) return;
  char seed_path_buf[PATH_MAX];
  const size_t num_path_chars =
      snprintf(seed_path_buf, PATH_MAX, "%s/%09lu", output_dir, seed_index);
  DispatcherCheck(num_path_chars < PATH_MAX, "seed path reaches PATH_MAX");
  FILE* output_file = fopen(seed_path_buf, "w");
  const size_t num_bytes_written = fwrite(data, 1, size, output_file);
  DispatcherCheck(num_bytes_written == size,
                  "wrong number of bytes written for seed");
  fclose(output_file);
  ++seed_index;
}

void FuzzTestDispatcherEmitMutant(const void* data, size_t size) {
  DispatcherCheck(GetDispatcherAction() == DispatcherAction::kTestMutate &&
                      in_test_callback,
                  "must be called inside test callback for mutating");
  DispatcherCheck(size > 0 && data != nullptr,
                  "mutant must be non-empty with a valid pointer");
  auto* output = GetOutputsBlobSequence();
  DispatcherCheck(output != nullptr, "outputs blob sequence must exist");
  DispatcherCheck(MutationResult::WriteMutant(
                      {static_cast<const uint8_t*>(data), size}, *output),
                  "failed to write mutant");
}

void FuzzTestDispatcherEmitFeedbackAs32BitFeatures(const uint32_t* features,
                                                   size_t num_features) {
  DispatcherCheck(GetDispatcherAction() == DispatcherAction::kTestExecute &&
                      in_test_callback,
                  "must be called inside test callback of executing");
  DispatcherCheck(num_features > 0 && features != nullptr,
                  "feature array must be non-empty with a valid pointer");
  auto* output = GetOutputsBlobSequence();
  DispatcherCheck(output != nullptr, "outputs blob sequence must exist");
  DispatcherCheck(BatchResult::WriteDispatcher32BitFeatures(
                      features, num_features, *output),
                  "failed to write feedback");
}

void FuzzTestDispatcherEmitExecutionMetadata(const void* metadata,
                                             size_t size) {
  DispatcherCheck(GetDispatcherAction() == DispatcherAction::kTestExecute &&
                      in_test_callback,
                  "must be called inside test callback of executing");
  DispatcherCheck(size > 0 && metadata != nullptr,
                  "metadata must be non-empty with a valid pointer");
  auto* output = GetOutputsBlobSequence();
  DispatcherCheck(output != nullptr, "outputs blob sequence must exist");
  DispatcherCheck(BatchResult::WriteMetadata(
                      {static_cast<const uint8_t*>(metadata), size}, *output),
                  "failed to write metadata");
}

void FuzzTestDispatcherEmitInputFailure(const char* description,
                                        const void* signature,
                                        size_t signature_size) {
  DispatcherCheck(GetDispatcherAction() == DispatcherAction::kTestExecute &&
                      in_test_callback,
                  "must be called inside test callback for executing");
  DispatcherCheck((signature == nullptr) == (signature_size == 0),
                  "violated invariant: signature should be nullptr if and only "
                  "if signature_size is 0");
  DispatcherEmitFailure(
      "INPUT FAILURE: ", description != nullptr ? description : "",
      reinterpret_cast<const char*>(signature), signature_size);
}

void FuzzTestDispatcherEmitIgnoredFailure(const char* description) {
  DispatcherEmitFailure(
      "IGNORED FAILURE: ", description != nullptr ? description : "",
      /*signature=*/nullptr, /*signature_size=*/0);
}

void FuzzTestDispatcherEmitSetupFailure(const char* description) {
  DispatcherEmitFailure(
      "SETUP FAILURE: ", description != nullptr ? description : "",
      /*signature=*/nullptr, /*signature_size=*/0);
}

void FuzzTestDispatcherEmitSkippedTestFailure(const char* description) {
  DispatcherEmitFailure(
      "SKIPPED TEST: ", description != nullptr ? description : "",
      /*signature=*/nullptr, /*signature_size=*/0);
}
