/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Various simple compression/decompression functions. */

#ifndef mozilla_Compression_h_
#define mozilla_Compression_h_

#include "mozilla/Types.h"
#include "mozilla/Assertions.h"

namespace mozilla {
namespace Compression {

/**
 * LZ4 is a very fast byte-wise compression algorithm.
 *
 * Compared to Google's Snappy it is faster to compress and decompress and
 * generally produces output of about the same size.
 *
 * Compared to zlib it compresses at about 10x the speed, decompresses at about
 * 4x the speed and produces output of about 1.5x the size.
 */

class LZ4
{
public:
  /**
   * Compresses |aInputSize| bytes from |aSource| into |aDest|. Destination
   * buffer must be already allocated, and must be sized to handle worst cases
   * situations (input data not compressible). Worst case size evaluation is
   * provided by function maxCompressedSize()
   *
   * @param aInputSize is the input size. Max supported value is ~1.9GB
   * @return the number of bytes written in buffer |aDest|
   */
  static MFBT_API size_t
  compress(const char* aSource, size_t aInputSize, char* aDest);

  /**
   * Compress |aInputSize| bytes from |aSource| into an output buffer
   * |aDest| of maximum size |aMaxOutputSize|.  If it cannot achieve it,
   * compression will stop, and result of the function will be zero,
   * |aDest| will still be written to, but since the number of input
   * bytes consumed is not returned the result is not usable.
   *
   * This function never writes outside of provided output buffer.
   *
   * @param aInputSize is the input size. Max supported value is ~1.9GB
   * @param aMaxOutputSize is the size of the destination buffer (which must
   *   be already allocated)
   * @return the number of bytes written in buffer |aDest| or 0 if the
   *   compression fails
   */
  static MFBT_API size_t
  compressLimitedOutput(const char* aSource, size_t aInputSize, char* aDest,
                        size_t aMaxOutputSize);

  /**
   * If the source stream is malformed, the function will stop decoding
   * and return a negative result, indicating the byte position of the
   * faulty instruction
   *
   * This function never writes outside of provided buffers, and never
   * modifies input buffer.
   *
   * Note: destination buffer must be already allocated, and its size must be a
   *       minimum of |aOutputSize| bytes.
   *
   * @param aOutputSize is the output size, therefore the original size
   * @return the number of bytes read in the source buffer
   */
  static MFBT_API bool
  decompress(const char* aSource, char* aDest, size_t aOutputSize);

  /**
   * If the source stream is malformed, the function will stop decoding
   * and return false.
   *
   * This function never writes beyond aDest + aMaxOutputSize, and is
   * therefore protected against malicious data packets.
   *
   * Note: Destination buffer must be already allocated.  This version is
   *       slightly slower than the decompress without the aMaxOutputSize.
   *
   * @param aInputSize is the length of the input compressed data
   * @param aMaxOutputSize is the size of the destination buffer (which must be
   *   already allocated)
   * @param aOutputSize the actual number of bytes decoded in the destination
   *   buffer (necessarily <= aMaxOutputSize)
   */
  static MFBT_API bool
  decompress(const char* aSource, size_t aInputSize, char* aDest,
             size_t aMaxOutputSize, size_t* aOutputSize);

  /*
   * Provides the maximum size that LZ4 may output in a "worst case"
   * scenario (input data not compressible) primarily useful for memory
   * allocation of output buffer.
   * note : this function is limited by "int" range (2^31-1)
   *
   * @param aInputSize is the input size. Max supported value is ~1.9GB
   * @return maximum output size in a "worst case" scenario
   */
  static inline size_t maxCompressedSize(size_t aInputSize)
  {
    size_t max = (aInputSize + (aInputSize / 255) + 16);
    MOZ_ASSERT(max > aInputSize);
    return max;
  }
};

} /* namespace Compression */
} /* namespace mozilla */

#endif /* mozilla_Compression_h_ */
