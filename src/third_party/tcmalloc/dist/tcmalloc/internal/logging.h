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
// Internal logging and related utility routines.

#ifndef TCMALLOC_INTERNAL_LOGGING_H_
#define TCMALLOC_INTERNAL_LOGGING_H_

#include <stdint.h>
#include <stdlib.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <string>
#include <type_traits>

#include "absl/base/attributes.h"
#include "absl/base/internal/sysinfo.h"
#include "absl/base/optimization.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "tcmalloc/internal/allocation_guard.h"
#include "tcmalloc/internal/config.h"

//-------------------------------------------------------------------
// Utility routines
//-------------------------------------------------------------------

// Safe logging helper: we write directly to the stderr file
// descriptor and avoid FILE buffering because that may invoke
// malloc().

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

static constexpr int kMaxStackDepth = 64;

// An opaque handle type used to identify allocations.
using AllocHandle = int64_t;

// size/depth are made the same size as a pointer so that some generic
// code below can conveniently cast them back and forth to void*.
struct StackTrace {
  // An opaque handle used by allocator to uniquely identify the sampled
  // memory block.
  AllocHandle sampled_alloc_handle;

  // For small sampled objects, we allocate a full span to hold the
  // sampled object.  However to avoid disturbing fragmentation
  // profiles, in such cases we also allocate a small proxy object
  // using the normal mechanism.
  //
  // proxy field is defined only for heap sample stack traces.
  // For heap samples, proxy==NULL iff size > kMaxSize.
  void* proxy;

  uintptr_t requested_size;
  uintptr_t requested_alignment;
  bool requested_size_returning;
  uintptr_t allocated_size;  // size after sizeclass/page rounding

  uint8_t access_hint;
  bool cold_allocated;

  uintptr_t depth;  // Number of PC values stored in array below
  void* stack[kMaxStackDepth];

  // weight is the expected number of *bytes* that were requested
  // between the previous sample and this one
  size_t weight;

  // Timestamp of allocation.
  absl::Time allocation_time;

  // If not nullptr, this is the start address of the span corresponding to this
  // sampled allocation. This may be nullptr for cases where it is not useful
  // for residency analysis such as for peakheapz.
  void* span_start_address = nullptr;

  // An integer representing the guarded status of the allocation.
  // The values are from the enum GuardedStatus in ../malloc_extension.h.
  int guarded_status;
};

