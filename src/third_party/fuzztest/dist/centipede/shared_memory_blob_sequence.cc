// Copyright 2022 The Centipede Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "./centipede/shared_memory_blob_sequence.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "absl/base/nullability.h"

namespace fuzztest::internal {

// TODO(ussuri): Refactor `char *` into a `string_view`.
static void ErrorOnFailure(bool condition, const char *absl_nonnull text) {
  if (!condition) return;
  std::perror(text);
  abort();
}

BlobSequence::BlobSequence(uint8_t *data, size_t size)
    : data_(data), size_(size) {
  ErrorOnFailure(size < sizeof(Blob::size), "Size too small");
}

bool BlobSequence::Write(Blob blob) {
  ErrorOnFailure(!blob.IsValid(), "Write(): blob.tag must not be zero");
  ErrorOnFailure(had_reads_after_reset_, "Write(): Had reads after reset");
  had_writes_after_reset_ = true;
  if (const auto available_size = size_ - offset_;
      available_size < sizeof(blob.size) + sizeof(blob.tag) ||
      available_size - sizeof(blob.size) - sizeof(blob.tag) < blob.size) {
    return false;
  }

  // Write tag.
  memcpy(data_ + offset_, &blob.tag, sizeof(blob.tag));
  offset_ += sizeof(blob.tag);

  // Write size.
  memcpy(data_ + offset_, &blob.size, sizeof(blob.size));
  offset_ += sizeof(blob.size);
  // Write data.
  if (blob.data != nullptr) {
      memcpy(data_ + offset_, blob.data, blob.size);
  }

  offset_ += blob.size;
  if (offset_ + sizeof(blob.size) + sizeof(blob.tag) <= size_) {
    // Write zero tag/size to data_+offset_ but don't change the offset.
    // This is required to overwrite any stale bits in data_.
    Blob invalid_blob;  // invalid.
    memcpy(data_ + offset_, &invalid_blob.tag, sizeof(invalid_blob.tag));
    memcpy(data_ + offset_ + sizeof(invalid_blob.tag), &invalid_blob.size,
           sizeof(invalid_blob.size));
  }
  return true;
}

Blob BlobSequence::Read() {
  ErrorOnFailure(had_writes_after_reset_, "Had writes after reset");
  had_reads_after_reset_ = true;
  if (offset_ + sizeof(Blob::size) + sizeof(Blob::tag) > size_) return {};
  size_t offset = offset_;
  // Read blob_tag.
  Blob::SizeAndTagT blob_tag = 0;
  memcpy(&blob_tag, data_ + offset, sizeof(blob_tag));
  offset += sizeof(blob_tag);
  // Read blob_size.
  Blob::SizeAndTagT blob_size = 0;
  memcpy(&blob_size, data_ + offset, sizeof(Blob::size));
  offset += sizeof(Blob::size);
  // Read blob_data.
  if (size_ - offset < blob_size) {
    std::perror("Read(): not enough bytes");
    return {};
  }
  if (blob_tag == 0 && blob_size == 0) return {};
  if (blob_tag == 0) {
    std::perror("Read(): blob.tag must not be zero");
    return {};
  }
  offset_ = offset + blob_size;
  return Blob{blob_tag, blob_size, data_ + offset};
}

void BlobSequence::Reset() {
  offset_ = 0;
  had_reads_after_reset_ = false;
  had_writes_after_reset_ = false;
}

SharedMemoryBlobSequence::SharedMemoryBlobSequence(const char *name,
                                                   size_t size,
                                                   bool use_posix_shmem) {
  ErrorOnFailure(size < sizeof(Blob::size), "Size too small");
  size_ = size;
  if (use_posix_shmem) {
    fd_ = shm_open(name, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
    ErrorOnFailure(fd_ < 0, "shm_open() failed");
    strncpy(path_, name, PATH_MAX);
    ErrorOnFailure(path_[PATH_MAX - 1] != 0,
                   "shm_open() path length exceeds PATH_MAX.");
    path_is_owned_ = true;
  } else {
#ifdef __APPLE__
    ErrorOnFailure(true, "must use POSIX shmem");
#else   // __APPLE__
    fd_ = memfd_create(name, MFD_CLOEXEC);
    ErrorOnFailure(fd_ < 0, "memfd_create() failed");
    const size_t path_size =
        snprintf(path_, PATH_MAX, "/proc/%d/fd/%d", getpid(), fd_);
    ErrorOnFailure(path_size >= PATH_MAX,
                   "internal fd path length exceeds PATH_MAX.");
    // memfd_create descriptors are automatically freed on close().
    path_is_owned_ = false;
#endif  // __APPLE__
  }
  ErrorOnFailure(ftruncate(fd_, static_cast<off_t>(size_)),
                 "ftruncate() failed)");
  MmapData();
}

SharedMemoryBlobSequence::SharedMemoryBlobSequence(const char *path) {
  // This is a quick way to tell shm-allocated paths from memfd paths without
  // requiring the caller to specify.
  if (strncmp(path, "/proc/", 6) == 0) {
    fd_ = open(path, O_RDWR, O_CLOEXEC);
  } else {
    fd_ = shm_open(path, O_RDWR, 0);
  }
  ErrorOnFailure(fd_ < 0, "open() failed");
  strncpy(path_, path, PATH_MAX);
  ErrorOnFailure(path_[PATH_MAX - 1] != 0, "path length exceeds PATH_MAX.");
  struct stat statbuf = {};
  ErrorOnFailure(fstat(fd_, &statbuf), "fstat() failed");
  size_ = statbuf.st_size;
  MmapData();
}

void SharedMemoryBlobSequence::MmapData() {
  data_ = static_cast<uint8_t *>(
      mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
  ErrorOnFailure(data_ == MAP_FAILED, "mmap() failed");
}

SharedMemoryBlobSequence::~SharedMemoryBlobSequence() {
  if (path_is_owned_) {
    ErrorOnFailure(shm_unlink(path_), "shm_unlink() failed");
  }
  ErrorOnFailure(munmap(data_, size_), "munmap() failed");
  ErrorOnFailure(close(fd_), "close() failed");
}

void SharedMemoryBlobSequence::ReleaseSharedMemory() {
#ifdef __APPLE__
  // MacOS only allows ftruncate shm once
  // (https://stackoverflow.com/questions/25502229/ftruncate-not-working-on-posix-shared-memory-in-mac-os-x).
  // So nothing we can do here.
#else   // __APPLE__
  // Setting size to 0 releases the memory to OS.
  ErrorOnFailure(ftruncate(fd_, 0) != 0, "ftruncate(0) failed)");
  // Set the size back to `size`. The memory is not actually reserved.
  ErrorOnFailure(ftruncate(fd_, size_) != 0, "ftruncate(size_) failed)");
#endif  // __APPLE__
}

size_t SharedMemoryBlobSequence::NumBytesUsed() const {
  struct stat statbuf;
  ErrorOnFailure(fstat(fd_, &statbuf), "fstat() failed)");
  return statbuf.st_blocks * S_BLKSIZE;
}

}  // namespace fuzztest::internal
