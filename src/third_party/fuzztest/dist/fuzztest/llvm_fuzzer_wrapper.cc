#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/base/no_destructor.h"
#include "absl/base/thread_annotations.h"
#include "absl/flags/declare.h"
#include "absl/flags/flag.h"
#include "absl/log/absl_check.h"
#include "absl/random/bit_gen_ref.h"
#include "absl/random/random.h"
#include "absl/synchronization/mutex.h"
#include "./fuzztest/fuzztest.h"
#include "./fuzztest/fuzztest_macros.h"
#include "./fuzztest/internal/domains/arbitrary_impl.h"
#include "./fuzztest/internal/domains/container_of_impl.h"
#include "./fuzztest/internal/domains/domain_base.h"
#include "./fuzztest/internal/io.h"
#include "./fuzztest/internal/logging.h"

ABSL_DECLARE_FLAG(std::string, llvm_fuzzer_wrapper_dict_file);
ABSL_DECLARE_FLAG(std::string, llvm_fuzzer_wrapper_corpus_dir);

constexpr static size_t kByteArrayMaxLen = 4096;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

extern "C" size_t __attribute__((weak))
LLVMFuzzerCustomMutator(uint8_t* data, size_t size, size_t max_size,
                        unsigned int seed);

// TODO(b/303267857): Migrate fuzz targets that use this feature manually.
// `LLVMFuzzerCustomCrossOver` is defined to produce a link-error (duplicate
// definition) if it's also defined by user.
extern "C" size_t LLVMFuzzerCustomCrossOver(const uint8_t* data1, size_t size1,
                                            const uint8_t* data2, size_t size2,
                                            uint8_t* out, size_t max_out_size,
                                            unsigned int seed) {
  std::cerr << "LLVMFuzzerCustomCrossOver is not supported in FuzzTest\n";
  exit(-1);
}

std::vector<std::vector<uint8_t>> ReadByteArraysFromDirectory() {
  const std::string flag = absl::GetFlag(FLAGS_llvm_fuzzer_wrapper_corpus_dir);
  if (flag.empty()) return {};
  std::vector<fuzztest::internal::FilePathAndData> files =
      fuzztest::internal::ReadFileOrDirectory(flag);

  std::vector<std::vector<uint8_t>> out;
  out.reserve(files.size());
  for (const fuzztest::internal::FilePathAndData& file : files) {
    out.push_back(
        {file.data.begin(),
         file.data.begin() + std::min(file.data.size(), kByteArrayMaxLen)});
  }
  return out;
}

std::vector<std::vector<uint8_t>> ReadByteArrayDictionaryFromFile() {
  const std::string flag = absl::GetFlag(FLAGS_llvm_fuzzer_wrapper_dict_file);
  if (flag.empty()) return {};
  std::vector<fuzztest::internal::FilePathAndData> files =
      fuzztest::internal::ReadFileOrDirectory(flag);

  std::vector<std::vector<uint8_t>> out;
  out.reserve(files.size());
  // Dictionary must be in the format specified at
  // https://llvm.org/docs/LibFuzzer.html#dictionaries
  for (const fuzztest::internal::FilePathAndData& file : files) {
    absl::StatusOr<std::vector<std::string>> parsed_entries =
        fuzztest::ParseDictionary(file.data);
    ABSL_CHECK(parsed_entries.status().ok())
        << "Could not parse dictionary file " << file.path << ": "
        << parsed_entries.status();
    for (const std::string& parsed_entry : *parsed_entries) {
      out.emplace_back(parsed_entry.begin(), parsed_entry.end());
    }
  }
  return out;
}

namespace {

using ::fuzztest::domain_implementor::MutationMetadata;

// Manager that controls the access of mutation metadata for
// LLVMFuzzerMutate/LLVMFuzzerCustomMutator.
//
// The metadata is supposed to be active only when FuzzTest is calling
// LLVMFuzzerCustomMutator.
//
// If the metadata is active, `Acquire` would return a non-null metadata
// pointer, and the caller should call `Release` after the metadata usage.
// If the metadata is inactive, `Acquire` would abort - this would only happen
// when the fuzzer calls `LLVMFuzzerMutate` outside of
// `LLVMFuzzerCustomMutator`.
class LLVMFuzzerMutateMetadataManager {
 public:
  void Activate(MutationMetadata mutation_metadata) {
    absl::MutexLock lock(&mu_);
    FUZZTEST_INTERNAL_CHECK(
        !mutation_metadata_.has_value(),
        "MutationMetadata is already active before calling Activate()!");
    FUZZTEST_INTERNAL_CHECK(
        acquire_count_ == 0,
        "MutationMetadata still has readers before being calling Activate()!");
    mutation_metadata_ = std::move(mutation_metadata);
  }

