// Copyright 2019 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "tcmalloc/segv_handler.h"

#include <sys/mman.h>
#include <sys/syscall.h>

#include "gtest/gtest.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/static_vars.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

TEST(SegvHandlerTest, SignalHandlerStackConsumption) {
  // Test the signal handler stack consumption. Since it runs on potentially
  // limited signal stack, the consumption is important. If the test fails,
  // the numbers may need to be updated. Reducing stack usage is always good,
  // increasing may indicate a problem. Avoid setting too high slack,
  // since it will prevent detection of usage changes in future.
  auto ptr = tc_globals.guardedpage_allocator().Allocate(1, 0);
  if (ptr.status != Profile::Sample::GuardedStatus::Guarded) {
    GTEST_SKIP() << "did not get a guarded allocation";
  }
  ASSERT_NE(ptr.alloc, nullptr);
  tc_globals.guardedpage_allocator().Deallocate(ptr.alloc);
  static void* addr;
  addr = ptr.alloc;
  constexpr size_t kStackSize = 1 << 20;
  void* altstack = mmap(nullptr, kStackSize, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ASSERT_NE(altstack, MAP_FAILED);
  stack_t sigstk = {};
  sigstk.ss_sp = altstack;
  sigstk.ss_size = kStackSize;
  stack_t old_sigstk;
  EXPECT_EQ(sigaltstack(&sigstk, &old_sigstk), 0);
  struct sigaction act = {};
  act.sa_flags = SA_SIGINFO | SA_ONSTACK;
  act.sa_sigaction = [](int sig, siginfo_t* info, void* ctx) {
    info->si_addr = addr;
    SegvHandler(SIGSEGV, info, ctx);
  };
  struct sigaction oldact;
  ASSERT_EQ(sigaction(SIGUSR1, &act, &oldact), 0);
  constexpr char kFillValue = 0xe1;
  memset(altstack, kFillValue, kStackSize);
  for (int i = 0; i < 10; ++i) {
    ASSERT_EQ(syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), SIGUSR1), 0);
  }
  ASSERT_EQ(sigaltstack(&old_sigstk, nullptr), 0);
  ASSERT_EQ(sigaction(SIGUSR1, &oldact, nullptr), 0);
  size_t usage = kStackSize;
  for (;
       usage && static_cast<char*>(altstack)[kStackSize - usage] == kFillValue;
       --usage) {
  }
#if defined(__x86_64__)
#if defined(NDEBUG)
  constexpr size_t kExpectedUsage = 12800;
  constexpr size_t kUsageSlack = 35;
#else
  constexpr size_t kExpectedUsage = 13500;
  constexpr size_t kUsageSlack = 45;
#endif
#elif defined(__aarch64__)
#if defined(NDEBUG)
  constexpr size_t kExpectedUsage = 12520;
  constexpr size_t kUsageSlack = 10;
#else
  constexpr size_t kExpectedUsage = 16000;
  constexpr size_t kUsageSlack = 30;
#endif
#else
  constexpr size_t kExpectedUsage = 100000;
  constexpr size_t kUsageSlack = 95;
#endif
  printf("stack usage: %zu\n", usage);
  EXPECT_GT(usage, 0);
  EXPECT_LT(usage, kExpectedUsage);
  EXPECT_GT(usage, kExpectedUsage * (100 - kUsageSlack) / 100);
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