#define TC_LOG(msg, ...)                                                \
  tcmalloc::tcmalloc_internal::LogImpl("%d %s:%d] " msg "\n", __FILE__, \
                                       __LINE__, ##__VA_ARGS__)

void RecordCrash(absl::string_view detector, absl::string_view error);
ABSL_ATTRIBUTE_NORETURN void CrashWithOOM(size_t alloc_size);
ABSL_ATTRIBUTE_NORETURN void CheckFailed(const char* file, int line,
                                         const char* msg, int msglen);

template <typename... Args>
ABSL_ATTRIBUTE_NORETURN ABSL_ATTRIBUTE_NOINLINE void CheckFailed(
    const char* func, const char* file, int line,
    const absl::FormatSpec<int, const char*, int, const char*, Args...>& format,
    const Args&... args) {
  AllocationGuard no_allocations;
  char buf[512];
  int n =
      absl::SNPrintF(buf, sizeof(buf), format, absl::base_internal::GetTID(),
                     file, line, func, args...);
  buf[sizeof(buf) - 1] = 0;
  CheckFailed(file, line, buf, std::min<size_t>(n, sizeof(buf) - 1));
}

void PrintStackTrace(void** stack_frames, size_t depth);
void PrintStackTraceFromSignalHandler(void* context);

// Tests can override this function to collect logging messages.
extern void (*log_message_writer)(const char* msg, int length);

template <typename... Args>
ABSL_ATTRIBUTE_NOINLINE void LogImpl(
    const absl::FormatSpec<int, Args...>& format, const Args&... args) {
  char buf[512];
  int n;
  {
    AllocationGuard no_allocations;
    n = absl::SNPrintF(buf, sizeof(buf), format, absl::base_internal::GetTID(),
                       args...);
  }
  buf[sizeof(buf) - 1] = 0;
  (*log_message_writer)(buf, std::min<size_t>(n, sizeof(buf) - 1));
}

// TC_BUG unconditionally aborts the program with the message.
#define TC_BUG(msg, ...)                                                       \
  tcmalloc::tcmalloc_internal::CheckFailed(__FUNCTION__, __FILE__, __LINE__,   \
                                           "%d %s:%d] CHECK in %s: " msg "\n", \
                                           ##__VA_ARGS__)

// TC_CHECK* check the given condition in both debug and release builds,
// and abort the program if the condition is false.
// Macros accept an additional optional formatted message, for example:
// TC_CHECK_EQ(a, b);
// TC_CHECK_EQ(a, b, "ptr=%p flags=%d", ptr, flags);
#define TC_CHECK(a, ...) TCMALLOC_CHECK_IMPL(a, #a, "" __VA_ARGS__)
#define TC_CHECK_EQ(a, b, ...) \
  TCMALLOC_CHECK_OP((a), ==, (b), #a, #b, "" __VA_ARGS__)
#define TC_CHECK_NE(a, b, ...) \
  TCMALLOC_CHECK_OP((a), !=, (b), #a, #b, "" __VA_ARGS__)
#define TC_CHECK_LT(a, b, ...) \
  TCMALLOC_CHECK_OP((a), <, (b), #a, #b, "" __VA_ARGS__)
#define TC_CHECK_LE(a, b, ...) \
  TCMALLOC_CHECK_OP((a), <=, (b), #a, #b, "" __VA_ARGS__)
#define TC_CHECK_GT(a, b, ...) \
  TCMALLOC_CHECK_OP((a), >, (b), #a, #b, "" __VA_ARGS__)
#define TC_CHECK_GE(a, b, ...) \
  TCMALLOC_CHECK_OP((a), >=, (b), #a, #b, "" __VA_ARGS__)

// TC_ASSERT* are debug-only versions of TC_CHECK*.
#ifndef NDEBUG
#define TC_ASSERT TC_CHECK
#define TC_ASSERT_EQ TC_CHECK_EQ
#define TC_ASSERT_NE TC_CHECK_NE
#define TC_ASSERT_LT TC_CHECK_LT
#define TC_ASSERT_LE TC_CHECK_LE
#define TC_ASSERT_GT TC_CHECK_GT
#define TC_ASSERT_GE TC_CHECK_GE
#else  // #ifndef NDEBUG
#define TC_ASSERT(a, ...) TC_CHECK(true || (a), ##__VA_ARGS__)
#define TC_ASSERT_EQ(a, b, ...) TC_ASSERT((a) == (b), ##__VA_ARGS__)
#define TC_ASSERT_NE(a, b, ...) TC_ASSERT((a) == (b), ##__VA_ARGS__)
#define TC_ASSERT_LT(a, b, ...) TC_ASSERT((a) == (b), ##__VA_ARGS__)
#define TC_ASSERT_LE(a, b, ...) TC_ASSERT((a) == (b), ##__VA_ARGS__)
#define TC_ASSERT_GT(a, b, ...) TC_ASSERT((a) == (b), ##__VA_ARGS__)
#define TC_ASSERT_GE(a, b, ...) TC_ASSERT((a) == (b), ##__VA_ARGS__)
#endif  // #ifndef NDEBUG

#define TCMALLOC_CHECK_IMPL(condition, str, msg, ...)          \
  ({                                                           \
    ABSL_PREDICT_TRUE((condition))                             \
    ? (void)0 : TC_BUG("%s (false) " msg, str, ##__VA_ARGS__); \
  })

#define TCMALLOC_CHECK_OP(c1, op, c2, cs1, cs2, msg, ...)                     \
  ({                                                                          \
    const auto& cc1 = (c1);                                                   \
    const auto& cc2 = (c2);                                                   \
    if (ABSL_PREDICT_FALSE(!(cc1 op cc2))) {                                  \
      TC_BUG("%s " #op " %s (%v " #op " %v) " msg, cs1, cs2,                  \
             tcmalloc::tcmalloc_internal::FormatConvert(cc1),                 \
             tcmalloc::tcmalloc_internal::FormatConvert(cc2), ##__VA_ARGS__); \
    }                                                                         \
    (void)0;                                                                  \
  })

// absl::SNPrintF rejects to print pointers with %v,
// so we need this little danse to convience it.
struct PtrFormatter {
  const volatile void* val;
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const PtrFormatter& p) {
    absl::Format(&sink, "%p", p.val);
  }
};

template <typename T>
PtrFormatter FormatConvert(T* v) {
  return PtrFormatter{v};
}

inline PtrFormatter FormatConvert(std::nullptr_t v) { return PtrFormatter{v}; }

template <typename T>
struct OptionalFormatter {
  const T* val;
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const OptionalFormatter<T>& p) {
    if (p.val != nullptr) {
      absl::Format(&sink, "%v", *p.val);
    } else {
      absl::Format(&sink, "???");
    }
  }
};

template <typename T>
OptionalFormatter<T> FormatConvert(const std::optional<T>& v) {
  return {v.has_value() ? &*v : nullptr};
}

template <typename T>
const T& FormatConvert(const T& v) {
  return v;
}

// Print into buffer
class Printer {
 private:
  char* buf_;     // Where should we write next
  size_t left_;   // Space left in buffer (including space for \0)
  size_t required_;  // Space we needed to complete all printf calls up to this
                     // point

 public:
  // REQUIRES: "length > 0"
  Printer(char* buf, size_t length) : buf_(buf), left_(length), required_(0) {
    TC_ASSERT_GT(length, 0);
    buf[0] = '\0';
  }

  template <typename... Args>
  void printf(const absl::FormatSpec<Args...>& format, const Args&... args) {
    TC_ASSERT_GE(left_, 0);
    if (left_ <= 0) {
      return;
    }
    AllocationGuard enforce_no_alloc;
    const int r = absl::SNPrintF(buf_, left_, format, args...);
    if (r < 0) {
      left_ = 0;
      return;
    }
    required_ += r;

    if (r > left_) {
      left_ = 0;
    } else {
      left_ -= r;
      buf_ += r;
    }
  }

  template <typename... Args>
  void Append(const Args&... args) {
    AllocationGuard enforce_no_alloc;
    AppendPieces({static_cast<const absl::AlphaNum&>(args).Piece()...});
  }

  size_t SpaceRequired() const { return required_; }

 private:
  void AppendPieces(std::initializer_list<absl::string_view> pieces) {
    TC_ASSERT_GE(left_, 0);
    if (left_ <= 0) {
      return;
    }

    size_t total_size = 0;
    for (const absl::string_view piece : pieces) total_size += piece.size();

    required_ += total_size;
    if (left_ < total_size) {
      left_ = 0;
      return;
    }

    for (const absl::string_view& piece : pieces) {
      const size_t this_size = piece.size();
      if (this_size == 0) {
        continue;
      }

      memcpy(buf_, piece.data(), this_size);
      buf_ += this_size;
    }

    left_ -= total_size;
  }
};

enum PbtxtRegionType { kTop, kNested };

// A helper class that prints pbtxt via RAII. A pbtxt region can be either a
// top region (with no brackets) or a nested region (enclosed by curly
// brackets).
class PbtxtRegion {
 public:
  PbtxtRegion(Printer* out, PbtxtRegionType type);
  ~PbtxtRegion();

  PbtxtRegion(const PbtxtRegion&) = delete;
  PbtxtRegion(PbtxtRegion&&) = default;

  // Prints 'key: value'.
  void PrintI64(absl::string_view key, int64_t value);
  void PrintDouble(absl::string_view key, double value);
  void PrintBool(absl::string_view key, bool value);
  // Useful for enums.
  void PrintRaw(absl::string_view key, absl::string_view value);

  // Prints 'key subregion'. Return the created subregion.
  PbtxtRegion CreateSubRegion(absl::string_view key);

 private:
  Printer* out_;
  PbtxtRegionType type_;
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_LOGGING_H_
