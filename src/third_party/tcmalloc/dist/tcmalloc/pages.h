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

#ifndef TCMALLOC_PAGES_H_
#define TCMALLOC_PAGES_H_

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/optimization.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Type that can hold the length of a run of pages
class Length {
 public:
  constexpr Length() : n_(0) {}
  explicit constexpr Length(uintptr_t n) : n_(n) {}

  constexpr Length(const Length&) = default;
  constexpr Length& operator=(const Length&) = default;

  constexpr size_t raw_num() const { return n_; }
  constexpr size_t in_bytes() const { return n_ * kPageSize; }
  double in_mib() const {
    return std::ldexp(static_cast<double>(n_),
                      static_cast<int>(kPageShift) - 20);
  }
  constexpr Length in_pages() const { return *this; }

  static constexpr Length min() { return Length(0); }
  static constexpr Length max() {
    return Length(std::numeric_limits<uintptr_t>::max() >> kPageShift);
  }

  constexpr Length& operator+=(Length rhs) {
    n_ += rhs.n_;
    return *this;
  }

  constexpr Length& operator-=(Length rhs) {
    TC_ASSERT_GE(n_, rhs.n_);
    n_ -= rhs.n_;
    return *this;
  }

  constexpr Length& operator*=(size_t rhs) {
    n_ *= rhs;
    return *this;
  }

  constexpr Length& operator/=(size_t rhs) {
    TC_ASSERT_NE(rhs, 0);
    n_ /= rhs;
    return *this;
  }

  constexpr Length& operator%=(Length rhs) {
    TC_ASSERT_NE(rhs.n_, 0);
    n_ %= rhs.n_;
    return *this;
  }

  friend constexpr bool operator<(Length lhs, Length rhs);
  friend constexpr bool operator>(Length lhs, Length rhs);
  friend constexpr bool operator<=(Length lhs, Length rhs);
  friend constexpr bool operator>=(Length lhs, Length rhs);
  friend constexpr bool operator==(Length lhs, Length rhs);
  friend constexpr bool operator!=(Length lhs, Length rhs);

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Length& v) {
    absl::Format(&sink, "%zu", v.in_bytes());
  }

 private:
  uintptr_t n_;
};

inline bool AbslParseFlag(absl::string_view text, Length* l,
                          std::string* /* error */) {
  uintptr_t n;
  if (!absl::SimpleAtoi(text, &n)) {
    return false;
  }
  *l = Length(n);
  return true;
}

inline std::string AbslUnparseFlag(Length l) {
  return absl::StrCat(l.raw_num());
}

// A single aligned page.
class PageId {
 public:
  constexpr PageId() : pn_(0) {}
  constexpr PageId(const PageId& p) = default;
  constexpr PageId& operator=(const PageId& p) = default;

  constexpr explicit PageId(uintptr_t pn) : pn_(pn) {}

  void* start_addr() const {
    return reinterpret_cast<void*>(pn_ << kPageShift);
  }

  uintptr_t start_uintptr() const { return pn_ << kPageShift; }

  size_t index() const { return pn_; }

  constexpr PageId& operator+=(Length rhs) {
    pn_ += rhs.raw_num();
    return *this;
  }

  constexpr PageId& operator-=(Length rhs) {
    TC_ASSERT_GE(pn_, rhs.raw_num());
    pn_ -= rhs.raw_num();
    return *this;
  }

  template <typename H>
  friend H AbslHashValue(H h, const PageId& p) {
    return H::combine(std::move(h), p.pn_);
  }

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const PageId& v) {
    absl::Format(&sink, "%p", v.start_addr());
  }

 private:
  friend constexpr bool operator<(PageId lhs, PageId rhs);
  friend constexpr bool operator>(PageId lhs, PageId rhs);
  friend constexpr bool operator<=(PageId lhs, PageId rhs);
  friend constexpr bool operator>=(PageId lhs, PageId rhs);
  friend constexpr bool operator==(PageId lhs, PageId rhs);
  friend constexpr bool operator!=(PageId lhs, PageId rhs);
  friend constexpr Length operator-(PageId lhs, PageId rhs);

  uintptr_t pn_;
};

TCMALLOC_ATTRIBUTE_CONST
inline constexpr Length LengthFromBytes(size_t bytes) {
  return Length(bytes >> kPageShift);
}

// Convert byte size into pages.  This won't overflow, but may return
// an unreasonably large value if bytes is huge enough.
TCMALLOC_ATTRIBUTE_CONST
inline constexpr Length BytesToLengthCeil(size_t bytes) {
  return Length((bytes >> kPageShift) +
                ((bytes & (kPageSize - 1)) > 0 ? 1 : 0));
}

TCMALLOC_ATTRIBUTE_CONST
inline constexpr Length BytesToLengthFloor(size_t bytes) {
  return Length(bytes >> kPageShift);
}

