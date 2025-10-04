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

#include <stddef.h>
#include <stdlib.h>

#include "gtest/gtest.h"
#include "tcmalloc/internal/linked_list.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/testing/testutil.h"

// This tests that heap profiling works properly in the face of allocations
// being rounded up to the next size class.
//
// We're checking for the following bug (or something similar):
//
// Suppose client code calls malloc(17) many times.  tcmalloc will round these
// allocations up to the next size class, which happens to be 32 bytes.
//
// As part of processing profiles, we reverse the effect of sampling to get an
// approximation of the actual total usage; if we do this reversal based on
// allocated size, but the sampling was actually done on requested size (as
// happens in practice), we will under-count these allocation.

namespace tcmalloc {
namespace {

// Return number of bytes of live data of size s according to the heap profile.
double HeapProfileReport(size_t s) {
  double result = 0;

  auto profile = MallocExtension::SnapshotCurrent(ProfileType::kHeap);
  profile.Iterate([&](const Profile::Sample& e) {
    if (e.allocated_size == s) {
      result += e.sum;
    }
  });

  return result;
}

TEST(SampleSizeClassTest, Main) {

  // We choose a small tcmalloc sampling parameter because this reduces the
  // random variance in this test's result.
  MallocExtension::SetProfileSamplingRate(1024);
  // Disable GWP-ASan since it doesn't use size classes.
  MallocExtension::SetGuardedSamplingRate(-1);

  // Make a huge allocation that's very likely to be sampled to clear
  // out the current sample point; ensures all our allocations are
  // actually sampled at the above rate.
  ::operator delete(::operator new(1024 * 1024 * 1024));

  // Pick kRequestSize so that it changes significantly when it is
  // rounded up by tcmalloc.  If this changes, you may want to pick a
  // new kRequestSize.
  const size_t kRequestSize = 17;
  const size_t kActualSize = 32;
  {
    ScopedNeverSample never_sample;  // sampling can affect reported sizes
    void* p = malloc(kRequestSize);
    EXPECT_EQ(kActualSize, MallocExtension::GetAllocatedSize(p));
    free(p);
  }

  // Allocate a large amount of data.  We construct a linked list with the
  // pointers to avoid having to allocate auxiliary data for keeping track of
  // all of the allocations.
  const double start = HeapProfileReport(kActualSize);
  size_t allocated = 0;
  tcmalloc_internal::LinkedList objs;
  while (allocated < 128 * 1024 * 1024) {
    // We must use the return value from malloc, otherwise the compiler may
    // optimize out the call altogether!
    void* ptr = malloc(kRequestSize);
    EXPECT_NE(nullptr, ptr);
    objs.Push(ptr);
    allocated += kActualSize;
  }
  const double finish = HeapProfileReport(kActualSize);

  EXPECT_NEAR(allocated, finish - start, 0.05 * allocated);

  void* ptr;
  while (objs.TryPop(&ptr)) {
    free(ptr);
  }
}

}  // namespace
}  // namespace tcmalloc
