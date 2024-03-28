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

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "absl/base/attributes.h"
#include "absl/base/const_init.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/macros.h"
#include "absl/debugging/stacktrace.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/internal/allocation_guard.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/environment.h"
#include "tcmalloc/internal/parameter_accessors.h"

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

// If this failure occurs during "bazel test", writes a warning for Bazel to
// display.
static void RecordBazelWarning(absl::string_view type,
                               absl::string_view error) {
  constexpr absl::string_view kHeaderSuffix = " error detected: ";

  const char* warning_file = thread_safe_getenv("TEST_WARNINGS_OUTPUT_FILE");
  if (!warning_file) return;  // Not a bazel test.

  int fd = open(warning_file, O_CREAT | O_WRONLY | O_APPEND, 0644);
  if (fd == -1) return;
  (void)write(fd, type.data(), type.size());
  (void)write(fd, kHeaderSuffix.data(), kHeaderSuffix.size());
  (void)write(fd, error.data(), error.size());
  (void)write(fd, "\n", 1);
  close(fd);
}

// If this failure occurs during a gUnit test, writes an XML file describing the
// error type.  Note that we cannot use ::testing::Test::RecordProperty()
// because it doesn't write the XML file if a test crashes (which we're about to
// do here).  So we write directly to the XML file instead.
//
static void RecordTestFailure(absl::string_view detector,
                              absl::string_view error) {
  const char* xml_file = thread_safe_getenv("XML_OUTPUT_FILE");
  if (!xml_file) return;  // Not a gUnit test.

  // Record test failure for Sponge.
  constexpr absl::string_view kXmlHeaderPart1 =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<testsuites><testsuite><testcase>"
      "  <properties>"
      "    <property name=\"";
  constexpr absl::string_view kXmlHeaderPart2 = "-report\" value=\"";
  constexpr absl::string_view kXmlFooterPart1 =
      "\"/>"
      "  </properties>"
      "  <failure message=\"MemoryError\">"
      "    ";
  constexpr absl::string_view kXmlFooterPart2 =
      " detected a memory error.  See the test log for full report."
      "  </failure>"
      "</testcase></testsuite></testsuites>";

  int fd = open(xml_file, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd == -1) return;
  (void)write(fd, kXmlHeaderPart1.data(), kXmlHeaderPart1.size());
  for (char c : detector) {
    c = absl::ascii_tolower(c);
    (void)write(fd, &c, 1);
  }
  (void)write(fd, kXmlHeaderPart2.data(), kXmlHeaderPart2.size());
  (void)write(fd, error.data(), error.size());
  (void)write(fd, kXmlFooterPart1.data(), kXmlFooterPart1.size());
  (void)write(fd, detector.data(), detector.size());
  (void)write(fd, kXmlFooterPart2.data(), kXmlFooterPart2.size());
  close(fd);
}
//
// If this crash occurs in a test, records test failure summaries.
//
// detector is the bug detector or tools that found the error
// error contains the type of error to record.
void RecordCrash(absl::string_view detector, absl::string_view error) {
  TC_ASSERT(!detector.empty());
  TC_ASSERT(!error.empty());

  RecordBazelWarning(detector, error);
  RecordTestFailure(detector, error);
}

ABSL_ATTRIBUTE_NOINLINE
ABSL_ATTRIBUTE_NORETURN
static void Crash(const char* filename, int line, const char* msg,
                  size_t msglen, bool oom) {
  StackTrace trace;
  trace.depth = absl::GetStackTrace(trace.stack, kMaxStackDepth, 1);

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
    AllocationGuardSpinLockHolder l(&crash_lock);
    if (!crashed) {
      crashed = true;
      first_crash = true;
    }
  }

  (*log_message_writer)(msg, msglen);
  if (first_crash && oom) {
#ifndef __APPLE__
    if (&TCMalloc_Internal_GetStats != nullptr) {
      size_t n = TCMalloc_Internal_GetStats(stats_buffer, kStatsBufferSize);
      (*log_message_writer)(stats_buffer, std::min(n, kStatsBufferSize));
    }
#endif  // __APPLE__
  }

  abort();
}

ABSL_ATTRIBUTE_NORETURN void CheckFailed(const char* file, int line,
                                         const char* msg, int msglen) {
  Crash(file, line, msg, msglen, false);
}

void CrashWithOOM(size_t alloc_size) {
  char buf[512];
  int n = absl::SNPrintF(buf, sizeof(buf),
                         "Unable to allocate %zu (new failed)", alloc_size);
  Crash("tcmalloc", 0, buf, n, true);
}

void PrintStackTrace(void** stack_frames, size_t depth) {
  for (size_t i = 0; i < depth; ++i) {
    TC_LOG("  @  %p", stack_frames[i]);
  }
}

void PrintStackTraceFromSignalHandler(void* context) {
  void* stack_frames[kMaxStackDepth];
  size_t depth = absl::GetStackTraceWithContext(stack_frames, kMaxStackDepth,
  1,
                                                context, nullptr);
  PrintStackTrace(stack_frames, depth);
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
