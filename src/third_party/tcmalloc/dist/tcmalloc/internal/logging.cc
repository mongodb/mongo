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

#include "tcmalloc/internal/logging.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>

#include "absl/base/attributes.h"
#include "absl/base/const_init.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/macros.h"
#include "absl/debugging/stacktrace.h"
#include "tcmalloc/internal/parameter_accessors.h"
#include "tcmalloc/malloc_extension.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Variables for storing crash output.  Allocated statically since we
// may not be able to heap-allocate while crashing.
ABSL_CONST_INIT static absl::base_internal::SpinLock crash_lock(
    absl::kConstInit, absl::base_internal::SCHEDULE_KERNEL_ONLY);
static bool crashed = false;

static const size_t kStatsBufferSize = 16 << 10;
#ifndef __APPLE__
static char stats_buffer[kStatsBufferSize] = {0};
#endif  // __APPLE__

static void WriteMessage(const char* msg, int length) {
  (void)::write(STDERR_FILENO, msg, length);
}

void (*log_message_writer)(const char* msg, int length) = WriteMessage;

class Logger {
 public:
  bool Add(const LogItem& item);
  bool AddStr(const char* str, int n);
  bool AddNum(uint64_t num, int base);  // base must be 10 or 16.

  static constexpr int kBufSize = 512;
  char* p_;
  char* end_;
  char buf_[kBufSize];

  StackTrace trace;
};

static Logger FormatLog(bool with_stack, const char* filename, int line,
                        LogItem a, LogItem b, LogItem c, LogItem d, LogItem e,
                        LogItem f) {
  Logger state;
  state.p_ = state.buf_;
  state.end_ = state.buf_ + sizeof(state.buf_);
  // clang-format off
  state.AddStr(filename, strlen(filename)) &&
      state.AddStr(":", 1) &&
      state.AddNum(line, 10) &&
      state.AddStr("]", 1) &&
      state.Add(a) &&
      state.Add(b) &&
      state.Add(c) &&
      state.Add(d) &&
      state.Add(e) &&
      state.Add(f);
  // clang-format on

  if (with_stack) {
    state.trace.depth =
        absl::GetStackTrace(state.trace.stack, kMaxStackDepth, 1);
    state.Add(LogItem("@"));
    for (int i = 0; i < state.trace.depth; i++) {
      state.Add(LogItem(state.trace.stack[i]));
    }
  }

  // Teminate with newline
  if (state.p_ >= state.end_) {
    state.p_ = state.end_ - 1;
  }
  *state.p_ = '\n';
  state.p_++;

  return state;
}

ABSL_ATTRIBUTE_NOINLINE
void Log(LogMode mode, const char* filename, int line, LogItem a, LogItem b,
         LogItem c, LogItem d, LogItem e, LogItem f) {
  Logger state =
      FormatLog(mode == kLogWithStack, filename, line, a, b, c, d, e, f);
  int msglen = state.p_ - state.buf_;
  (*log_message_writer)(state.buf_, msglen);
}

ABSL_ATTRIBUTE_NOINLINE
void Crash(CrashMode mode, const char* filename, int line, LogItem a, LogItem b,
           LogItem c, LogItem d, LogItem e, LogItem f) {
  Logger state = FormatLog(true, filename, line, a, b, c, d, e, f);

  int msglen = state.p_ - state.buf_;

  // FailureSignalHandler mallocs for various logging attempts.
  // We might be crashing holding tcmalloc locks.
  // We're substantially less likely to try to take those locks
  // (and thus deadlock until the alarm timer fires) if we disable sampling.
#ifndef __APPLE__
  if (&TCMalloc_Internal_SetProfileSamplingRate != nullptr) {
    TCMalloc_Internal_SetProfileSamplingRate(0);
  }
#endif  // __APPLE__

  bool first_crash = false;
  {
    absl::base_internal::SpinLockHolder l(&crash_lock);
    if (!crashed) {
      crashed = true;
      first_crash = true;
    }
  }

  (*log_message_writer)(state.buf_, msglen);
  if (first_crash && mode == kCrashWithStats) {
#ifndef __APPLE__
    if (&TCMalloc_Internal_GetStats != nullptr) {
      size_t n = TCMalloc_Internal_GetStats(stats_buffer, kStatsBufferSize);
      (*log_message_writer)(stats_buffer, std::min(n, kStatsBufferSize));
    }
#endif  // __APPLE__
  }

  abort();
}

bool Logger::Add(const LogItem& item) {
  // Separate real items with spaces
  if (item.tag_ != LogItem::kEnd && p_ < end_) {
    *p_ = ' ';
    p_++;
  }

  switch (item.tag_) {
    case LogItem::kStr:
      return AddStr(item.u_.str, strlen(item.u_.str));
    case LogItem::kUnsigned:
      return AddNum(item.u_.unum, 10);
    case LogItem::kSigned:
      if (item.u_.snum < 0) {
        // The cast to uint64_t is intentionally before the negation
        // so that we do not attempt to negate -2^63.
        return AddStr("-", 1) &&
               AddNum(-static_cast<uint64_t>(item.u_.snum), 10);
      } else {
        return AddNum(static_cast<uint64_t>(item.u_.snum), 10);
      }
    case LogItem::kPtr:
      return AddStr("0x", 2) &&
             AddNum(reinterpret_cast<uintptr_t>(item.u_.ptr), 16);
    default:
      return false;
  }
}

bool Logger::AddStr(const char* str, int n) {
  ptrdiff_t remaining = end_ - p_;
  if (remaining < n) {
    // Try to log a truncated message if there is some space.
    static constexpr absl::string_view kDots = "...";
    if (remaining > kDots.size() + 1) {
      int truncated = remaining - kDots.size();
      memcpy(p_, str, truncated);
      p_ += truncated;
      memcpy(p_, kDots.data(), kDots.size());
      p_ += kDots.size();

      return true;
    }
    return false;
  } else {
    memcpy(p_, str, n);
    p_ += n;
    return true;
  }
}

bool Logger::AddNum(uint64_t num, int base) {
  static const char kDigits[] = "0123456789abcdef";
  char space[22];  // more than enough for 2^64 in smallest supported base (10)
  char* end = space + sizeof(space);
  char* pos = end;
  do {
    pos--;
    *pos = kDigits[num % base];
    num /= base;
  } while (num > 0 && pos > space);
  return AddStr(pos, end - pos);
}

PbtxtRegion::PbtxtRegion(Printer* out, PbtxtRegionType type)
    : out_(out), type_(type) {
  switch (type_) {
    case kTop:
      break;
    case kNested:
      out_->Append("{");
      break;
  }
}

PbtxtRegion::~PbtxtRegion() {
  switch (type_) {
    case kTop:
      break;
    case kNested:
      out_->Append("}");
      break;
  }
}

void PbtxtRegion::PrintI64(absl::string_view key, int64_t value) {
  out_->Append(" ", key, ": ", value);
}

void PbtxtRegion::PrintDouble(absl::string_view key, double value) {
  out_->Append(" ", key, ": ", value);
}

void PbtxtRegion::PrintBool(absl::string_view key, bool value) {
  out_->Append(" ", key, value ? ": true" : ": false");
}

void PbtxtRegion::PrintRaw(absl::string_view key, absl::string_view value) {
  out_->Append(" ", key, ": ", value);
}

PbtxtRegion PbtxtRegion::CreateSubRegion(absl::string_view key) {
  out_->Append(" ", key, " ");
  PbtxtRegion sub(out_, kNested);
  return sub;
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