inline constexpr Length kMaxValidPages = Length::max();
// For all span-lengths < kMaxPages we keep an exact-size list.
inline constexpr Length kMaxPages = Length(1 << (20 - kPageShift));

inline PageId& operator++(PageId& p) {  // NOLINT(runtime/references)
  return p += Length(1);
}

TCMALLOC_ATTRIBUTE_CONST
inline constexpr bool operator<(PageId lhs, PageId rhs) {
  return lhs.pn_ < rhs.pn_;
}

TCMALLOC_ATTRIBUTE_CONST
inline constexpr bool operator>(PageId lhs, PageId rhs) {
  return lhs.pn_ > rhs.pn_;
}

TCMALLOC_ATTRIBUTE_CONST
inline constexpr bool operator<=(PageId lhs, PageId rhs) {
  return lhs.pn_ <= rhs.pn_;
}

TCMALLOC_ATTRIBUTE_CONST
inline constexpr bool operator>=(PageId lhs, PageId rhs) {
  return lhs.pn_ >= rhs.pn_;
}

TCMALLOC_ATTRIBUTE_CONST
inline constexpr bool operator==(PageId lhs, PageId rhs) {
  return lhs.pn_ == rhs.pn_;
}

TCMALLOC_ATTRIBUTE_CONST
inline constexpr bool operator!=(PageId lhs, PageId rhs) {
  return lhs.pn_ != rhs.pn_;
}

TCMALLOC_ATTRIBUTE_CONST
inline constexpr PageId operator+(PageId lhs, Length rhs) { return lhs += rhs; }

TCMALLOC_ATTRIBUTE_CONST
inline constexpr PageId operator+(Length lhs, PageId rhs) { return rhs += lhs; }

TCMALLOC_ATTRIBUTE_CONST
inline constexpr PageId operator-(PageId lhs, Length rhs) { return lhs -= rhs; }

TCMALLOC_ATTRIBUTE_CONST
inline constexpr Length operator-(PageId lhs, PageId rhs) {
  TC_ASSERT_GE(lhs.pn_, rhs.pn_);
  return Length(lhs.pn_ - rhs.pn_);
}

TCMALLOC_ATTRIBUTE_CONST
inline PageId PageIdContaining(const void* p) {
  return PageId(reinterpret_cast<uintptr_t>(p) >> kPageShift);
}

TCMALLOC_ATTRIBUTE_CONST
inline constexpr bool operator<(Length lhs, Length rhs) {
  return lhs.n_ < rhs.n_;
}

TCMALLOC_ATTRIBUTE_CONST
inline constexpr bool operator>(Length lhs, Length rhs) {
  return lhs.n_ > rhs.n_;
}

TCMALLOC_ATTRIBUTE_CONST
inline constexpr bool operator<=(Length lhs, Length rhs) {
  return lhs.n_ <= rhs.n_;
}

TCMALLOC_ATTRIBUTE_CONST
inline constexpr bool operator>=(Length lhs, Length rhs) {
  return lhs.n_ >= rhs.n_;
}

TCMALLOC_ATTRIBUTE_CONST
inline constexpr bool operator==(Length lhs, Length rhs) {
  return lhs.n_ == rhs.n_;
}

TCMALLOC_ATTRIBUTE_CONST
inline constexpr bool operator!=(Length lhs, Length rhs) {
  return lhs.n_ != rhs.n_;
}

inline Length& operator++(Length& l) { return l += Length(1); }

inline Length& operator--(Length& l) { return l -= Length(1); }

TCMALLOC_ATTRIBUTE_CONST
inline constexpr Length operator+(Length lhs, Length rhs) { return lhs += rhs; }

TCMALLOC_ATTRIBUTE_CONST
inline constexpr Length operator-(Length lhs, Length rhs) { return lhs -= rhs; }

TCMALLOC_ATTRIBUTE_CONST
inline constexpr Length operator*(Length lhs, size_t rhs) { return lhs *= rhs; }

TCMALLOC_ATTRIBUTE_CONST
inline constexpr Length operator*(size_t lhs, Length rhs) { return rhs *= lhs; }

TCMALLOC_ATTRIBUTE_CONST
inline constexpr size_t operator/(Length lhs, Length rhs) {
  TC_ASSERT_NE(rhs.raw_num(), 0);
  return lhs.raw_num() / rhs.raw_num();
}

TCMALLOC_ATTRIBUTE_CONST
inline constexpr Length operator/(Length lhs, size_t rhs) { return lhs /= rhs; }

TCMALLOC_ATTRIBUTE_CONST
inline constexpr Length operator%(Length lhs, Length rhs) {
  TC_ASSERT_NE(rhs.raw_num(), 0);
  return lhs %= rhs;
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_PAGES_H_
