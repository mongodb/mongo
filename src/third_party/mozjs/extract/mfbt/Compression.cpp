/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Compression.h"
#include "mozilla/CheckedInt.h"

// Without including <string>, MSVC 2015 complains about e.g. the impossibility
// to convert `const void* const` to `void*` when calling memchr from
// corecrt_memory.h.
#include <string>

#include "lz4/lz4.h"
#include "lz4/lz4frame.h"

using namespace mozilla;
using namespace mozilla::Compression;

/* Our wrappers */

size_t LZ4::compress(const char* aSource, size_t aInputSize, char* aDest) {
  CheckedInt<int> inputSizeChecked = aInputSize;
  MOZ_ASSERT(inputSizeChecked.isValid());
  return LZ4_compress_default(aSource, aDest, inputSizeChecked.value(),
                              LZ4_compressBound(inputSizeChecked.value()));
}

size_t LZ4::compressLimitedOutput(const char* aSource, size_t aInputSize,
                                  char* aDest, size_t aMaxOutputSize) {
  CheckedInt<int> inputSizeChecked = aInputSize;
  MOZ_ASSERT(inputSizeChecked.isValid());
  CheckedInt<int> maxOutputSizeChecked = aMaxOutputSize;
  MOZ_ASSERT(maxOutputSizeChecked.isValid());
  return LZ4_compress_default(aSource, aDest, inputSizeChecked.value(),
                              maxOutputSizeChecked.value());
}

bool LZ4::decompress(const char* aSource, size_t aInputSize, char* aDest,
                     size_t aMaxOutputSize, size_t* aOutputSize) {
  CheckedInt<int> maxOutputSizeChecked = aMaxOutputSize;
  MOZ_ASSERT(maxOutputSizeChecked.isValid());
  CheckedInt<int> inputSizeChecked = aInputSize;
  MOZ_ASSERT(inputSizeChecked.isValid());

  int ret = LZ4_decompress_safe(aSource, aDest, inputSizeChecked.value(),
                                maxOutputSizeChecked.value());
  if (ret >= 0) {
    *aOutputSize = ret;
    return true;
  }

  *aOutputSize = 0;
  return false;
}

bool LZ4::decompressPartial(const char* aSource, size_t aInputSize, char* aDest,
                            size_t aMaxOutputSize, size_t* aOutputSize) {
  CheckedInt<int> maxOutputSizeChecked = aMaxOutputSize;
  MOZ_ASSERT(maxOutputSizeChecked.isValid());
  CheckedInt<int> inputSizeChecked = aInputSize;
  MOZ_ASSERT(inputSizeChecked.isValid());

  int ret = LZ4_decompress_safe_partial(
      aSource, aDest, inputSizeChecked.value(), maxOutputSizeChecked.value(),
      maxOutputSizeChecked.value());
  if (ret >= 0) {
    *aOutputSize = ret;
    return true;
  }

  *aOutputSize = 0;
  return false;
}

LZ4FrameCompressionContext::LZ4FrameCompressionContext(int aCompressionLevel,
                                                       size_t aMaxSrcSize,
                                                       bool aChecksum,
                                                       bool aStableSrc)
    : mContext(nullptr),
      mCompressionLevel(aCompressionLevel),
      mGenerateChecksum(aChecksum),
      mStableSrc(aStableSrc),
      mMaxSrcSize(aMaxSrcSize),
      mWriteBufLen(0) {
  LZ4F_contentChecksum_t checksum =
      mGenerateChecksum ? LZ4F_contentChecksumEnabled : LZ4F_noContentChecksum;
  LZ4F_preferences_t prefs = {
      {
          LZ4F_max256KB,
          LZ4F_blockLinked,
          checksum,
      },
      mCompressionLevel,
  };
  mWriteBufLen = LZ4F_compressBound(mMaxSrcSize, &prefs);
  LZ4F_errorCode_t err = LZ4F_createCompressionContext(&mContext, LZ4F_VERSION);
  MOZ_RELEASE_ASSERT(!LZ4F_isError(err));
}

LZ4FrameCompressionContext::~LZ4FrameCompressionContext() {
  LZ4F_freeCompressionContext(mContext);
}

Result<Span<const char>, size_t> LZ4FrameCompressionContext::BeginCompressing(
    Span<char> aWriteBuffer) {
  mWriteBuffer = aWriteBuffer;
  LZ4F_contentChecksum_t checksum =
      mGenerateChecksum ? LZ4F_contentChecksumEnabled : LZ4F_noContentChecksum;
  LZ4F_preferences_t prefs = {
      {
          LZ4F_max256KB,
          LZ4F_blockLinked,
          checksum,
      },
      mCompressionLevel,
  };
  size_t headerSize = LZ4F_compressBegin(mContext, mWriteBuffer.Elements(),
                                         mWriteBufLen, &prefs);
  if (LZ4F_isError(headerSize)) {
    return Err(headerSize);
  }

  return Span{static_cast<const char*>(mWriteBuffer.Elements()), headerSize};
}

Result<Span<const char>, size_t>
LZ4FrameCompressionContext::ContinueCompressing(Span<const char> aInput) {
  LZ4F_compressOptions_t opts = {};
  opts.stableSrc = (uint32_t)mStableSrc;
  size_t outputSize =
      LZ4F_compressUpdate(mContext, mWriteBuffer.Elements(), mWriteBufLen,
                          aInput.Elements(), aInput.Length(), &opts);
  if (LZ4F_isError(outputSize)) {
    return Err(outputSize);
  }

  return Span{static_cast<const char*>(mWriteBuffer.Elements()), outputSize};
}

Result<Span<const char>, size_t> LZ4FrameCompressionContext::EndCompressing() {
  size_t outputSize =
      LZ4F_compressEnd(mContext, mWriteBuffer.Elements(), mWriteBufLen,
                       /* options */ nullptr);
  if (LZ4F_isError(outputSize)) {
    return Err(outputSize);
  }

  return Span{static_cast<const char*>(mWriteBuffer.Elements()), outputSize};
}

LZ4FrameDecompressionContext::LZ4FrameDecompressionContext(bool aStableDest)
    : mContext(nullptr), mStableDest(aStableDest) {
  LZ4F_errorCode_t err =
      LZ4F_createDecompressionContext(&mContext, LZ4F_VERSION);
  MOZ_RELEASE_ASSERT(!LZ4F_isError(err));
}

LZ4FrameDecompressionContext::~LZ4FrameDecompressionContext() {
  LZ4F_freeDecompressionContext(mContext);
}

Result<LZ4FrameDecompressionResult, size_t>
LZ4FrameDecompressionContext::Decompress(Span<char> aOutput,
                                         Span<const char> aInput) {
  LZ4F_decompressOptions_t opts = {};
  opts.stableDst = (uint32_t)mStableDest;
  size_t outBytes = aOutput.Length();
  size_t inBytes = aInput.Length();
  size_t result = LZ4F_decompress(mContext, aOutput.Elements(), &outBytes,
                                  aInput.Elements(), &inBytes, &opts);
  if (LZ4F_isError(result)) {
    return Err(result);
  }

  LZ4FrameDecompressionResult decompressionResult = {};
  decompressionResult.mFinished = !result;
  decompressionResult.mSizeRead = inBytes;
  decompressionResult.mSizeWritten = outBytes;
  return decompressionResult;
}
