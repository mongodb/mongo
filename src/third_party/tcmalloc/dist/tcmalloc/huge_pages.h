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
//
// Helpers for nicely typed interfaces that pass around refs to large
// ranges.  You probably don't want to store HugeRanges long term
// (nothing will break, but that's not what they're efficient for.)
#ifndef TCMALLOC_HUGE_PAGES_H_
#define TCMALLOC_HUGE_PAGES_H_

#include <stddef.h>
#include <stdint.h>

#include <cmath>
#include <limits>
#include <ostream>
#include <utility>

#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/optimization.h"
#include "tcmalloc/pages.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

inline constexpr Length kPagesPerHugePage =
    Length(1 << (kHugePageShift - kPageShift));

// A single aligned huge page.
struct HugePage {
  void* start_addr() const {
    TC_ASSERT_LE(pn, kMaxPageNumber);
    return reinterpret_cast<void*>(pn << kHugePageShift);
  }

  PageId first_page() const {
    TC_ASSERT_LE(pn, kMaxPageNumber);
    return PageId(pn << (kHugePageShift - kPageShift));
  }

  size_t index() const {
    TC_ASSERT_LE(pn, kMaxPageNumber);
    return pn;
  }

  template <typename H>
  friend H AbslHashValue(H h, const HugePage& p) {
    return H::combine(std::move(h), p.pn);
  }

  static constexpr uintptr_t kMaxPageNumber =
      std::numeric_limits<uintptr_t>::max() >> kHugePageShift;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const HugePage& v) {
    absl::Format(&sink, "%p", v.start_addr());
  }

  uintptr_t pn;
};

struct HugeLength {
  constexpr HugeLength() : n(0) {}
  explicit HugeLength(double x) : n(ceil(x)) { TC_ASSERT_GE(x, 0); }
  constexpr size_t raw_num() const { return n; }
  constexpr size_t in_bytes() const { return n * kHugePageSize; }
  constexpr size_t in_mib() const {
    static_assert(kHugePageSize >= 1024 * 1024, "tiny hugepages?");
    return n * (kHugePageSize / 1024 / 1024);
  }
  constexpr Length in_pages() const { return n * kPagesPerHugePage; }

  // It is possible to have a HugeLength that corresponds to more
  // bytes than can be addressed (i.e. > size_t.)  Check for that.
  bool overflows() const;
  static constexpr HugeLength min() {
    return HugeLength(static_cast<size_t>(0));
  }
  static constexpr HugeLength max() {
    return HugeLength(static_cast<size_t>(HugePage::kMaxPageNumber));
  }

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const HugeLength& v) {
    absl::Format(&sink, "%zu", v.in_bytes());
  }

 private:
  size_t n;

  explicit constexpr HugeLength(size_t x) : n(x) {}
  friend constexpr HugeLength NHugePages(size_t n);
  friend HugeLength& operator++(HugeLength&);
  friend HugeLength& operator--(HugeLength&);
  friend constexpr bool operator<(HugeLength, HugeLength);
  friend constexpr bool operator>(HugeLength, HugeLength);
  friend constexpr bool operator<=(HugeLength, HugeLength);
  friend constexpr bool operator>=(HugeLength, HugeLength);
  friend constexpr bool operator==(HugeLength, HugeLength);
  friend constexpr bool operator!=(HugeLength, HugeLength);
  friend constexpr size_t operator/(HugeLength, HugeLength);
  friend constexpr HugeLength operator%(HugeLength, HugeLength);
  friend constexpr HugeLength operator*(HugeLength, size_t);
  friend HugeLength& operator*=(HugeLength&, size_t);
  friend constexpr HugeLength operator/(HugeLength, size_t);
  friend constexpr HugePage operator+(HugePage lhs, HugeLength rhs);
  friend constexpr HugePage operator-(HugePage lhs, HugeLength rhs);
  friend HugePage& operator+=(HugePage& lhs, HugeLength rhs);
  friend constexpr HugeLength operator+(HugeLength lhs, HugeLength rhs);
  friend HugeLength& operator+=(HugeLength& lhs, HugeLength rhs);
  friend constexpr HugeLength operator-(HugeLength lhs, HugeLength rhs);
  friend HugeLength& operator-=(HugeLength& lhs, HugeLength rhs);
};

// Literal constructors (made explicit to avoid accidental uses when
// another unit was meant.)
TCMALLOC_ATTRIBUTE_CONST
inline constexpr HugeLength NHugePages(size_t n) { return HugeLength(n); }

