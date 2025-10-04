/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Compression_h
#define vm_Compression_h

#include <zlib.h>

#include "jstypes.h"

#include "js/AllocPolicy.h"
#include "js/Vector.h"

namespace js {

struct CompressedDataHeader {
  uint32_t compressedBytes;
};

class Compressor {
 public:
  // After compressing CHUNK_SIZE bytes, we will do a full flush so we can
  // start decompression at that point.
  static constexpr size_t CHUNK_SIZE = 64 * 1024;

 private:
  // Number of bytes we should hand to zlib each compressMore() call.
  static constexpr size_t MAX_INPUT_SIZE = 2 * 1024;

  z_stream zs;
  const unsigned char* inp;
  size_t inplen;
  size_t outbytes;
  bool initialized;
  bool finished;

  // The number of uncompressed bytes written for the current chunk. When this
  // reaches CHUNK_SIZE, we finish the current chunk and start a new chunk.
  uint32_t currentChunkSize;

  // At the end of each chunk (and the end of the uncompressed data if it's
  // not a chunk boundary), we record the offset in the compressed data.
  js::Vector<uint32_t, 8, SystemAllocPolicy> chunkOffsets;

 public:
  enum Status { MOREOUTPUT, DONE, CONTINUE, OOM };

  Compressor(const unsigned char* inp, size_t inplen);
  ~Compressor();
  bool init();
  void setOutput(unsigned char* out, size_t outlen);
  /* Compress some of the input. Return true if it should be called again. */
  Status compressMore();
  size_t sizeOfChunkOffsets() const {
    return chunkOffsets.length() * sizeof(chunkOffsets[0]);
  }

  // Returns the number of bytes needed to store the data currently written +
  // the chunk offsets.
  size_t totalBytesNeeded() const;

  // Append the chunk offsets to |dest|.
  void finish(char* dest, size_t destBytes);

  static void rangeToChunkAndOffset(size_t uncompressedStart,
                                    size_t uncompressedLimit,
                                    size_t* firstChunk,
                                    size_t* firstChunkOffset,
                                    size_t* firstChunkSize, size_t* lastChunk,
                                    size_t* lastChunkSize) {
    *firstChunk = uncompressedStart / CHUNK_SIZE;
    *firstChunkOffset = uncompressedStart % CHUNK_SIZE;
    *firstChunkSize = CHUNK_SIZE - *firstChunkOffset;

    MOZ_ASSERT(uncompressedStart < uncompressedLimit,
               "subtraction below requires a non-empty range");

    *lastChunk = (uncompressedLimit - 1) / CHUNK_SIZE;
    *lastChunkSize = ((uncompressedLimit - 1) % CHUNK_SIZE) + 1;
  }

  static size_t chunkSize(size_t uncompressedBytes, size_t chunk) {
    MOZ_ASSERT(uncompressedBytes > 0, "must have uncompressed data to chunk");

    size_t startOfChunkBytes = chunk * CHUNK_SIZE;
    MOZ_ASSERT(startOfChunkBytes < uncompressedBytes,
               "chunk must refer to bytes not exceeding "
               "|uncompressedBytes|");

    size_t remaining = uncompressedBytes - startOfChunkBytes;
    return remaining < CHUNK_SIZE ? remaining : CHUNK_SIZE;
  }
};

/*
 * Decompress a string. The caller must know the length of the output and
 * allocate |out| to a string of that length.
 */
bool DecompressString(const unsigned char* inp, size_t inplen,
                      unsigned char* out, size_t outlen);

/*
 * Decompress a single chunk of at most Compressor::CHUNK_SIZE bytes.
 * |chunk| is the chunk index. The caller must know the length of the output
 * (the uncompressed chunk) and allocate |out| to a string of that length.
 */
bool DecompressStringChunk(const unsigned char* inp, size_t chunk,
                           unsigned char* out, size_t outlen);

} /* namespace js */

#endif /* vm_Compression_h */
