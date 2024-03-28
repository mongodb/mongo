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

#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/size_class_info.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Columns in the following tables:
// - bytes: size of the size class
// - pages: number of pages per span
// - batch: preferred number of objects for transfers between caches
// - cap: maximum cpu cache capacity
// - class: size class number
// - objs: number of objects per span
// - waste/fixed: fixed per-size-class overhead due to end-of-span fragmentation
//   and other factors. For instance, if we have a 96 byte size class, and use
//   a single 8KiB page, then we will hold 85 objects per span, and have 32
//   bytes left over. There is also a fixed component of 48 bytes of TCMalloc
//   metadata per span. Together, the fixed overhead would be wasted/allocated
//   = (32 + 48) / (8192 - 32) ~= 0.98%.
// - waste/sampling: overhead due to heap sampling
//   (rounding to page size, proxy object, metadata).
// - inc: increment from the previous size class. This caps the dynamic
//   overhead component based on mismatches between the number of bytes
//   requested and the number of bytes provided by the size class. Together
//   they sum to the total overhead; for instance if you asked for a 50-byte
//   allocation that rounds up to a 64-byte size class, the dynamic overhead
//   would be 28%, and if waste were 22% it would mean (on average) 25 bytes
//   of overhead for allocations of that size.

// clang-format off
#if defined(__cpp_aligned_new) && __STDCPP_DEFAULT_NEW_ALIGNMENT__ <= 8
#if TCMALLOC_PAGE_SHIFT == 13
static_assert(kMaxSize == 262144, "kMaxSize mismatch");
static constexpr SizeClassAssumptions Assumptions{
  .has_expanded_classes = true,
  .span_size = 48,
  .sampling_rate = 2097152,
  .large_size = 1024,
  .large_size_alignment = 128,
};
static constexpr SizeClassInfo List[] = {
//                                         |    waste     |
//  bytes pages batch   cap    class  objs |fixed sampling|    inc
  {     0,    0,    0,    0},  //  0     0  0.00%    0.00%   0.00%
  {     8,    1,   32, 4712},  //  0  1024  0.58%    0.42%   0.00%
  {    16,    1,   32, 4712},  //  1   512  0.58%    0.42% 100.00%
  {    32,    1,   32, 4713},  //  2   256  0.58%    0.42% 100.00%
  {    64,    1,   32, 4712},  //  3   128  0.58%    0.42% 100.00%
  {   128,    1,   32, 4712},  //  4    64  0.58%    0.42% 100.00%
  {   256,    1,   32, 2427},  //  5    32  0.58%    0.42% 100.00%
  {   512,    1,   32, 1337},  //  6    16  0.58%    0.42% 100.00%
  {  1024,    1,   32,  789},  //  7     8  0.58%    0.42% 100.00%
  {  2048,    2,   32,  513},  //  8     8  0.29%    0.42% 100.00%
  {  4096,    1,   16,  529},  //  9     2  0.58%    0.43% 100.00%
  {  8192,    1,    8,  384},  // 10     1  0.58%    0.03% 100.00%
  { 16384,    2,    4,  320},  // 11     1  0.29%    0.03% 100.00%
  { 32768,    4,    2,  318},  // 12     1  0.15%    0.03% 100.00%
  { 65536,    8,    2,  301},  // 13     1  0.07%    0.03% 100.00%
  {131072,   16,    2,  299},  // 14     1  0.04%    0.03% 100.00%
  {262144,   32,    2,  294},  // 15     1  0.02%    0.03% 100.00%
};
#elif TCMALLOC_PAGE_SHIFT == 15
static_assert(kMaxSize == 262144, "kMaxSize mismatch");
static constexpr SizeClassAssumptions Assumptions{
  .has_expanded_classes = true,
  .span_size = 48,
  .sampling_rate = 2097152,
  .large_size = 1024,
  .large_size_alignment = 128,
};
static constexpr SizeClassInfo List[] = {
//                                         |    waste     |
//  bytes pages batch   cap    class  objs |fixed sampling|    inc
  {     0,    0,    0,    0},  //  0     0  0.00%    0.00%   0.00%
  {     8,    1,   32, 4795},  //  0  4096  0.15%    1.60%   0.00%
  {    16,    1,   32, 4795},  //  1  2048  0.15%    1.60% 100.00%
  {    32,    1,   32, 4795},  //  2  1024  0.15%    1.60% 100.00%
  {    64,    1,   32, 4795},  //  3   512  0.15%    1.60% 100.00%
  {   128,    1,   32, 4795},  //  4   256  0.15%    1.60% 100.00%
  {   256,    1,   32, 2276},  //  5   128  0.15%    1.60% 100.00%
  {   512,    1,   32, 1114},  //  6    64  0.15%    1.60% 100.00%
  {  1024,    1,   32,  780},  //  7    32  0.15%    1.60% 100.00%
  {  2048,    1,   32,  499},  //  8    16  0.15%    1.60% 100.00%
  {  4096,    1,   16,  492},  //  9     8  0.15%    1.60% 100.00%
  {  8192,    1,    8,  361},  // 10     4  0.15%    1.60% 100.00%
  { 16384,    1,    4,  332},  // 11     2  0.15%    1.60% 100.00%
  { 32768,    1,    2,  321},  // 12     1  0.15%    0.03% 100.00%
  { 65536,    2,    2,  307},  // 13     1  0.07%    0.03% 100.00%
  {131072,    4,    2,  316},  // 14     1  0.04%    0.03% 100.00%
  {262144,    8,    2,  299},  // 15     1  0.02%    0.03% 100.00%
};
#elif TCMALLOC_PAGE_SHIFT == 18
static_assert(kMaxSize == 262144, "kMaxSize mismatch");
static constexpr SizeClassAssumptions Assumptions{
  .has_expanded_classes = true,
  .span_size = 48,
  .sampling_rate = 2097152,
  .large_size = 1024,
  .large_size_alignment = 128,
};
static constexpr SizeClassInfo List[] = {
//                                         |    waste     |
//  bytes pages batch   cap    class  objs |fixed sampling|    inc
  {     0,    0,    0,    0},  //  0     0  0.00%    0.00%   0.00%
  {     8,    1,   32, 4635},  //  0 32768  0.02%   12.53%   0.00%
  {    16,    1,   32, 4635},  //  1 16384  0.02%   12.53% 100.00%
  {    32,    1,   32, 4635},  //  2  8192  0.02%   12.53% 100.00%
  {    64,    1,   32, 4635},  //  3  4096  0.02%   12.53% 100.00%
  {   128,    1,   32, 4635},  //  4  2048  0.02%   12.53% 100.00%
  {   256,    1,   32, 2573},  //  5  1024  0.02%   12.53% 100.00%
  {   512,    1,   32, 1405},  //  6   512  0.02%   12.53% 100.00%
  {  1024,    1,   32,  788},  //  7   256  0.02%   12.53% 100.00%
  {  2048,    1,   32,  600},  //  8   128  0.02%   12.53% 100.00%
  {  4096,    1,   16,  613},  //  9    64  0.02%   12.53% 100.00%
  {  8192,    1,    8,  378},  // 10    32  0.02%   12.53% 100.00%
  { 16384,    1,    4,  328},  // 11    16  0.02%   12.53% 100.00%
  { 32768,    1,    2,  339},  // 12     8  0.02%   12.53% 100.00%
  { 65536,    1,    2,  294},  // 13     4  0.02%   12.53% 100.00%
  {131072,    1,    2,  289},  // 14     2  0.02%   12.53% 100.00%
  {262144,    1,    2,  290},  // 15     1  0.02%    0.03% 100.00%
};
#elif TCMALLOC_PAGE_SHIFT == 12
static_assert(kMaxSize == 8192, "kMaxSize mismatch");
static constexpr SizeClassAssumptions Assumptions{
  .has_expanded_classes = false,
  .span_size = 48,
  .sampling_rate = 524288,
  .large_size = 1024,
  .large_size_alignment = 128,
};
static constexpr SizeClassInfo List[] = {
//                                         |    waste     |
//  bytes pages batch   cap    class  objs |fixed sampling|    inc
  {     0,    0,    0,    0},  //  0     0  0.00%    0.00%   0.00%
  {     8,    1,   32, 4840},  //  0   512  1.16%    0.92%   0.00%
  {    16,    1,   32, 4840},  //  1   256  1.16%    0.92% 100.00%
  {    32,    1,   32, 4840},  //  2   128  1.16%    0.92% 100.00%
  {    64,    1,   32, 4840},  //  3    64  1.16%    0.92% 100.00%
  {   128,    1,   32, 4840},  //  4    32  1.16%    0.92% 100.00%
  {   256,    1,   32, 4050},  //  5    16  1.16%    0.92% 100.00%
  {   512,    1,   32, 1491},  //  6     8  1.16%    0.92% 100.00%
  {  1024,    2,   32, 1425},  //  7     8  0.58%    0.92% 100.00%
  {  2048,    4,   32,  612},  //  8     8  0.29%    0.92% 100.00%
  {  4096,    4,   16,  666},  //  9     4  0.29%    0.92% 100.00%
  {  8192,    4,    8,  302},  // 10     2  0.29%    1.70% 100.00%
};
#else
#error "Unsupported TCMALLOC_PAGE_SHIFT value!"
#endif
#else
#if TCMALLOC_PAGE_SHIFT == 13
static_assert(kMaxSize == 262144, "kMaxSize mismatch");
static constexpr SizeClassAssumptions Assumptions{
  .has_expanded_classes = true,
  .span_size = 48,
  .sampling_rate = 2097152,
  .large_size = 1024,
  .large_size_alignment = 128,
};
static constexpr SizeClassInfo List[] = {
//                                         |    waste     |
//  bytes pages batch   cap    class  objs |fixed sampling|    inc
  {     0,    0,    0,    0},  //  0     0  0.00%    0.00%   0.00%
  {     8,    1,   32, 4712},  //  0  1024  0.58%    0.42%   0.00%
  {    16,    1,   32, 4712},  //  1   512  0.58%    0.42% 100.00%
  {    32,    1,   32, 4713},  //  2   256  0.58%    0.42% 100.00%
  {    64,    1,   32, 4712},  //  3   128  0.58%    0.42% 100.00%
  {   128,    1,   32, 4712},  //  4    64  0.58%    0.42% 100.00%
  {   256,    1,   32, 2427},  //  5    32  0.58%    0.42% 100.00%
  {   512,    1,   32, 1337},  //  6    16  0.58%    0.42% 100.00%
  {  1024,    1,   32,  789},  //  7     8  0.58%    0.42% 100.00%
  {  2048,    2,   32,  513},  //  8     8  0.29%    0.42% 100.00%
  {  4096,    1,   16,  529},  //  9     2  0.58%    0.43% 100.00%
  {  8192,    1,    8,  384},  // 10     1  0.58%    0.03% 100.00%
  { 16384,    2,    4,  320},  // 11     1  0.29%    0.03% 100.00%
  { 32768,    4,    2,  318},  // 12     1  0.15%    0.03% 100.00%
  { 65536,    8,    2,  301},  // 13     1  0.07%    0.03% 100.00%
  {131072,   16,    2,  299},  // 14     1  0.04%    0.03% 100.00%
  {262144,   32,    2,  294},  // 15     1  0.02%    0.03% 100.00%
};
#elif TCMALLOC_PAGE_SHIFT == 15
static_assert(kMaxSize == 262144, "kMaxSize mismatch");
static constexpr SizeClassAssumptions Assumptions{
  .has_expanded_classes = true,
  .span_size = 48,
  .sampling_rate = 2097152,
  .large_size = 1024,
  .large_size_alignment = 128,
};
static constexpr SizeClassInfo List[] = {
//                                         |    waste     |
//  bytes pages batch   cap    class  objs |fixed sampling|    inc
  {     0,    0,    0,    0},  //  0     0  0.00%    0.00%   0.00%
  {     8,    1,   32, 4795},  //  0  4096  0.15%    1.60%   0.00%
  {    16,    1,   32, 4795},  //  1  2048  0.15%    1.60% 100.00%
  {    32,    1,   32, 4795},  //  2  1024  0.15%    1.60% 100.00%
  {    64,    1,   32, 4795},  //  3   512  0.15%    1.60% 100.00%
  {   128,    1,   32, 4795},  //  4   256  0.15%    1.60% 100.00%
  {   256,    1,   32, 2276},  //  5   128  0.15%    1.60% 100.00%
  {   512,    1,   32, 1114},  //  6    64  0.15%    1.60% 100.00%
  {  1024,    1,   32,  780},  //  7    32  0.15%    1.60% 100.00%
  {  2048,    1,   32,  499},  //  8    16  0.15%    1.60% 100.00%
  {  4096,    1,   16,  492},  //  9     8  0.15%    1.60% 100.00%
  {  8192,    1,    8,  361},  // 10     4  0.15%    1.60% 100.00%
  { 16384,    1,    4,  332},  // 11     2  0.15%    1.60% 100.00%
  { 32768,    1,    2,  321},  // 12     1  0.15%    0.03% 100.00%
  { 65536,    2,    2,  307},  // 13     1  0.07%    0.03% 100.00%
  {131072,    4,    2,  316},  // 14     1  0.04%    0.03% 100.00%
  {262144,    8,    2,  299},  // 15     1  0.02%    0.03% 100.00%
};
#elif TCMALLOC_PAGE_SHIFT == 18
static_assert(kMaxSize == 262144, "kMaxSize mismatch");
static constexpr SizeClassAssumptions Assumptions{
  .has_expanded_classes = true,
  .span_size = 48,
  .sampling_rate = 2097152,
  .large_size = 1024,
  .large_size_alignment = 128,
};
static constexpr SizeClassInfo List[] = {
//                                         |    waste     |
//  bytes pages batch   cap    class  objs |fixed sampling|    inc
  {     0,    0,    0,    0},  //  0     0  0.00%    0.00%   0.00%
  {     8,    1,   32, 4635},  //  0 32768  0.02%   12.53%   0.00%
  {    16,    1,   32, 4635},  //  1 16384  0.02%   12.53% 100.00%
  {    32,    1,   32, 4635},  //  2  8192  0.02%   12.53% 100.00%
  {    64,    1,   32, 4635},  //  3  4096  0.02%   12.53% 100.00%
  {   128,    1,   32, 4635},  //  4  2048  0.02%   12.53% 100.00%
  {   256,    1,   32, 2573},  //  5  1024  0.02%   12.53% 100.00%
  {   512,    1,   32, 1405},  //  6   512  0.02%   12.53% 100.00%
  {  1024,    1,   32,  788},  //  7   256  0.02%   12.53% 100.00%
  {  2048,    1,   32,  600},  //  8   128  0.02%   12.53% 100.00%
  {  4096,    1,   16,  613},  //  9    64  0.02%   12.53% 100.00%
  {  8192,    1,    8,  378},  // 10    32  0.02%   12.53% 100.00%
  { 16384,    1,    4,  328},  // 11    16  0.02%   12.53% 100.00%
  { 32768,    1,    2,  339},  // 12     8  0.02%   12.53% 100.00%
  { 65536,    1,    2,  294},  // 13     4  0.02%   12.53% 100.00%
  {131072,    1,    2,  289},  // 14     2  0.02%   12.53% 100.00%
  {262144,    1,    2,  290},  // 15     1  0.02%    0.03% 100.00%
};
#elif TCMALLOC_PAGE_SHIFT == 12
static_assert(kMaxSize == 8192, "kMaxSize mismatch");
static constexpr SizeClassAssumptions Assumptions{
  .has_expanded_classes = false,
  .span_size = 48,
  .sampling_rate = 524288,
  .large_size = 1024,
  .large_size_alignment = 128,
};
static constexpr SizeClassInfo List[] = {
//                                         |    waste     |
//  bytes pages batch   cap    class  objs |fixed sampling|    inc
  {     0,    0,    0,    0},  //  0     0  0.00%    0.00%   0.00%
  {     8,    1,   32, 4840},  //  0   512  1.16%    0.92%   0.00%
  {    16,    1,   32, 4840},  //  1   256  1.16%    0.92% 100.00%
  {    32,    1,   32, 4840},  //  2   128  1.16%    0.92% 100.00%
  {    64,    1,   32, 4840},  //  3    64  1.16%    0.92% 100.00%
  {   128,    1,   32, 4840},  //  4    32  1.16%    0.92% 100.00%
  {   256,    1,   32, 4050},  //  5    16  1.16%    0.92% 100.00%
  {   512,    1,   32, 1491},  //  6     8  1.16%    0.92% 100.00%
  {  1024,    2,   32, 1425},  //  7     8  0.58%    0.92% 100.00%
  {  2048,    4,   32,  612},  //  8     8  0.29%    0.92% 100.00%
  {  4096,    4,   16,  666},  //  9     4  0.29%    0.92% 100.00%
  {  8192,    4,    8,  302},  // 10     2  0.29%    1.70% 100.00%
};
#else
#error "Unsupported TCMALLOC_PAGE_SHIFT value!"
#endif
#endif
// clang-format on

static_assert(sizeof(List) / sizeof(List[0]) <= kNumBaseClasses);
extern constexpr SizeClasses kExperimentalPow2SizeClasses{List, Assumptions};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