TCMALLOC_ATTRIBUTE_CONST
inline constexpr HugeLength HLFromBytes(size_t bytes) {
  return NHugePages(bytes / kHugePageSize);
}

// Rounds *up* to the nearest hugepage.
TCMALLOC_ATTRIBUTE_CONST
inline constexpr HugeLength HLFromPages(Length pages) {
  return NHugePages((pages + kPagesPerHugePage - Length(1)) /
                    kPagesPerHugePage);
}

inline HugeLength& operator++(HugeLength& len) {  // NOLINT(runtime/references)
  len.n++;
  return len;
}

inline HugePage& operator++(HugePage& p) {  // NOLINT(runtime/references)
  TC_ASSERT_LE(p.pn + 1, HugePage::kMaxPageNumber);
  p.pn++;
  return p;
}

inline HugeLength& operator--(HugeLength& len) {  // NOLINT(runtime/references)
  TC_ASSERT_GE(len.n, 1);
  len.n--;
  return len;
}

inline constexpr bool operator<(HugeLength lhs, HugeLength rhs) {
  return lhs.n < rhs.n;
}

TCMALLOC_ATTRIBUTE_CONST
inline constexpr bool operator>(HugeLength lhs, HugeLength rhs) {
  return lhs.n > rhs.n;
}

TCMALLOC_ATTRIBUTE_CONST
inline constexpr bool operator<=(HugeLength lhs, HugeLength rhs) {
  return lhs.n <= rhs.n;
}

TCMALLOC_ATTRIBUTE_CONST
inline constexpr bool operator<(HugePage lhs, HugePage rhs) {
  return lhs.pn < rhs.pn;
}

TCMALLOC_ATTRIBUTE_CONST
inline constexpr bool operator>(HugePage lhs, HugePage rhs) {
  return lhs.pn > rhs.pn;
}

TCMALLOC_ATTRIBUTE_CONST
inline constexpr bool operator>=(HugeLength lhs, HugeLength rhs) {
  return lhs.n >= rhs.n;
}

TCMALLOC_ATTRIBUTE_CONST
inline constexpr bool operator<=(HugePage lhs, HugePage rhs) {
  return lhs.pn <= rhs.pn;
}

TCMALLOC_ATTRIBUTE_CONST
inline constexpr bool operator>=(HugePage lhs, HugePage rhs) {
  return lhs.pn >= rhs.pn;
}

TCMALLOC_ATTRIBUTE_CONST
inline constexpr bool operator==(HugePage lhs, HugePage rhs) {
  return lhs.pn == rhs.pn;
}

TCMALLOC_ATTRIBUTE_CONST
inline constexpr bool operator!=(HugePage lhs, HugePage rhs) {
  return !(lhs == rhs);
}

TCMALLOC_ATTRIBUTE_CONST
inline constexpr bool operator==(HugeLength lhs, HugeLength rhs) {
  return lhs.n == rhs.n;
}

TCMALLOC_ATTRIBUTE_CONST
inline constexpr bool operator!=(HugeLength lhs, HugeLength rhs) {
  return lhs.n != rhs.n;
}

TCMALLOC_ATTRIBUTE_CONST
inline constexpr size_t operator/(HugeLength lhs, HugeLength rhs) {
  return lhs.n / rhs.n;
}

TCMALLOC_ATTRIBUTE_CONST
inline constexpr HugeLength operator*(HugeLength lhs, size_t rhs) {
  return NHugePages(lhs.n * rhs);
}

TCMALLOC_ATTRIBUTE_CONST
inline constexpr HugeLength operator/(HugeLength lhs, size_t rhs) {
  return NHugePages(lhs.n / rhs);
}

inline HugeLength& operator*=(HugeLength& lhs, size_t rhs) {
  lhs.n *= rhs;
  return lhs;
}

TCMALLOC_ATTRIBUTE_CONST
inline constexpr HugeLength operator%(HugeLength lhs, HugeLength rhs) {
  return NHugePages(lhs.n % rhs.n);
}

TCMALLOC_ATTRIBUTE_CONST
inline constexpr HugePage operator+(HugePage lhs, HugeLength rhs) {
  TC_ASSERT_LE(lhs.pn + rhs.n, HugePage::kMaxPageNumber);
  return HugePage{lhs.pn + rhs.n};
}

TCMALLOC_ATTRIBUTE_CONST
inline constexpr HugePage operator+(HugeLength lhs, HugePage rhs) {
  return rhs + lhs;
}

