/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Various simple compression/decompression functions. */

#ifndef mozilla_Compression_h_
#define mozilla_Compression_h_

#include "mozilla/Assertions.h"
#include "mozilla/Types.h"
#include "mozilla/ResultVariant.h"
#include "mozilla/Span.h"
#include "mozilla/UniquePtr.h"

struct LZ4F_cctx_s;  // compression context
struct LZ4F_dctx_s;  // decompression context

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

class LZ4 {
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
  static size_t compress(const char* aSource, size_t aInputSize, char* aDest);

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
  static size_t compressLimitedOutput(const char* aSource, size_t aInputSize,
                                      char* aDest, size_t aMaxOutputSize);

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
   * @return true on success, false on failure
   */
  [[nodiscard]] static bool decompress(const char* aSource, size_t aInputSize,
                                       char* aDest, size_t aMaxOutputSize,
                                       size_t* aOutputSize);

  /**
   * If the source stream is malformed, the function will stop decoding
   * and return false.
   *
   * This function never writes beyond aDest + aMaxOutputSize, and is
   * therefore protected against malicious data packets. It also ignores
   * unconsumed input upon reaching aMaxOutputSize and can therefore be used
   * for partial decompression.
   *
   * Note: Destination buffer must be already allocated.  This version is
   *       slightly slower than the decompress without the aMaxOutputSize.
   *
   * @param aInputSize is the length of the input compressed data
   * @param aMaxOutputSize is the size of the destination buffer (which must be
   *   already allocated)
   * @param aOutputSize the actual number of bytes decoded in the destination
   *   buffer (necessarily <= aMaxOutputSize)
   * @return true on success, false on failure
   */
  [[nodiscard]] static bool decompressPartial(const char* aSource,
                                              size_t aInputSize, char* aDest,
                                              size_t aMaxOutputSize,
                                              size_t* aOutputSize);

  /*
   * Provides the maximum size that LZ4 may output in a "worst case"
   * scenario (input data not compressible) primarily useful for memory
   * allocation of output buffer.
   * note : this function is limited by "int" range (2^31-1)
   *
   * @param aInputSize is the input size. Max supported value is ~1.9GB
   * @return maximum output size in a "worst case" scenario
   */
  static inline size_t maxCompressedSize(size_t aInputSize) {
    size_t max = (aInputSize + (aInputSize / 255) + 16);
    MOZ_ASSERT(max > aInputSize);
    return max;
  }
};

/**
 * Context for LZ4 Frame-based streaming compression. Use this if you
 * want to incrementally compress something or if you want to compress
 * something such that another application can read it.
 */
class LZ4FrameCompressionContext final {
 public:
  LZ4FrameCompressionContext(int aCompressionLevel, size_t aMaxSrcSize,
                             bool aChecksum, bool aStableSrc = false);

  ~LZ4FrameCompressionContext();

  size_t GetRequiredWriteBufferLength() { return mWriteBufLen; }

  /**
   * Begin streaming frame-based compression.
   *
   * @return a Result with a Span containing the frame header, or an lz4 error
   * code (size_t).
   */
  Result<Span<const char>, size_t> BeginCompressing(Span<char> aWriteBuffer);

  /**
   * Continue streaming frame-based compression with the provided input.
   *
   * @param aInput input buffer to be compressed.
   * @return a Result with a Span containing compressed output, or an lz4 error
   * code (size_t).
   */
  Result<Span<const char>, size_t> ContinueCompressing(Span<const char> aInput);

  /**
   * Finalize streaming frame-based compression with the provided input.
   *
   * @return a Result with a Span containing compressed output and the frame
   * footer, or an lz4 error code (size_t).
   */
  Result<Span<const char>, size_t> EndCompressing();

 private:
  LZ4F_cctx_s* mContext;
  int mCompressionLevel;
  bool mGenerateChecksum;
  bool mStableSrc;
  size_t mMaxSrcSize;
  size_t mWriteBufLen;
  Span<char> mWriteBuffer;
};

struct LZ4FrameDecompressionResult {
  size_t mSizeRead;
  size_t mSizeWritten;
  bool mFinished;
};

/**
 * Context for LZ4 Frame-based streaming decompression. Use this if you
 * want to decompress something compressed by LZ4FrameCompressionContext
 * or by another application.
 */
class LZ4FrameDecompressionContext final {
 public:
  explicit LZ4FrameDecompressionContext(bool aStableDest = false);
  ~LZ4FrameDecompressionContext();

  /**
   * Decompress a buffer/part of a buffer compressed with
   * LZ4FrameCompressionContext or another application.
   *
   * @param aOutput output buffer to be write results into.
   * @param aInput input buffer to be decompressed.
   * @return a Result with information on bytes read/written and whether we
   * completely decompressed the input into the output, or an lz4 error code
   * (size_t).
   */
  Result<LZ4FrameDecompressionResult, size_t> Decompress(
      Span<char> aOutput, Span<const char> aInput);

 private:
  LZ4F_dctx_s* mContext;
  bool mStableDest;
};

} /* namespace Compression */
} /* namespace mozilla */

#endif /* mozilla_Compression_h_ */
