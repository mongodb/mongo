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

#include "tcmalloc/internal/proc_maps.h"

#include <fcntl.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "absl/strings/str_format.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/util.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

ProcMapsIterator::ProcMapsIterator(pid_t pid, Buffer* buffer) {
  if (pid == 0) {
    pid = getpid();
  }

  pid_ = pid;

  ibuf_ = buffer->buf;

  stext_ = etext_ = nextline_ = ibuf_;
  ebuf_ = ibuf_ + Buffer::kBufSize - 1;
  nextline_ = ibuf_;

#if defined(__linux__)
  // /maps exists in two places: /proc/pid/ and /proc/pid/task/tid (for each
  // thread in the process.)  The only difference between these is the "global"
  // view (/proc/pid/maps) attempts to label each VMA which is the stack of a
  // thread.  This is nice to have, but not critical, and scales quadratically.
  // Use the main thread's "local" view to ensure adequate performance.
  int path_length = absl::SNPrintF(ibuf_, Buffer::kBufSize,
                                   "/proc/%d/task/%d/maps", pid, pid);
  TC_CHECK_LT(path_length, Buffer::kBufSize);

  // No error logging since this can be called from the crash dump
  // handler at awkward moments. Users should call Valid() before
  // using.
  TCMALLOC_RETRY_ON_TEMP_FAILURE(fd_ = open(ibuf_, O_RDONLY));
#else
  fd_ = -1;  // so Valid() is always false
#endif
}

ProcMapsIterator::~ProcMapsIterator() {
  // As it turns out, Linux guarantees that close() does in fact close a file
  // descriptor even when the return value is EINTR. According to the notes in
  // the manpage for close(2), this is widespread yet not fully portable, which
  // is unfortunate. POSIX explicitly leaves this behavior as unspecified.
  if (fd_ >= 0) close(fd_);
}

bool ProcMapsIterator::Valid() const { return fd_ != -1; }

bool ProcMapsIterator::NextExt(uint64_t* start, uint64_t* end, char** flags,
                               uint64_t* offset, int64_t* inode,
                               char** filename, dev_t* dev) {
#if defined __linux__
  do {
    // Advance to the start of the next line
    stext_ = nextline_;

    // See if we have a complete line in the buffer already
    nextline_ = static_cast<char*>(memchr(stext_, '\n', etext_ - stext_));
    if (!nextline_) {
      // Shift/fill the buffer so we do have a line
      int count = etext_ - stext_;

      // Move the current text to the start of the buffer
      memmove(ibuf_, stext_, count);
      stext_ = ibuf_;
      etext_ = ibuf_ + count;

      int nread = 0;  // fill up buffer with text
      while (etext_ < ebuf_) {
        TCMALLOC_RETRY_ON_TEMP_FAILURE(nread =
                                           read(fd_, etext_, ebuf_ - etext_));
        if (nread > 0)
          etext_ += nread;
        else
          break;
      }

      // Zero out remaining characters in buffer at EOF to avoid returning
      // garbage from subsequent calls.
      if (etext_ != ebuf_ && nread == 0) {
        memset(etext_, 0, ebuf_ - etext_);
      }
      *etext_ = '\n';  // sentinel; safe because ibuf extends 1 char beyond ebuf
      nextline_ = static_cast<char*>(memchr(stext_, '\n', etext_ + 1 - stext_));
    }
    *nextline_ = 0;                               // turn newline into nul
    nextline_ += ((nextline_ < etext_) ? 1 : 0);  // skip nul if not end of text
    // stext_ now points at a nul-terminated line
    unsigned long long tmpstart, tmpend, tmpoffset;           // NOLINT
    long long tmpinode, local_inode;                          // NOLINT
    unsigned long long local_start, local_end, local_offset;  // NOLINT
    int major, minor;
    unsigned filename_offset = 0;
    // for now, assume all linuxes have the same format
    int para_num =
        sscanf(stext_, "%llx-%llx %4s %llx %x:%x %lld %n",
               start ? &local_start : &tmpstart, end ? &local_end : &tmpend,
               flags_, offset ? &local_offset : &tmpoffset, &major, &minor,
               inode ? &local_inode : &tmpinode, &filename_offset);

    if (para_num != 7) continue;

    if (start) *start = local_start;
    if (end) *end = local_end;
    if (offset) *offset = local_offset;
    if (inode) *inode = local_inode;
    // Depending on the Linux kernel being used, there may or may not be a space
    // after the inode if there is no filename.  sscanf will in such situations
    // nondeterministically either fill in filename_offset or not (the results
    // differ on multiple calls in the same run even with identical arguments).
    // We don't want to wander off somewhere beyond the end of the string.
    size_t stext_length = strlen(stext_);
    if (filename_offset == 0 || filename_offset > stext_length)
      filename_offset = stext_length;

    // We found an entry
    if (flags) *flags = flags_;
    if (filename) *filename = stext_ + filename_offset;
    if (dev) *dev = makedev(major, minor);

    return true;
  } while (etext_ > ibuf_);
#endif

  // We didn't find anything
  return false;
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