TCMALLOC_ATTRIBUTE_CONST
inline constexpr HugePage operator-(HugePage lhs, HugeLength rhs) {
  return TC_ASSERT_GE(lhs.pn, rhs.n), HugePage{lhs.pn - rhs.n};
}

TCMALLOC_ATTRIBUTE_CONST
inline constexpr HugeLength operator-(HugePage lhs, HugePage rhs) {
  return TC_ASSERT_GE(lhs.pn, rhs.pn), NHugePages(lhs.pn - rhs.pn);
}

inline HugePage& operator+=(HugePage& lhs, HugeLength rhs) {
  TC_ASSERT_LE(lhs.pn + rhs.n, HugePage::kMaxPageNumber);
  lhs.pn += rhs.n;
  return lhs;
}

TCMALLOC_ATTRIBUTE_CONST
inline constexpr HugeLength operator+(HugeLength lhs, HugeLength rhs) {
  return NHugePages(lhs.n + rhs.n);
}

inline HugeLength& operator+=(HugeLength& lhs, HugeLength rhs) {
  lhs.n += rhs.n;
  return lhs;
}

TCMALLOC_ATTRIBUTE_CONST
inline constexpr HugeLength operator-(HugeLength lhs, HugeLength rhs) {
  return TC_ASSERT_GE(lhs.n, rhs.n), NHugePages(lhs.n - rhs.n);
}

inline HugeLength& operator-=(HugeLength& lhs, HugeLength rhs) {
  TC_ASSERT_GE(lhs.n, rhs.n);
  lhs.n -= rhs.n;
  return lhs;
}

inline bool HugeLength::overflows() const {
  return *this > HLFromBytes(std::numeric_limits<size_t>::max());
}

inline void PrintTo(const HugeLength& n, ::std::ostream* os) {
  *os << n.raw_num() << "hps";
}

TCMALLOC_ATTRIBUTE_CONST
inline HugePage HugePageContaining(PageId p) {
  return {p.index() >> (kHugePageShift - kPageShift)};
}

TCMALLOC_ATTRIBUTE_CONST
inline HugePage HugePageContaining(void* p) {
  return HugePageContaining(PageIdContaining(p));
}

// A set of contiguous huge pages.
struct HugeRange {
  void* start_addr() const { return first.start_addr(); }
  void* end_addr() const { return (first + n).start_addr(); }
  size_t byte_len() const {
    return static_cast<char*>(end_addr()) - static_cast<char*>(start_addr());
  }

  // Assume any range starting at 0 is bogus.
  bool valid() const { return first.start_addr() != nullptr; }

  constexpr HugePage start() const { return first; }

  constexpr HugeLength len() const { return n; }

  HugePage operator[](HugeLength i) const { return first + i; }

  template <typename H>
  friend H AbslHashValue(H h, const HugeRange& r) {
    return H::combine(std::move(h), r.start().start_addr(), r.len().raw_num());
  }

  bool contains(PageId p) const { return contains(HugePageContaining(p)); }
  bool contains(HugePage p) const { return p >= first && (p - first) < n; }
  bool contains(HugeRange r) const {
    return r.first >= first && (r.first + r.n) <= (first + n);
  }

  bool intersects(HugeRange r) const {
    return r.contains(start()) || contains(r.start());
  }

  // True iff r is our immediate successor (i.e. this + r is one large
  // (non-overlapping) range.)
  bool precedes(HugeRange r) const { return end_addr() == r.start_addr(); }

  static HugeRange Nil() {
    return {HugePageContaining(nullptr), NHugePages(0)};
  }

  static HugeRange Make(HugePage p, HugeLength n) { return {p, n}; }

  HugePage first;
  HugeLength n;
};

inline constexpr bool operator==(HugeRange lhs, HugeRange rhs) {
  return lhs.start() == rhs.start() && lhs.len() == rhs.len();
}

// REQUIRES: a and b are disjoint but adjacent (in that order)

inline HugeRange Join(HugeRange a, HugeRange b) {
  TC_CHECK(a.precedes(b));
  return {a.start(), a.len() + b.len()};
}

// REQUIRES r.len() >= n
// Splits r into two ranges, one of length n.  The other is either the rest
// of the space (if any) or Nil.
inline std::pair<HugeRange, HugeRange> Split(HugeRange r, HugeLength n) {
  TC_ASSERT_GE(r.len(), n);
  if (r.len() > n) {
    return {HugeRange::Make(r.start(), n),
            HugeRange::Make(r.start() + n, r.len() - n)};
  } else {
    return {r, HugeRange::Nil()};
  }
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
#endif  // TCMALLOC_HUGE_PAGES_H_