  void Deactivate() {
    absl::MutexLock lock(&mu_);
    FUZZTEST_INTERNAL_CHECK(
        mutation_metadata_.has_value(),
        "MutationMetadata is not active before calling Deactivate()!");
    FUZZTEST_INTERNAL_CHECK(
        acquire_count_ == 0,
        "MutationMetadata still has readers before calling Deactivate()!");
    mutation_metadata_ = std::nullopt;
  }

  const MutationMetadata& Acquire() {
    absl::MutexLock lock(&mu_);
    FUZZTEST_INTERNAL_CHECK_PRECONDITION(
        mutation_metadata_.has_value(),
        "Cannot acquire unavailable mutation metadata, likely due to the "
        "fuzzer calling LLVMFuzzerMutate() outside of "
        "LLVMFuzzerCustomMutator() invocation, which is not allowed.");
    ++acquire_count_;
    return *mutation_metadata_;
  }

  void Release() {
    absl::MutexLock lock(&mu_);
    FUZZTEST_INTERNAL_CHECK(
        mutation_metadata_.has_value(),
        "MutationMetadata is not active before calling Release()!");
    FUZZTEST_INTERNAL_CHECK(
        acquire_count_ > 0,
        "MutationMetadata has no readers before calling Release()!");
    --acquire_count_;
  }

 private:
  absl::Mutex mu_;
  size_t acquire_count_ ABSL_GUARDED_BY(mu_) = 0;
  std::optional<MutationMetadata> mutation_metadata_ ABSL_GUARDED_BY(mu_);
};

absl::NoDestructor<LLVMFuzzerMutateMetadataManager> mutation_metadata_manager;
}  // namespace

#ifdef FUZZTEST_USE_CENTIPEDE
extern "C" size_t CentipedeLLVMFuzzerMutateCallback(uint8_t* data, size_t size,
                                                    size_t max_size) {
#else   // FUZZTEST_USE_CENTIPEDE
extern "C" size_t LLVMFuzzerMutate(uint8_t* data, size_t size,
                                   size_t max_size) {
#endif  // FUZZTEST_USE_CENTIPEDE
  static auto domain = fuzztest::Arbitrary<std::vector<uint8_t>>()
                           .WithDictionary(ReadByteArrayDictionaryFromFile)
                           .WithSeeds(ReadByteArraysFromDirectory);
  domain.WithMaxSize(max_size);
  absl::BitGen bitgen;
  std::vector<uint8_t> val{data, data + size};
  const auto& metadata = mutation_metadata_manager->Acquire();
  domain.Mutate(val, bitgen, metadata, false);
  mutation_metadata_manager->Release();
  // This can be eliminated if Mutate() can operate on the original `data`.
  std::copy(val.begin(), val.end(), data);
  return val.size();
}

class ArbitraryByteVector
    : public fuzztest::internal::SequenceContainerOfImplBase<
          ArbitraryByteVector, std::vector<uint8_t>,
          fuzztest::internal::ArbitraryImpl<uint8_t>> {
  using Base = typename ArbitraryByteVector::ContainerOfImplBase;

 public:
  using typename Base::corpus_type;

  ArbitraryByteVector() { WithMaxSize(kByteArrayMaxLen); }

  void Mutate(corpus_type& val, absl::BitGenRef prng,
              const MutationMetadata& metadata, bool only_shrink) {
    if (LLVMFuzzerCustomMutator) {
      const size_t size = val.size();
      const size_t max_size = only_shrink ? size : kByteArrayMaxLen;
      val.resize(max_size);
      mutation_metadata_manager->Activate(metadata);
      val.resize(LLVMFuzzerCustomMutator(val.data(), size, max_size, prng()));
      mutation_metadata_manager->Deactivate();
    } else {
      Base::Mutate(val, prng, metadata, only_shrink);
    }
  }
};

void TestOneInput(const std::vector<uint8_t>& data) {
  LLVMFuzzerTestOneInput(data.data(), data.size());
}

FUZZ_TEST(LLVMFuzzer, TestOneInput)
    .WithDomains(ArbitraryByteVector()
                     .WithDictionary(ReadByteArrayDictionaryFromFile)
                     .WithSeeds(ReadByteArraysFromDirectory));
