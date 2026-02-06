#include <string>

#include "gtest/gtest.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "./fuzztest/init_fuzztest.h"

extern "C" int __attribute__((weak)) LLVMFuzzerInitialize(int* argc,
                                                          char*** argv);

ABSL_FLAG(std::string, llvm_fuzzer_wrapper_dict_file, "",
          "Path to dictionary file used by the wrapped legacy LLVMFuzzer "
          "target (https://llvm.org/docs/LibFuzzer.html#fuzz-target).");
ABSL_FLAG(std::string, llvm_fuzzer_wrapper_corpus_dir, "",
          "Path to seed corpus directory used by the wrapped legacy LLVMFuzzer "
          "target (https://llvm.org/docs/LibFuzzer.html#fuzz-target).");

int main(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);
  testing::InitGoogleTest(&argc, argv);
  if (LLVMFuzzerInitialize) {
    LLVMFuzzerInitialize(&argc, &argv);
  }
  fuzztest::InitFuzzTest(&argc, &argv);
  return RUN_ALL_TESTS();
}
