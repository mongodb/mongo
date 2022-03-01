/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsConverterInputStream.h"
#include "nsIInputStream.h"
#include "nsReadLine.h"
#include "nsStreamUtils.h"
#include <algorithm>
#include "mozilla/Unused.h"

using namespace mozilla;

#define CONVERTER_BUFFER_SIZE 8192

NS_IMPL_ISUPPORTS(nsConverterInputStream, nsIConverterInputStream,
                  nsIUnicharInputStream, nsIUnicharLineInputStream)

NS_IMETHODIMP
nsConverterInputStream::Init(nsIInputStream* aStream, const char* aCharset,
                             int32_t aBufferSize, char16_t aReplacementChar) {
  nsAutoCString label;
  if (!aCharset) {
    label.AssignLiteral("UTF-8");
  } else {
    label = aCharset;
  }

  auto encoding = Encoding::ForLabelNoReplacement(label);
  if (!encoding) {
    return NS_ERROR_UCONV_NOCONV;
  }
  // Previously, the implementation auto-switched only
  // between the two UTF-16 variants and only when
  // initialized with an endianness-unspecific label.
  mConverter = encoding->NewDecoder();

  size_t outputBufferSize;
  if (aBufferSize <= 0) {
    aBufferSize = CONVERTER_BUFFER_SIZE;
    outputBufferSize = CONVERTER_BUFFER_SIZE;
  } else {
    // NetUtil.jsm assumes that if buffer size equals
    // the input size, the whole stream will be processed
    // as one readString. This is not true with encoding_rs,
    // because encoding_rs might want to see space for a
    // surrogate pair, so let's compute a larger output
    // buffer length.
    CheckedInt<size_t> needed = mConverter->MaxUTF16BufferLength(aBufferSize);
    if (!needed.isValid()) {
      return NS_ERROR_OUT_OF_MEMORY;
    }
    outputBufferSize = needed.value();
  }

  // set up our buffers.
  if (!mByteData.SetCapacity(aBufferSize, mozilla::fallible) ||
      !mUnicharData.SetLength(outputBufferSize, mozilla::fallible)) {
    return NS_ERROR_OUT_OF_MEMORY;
  }

  mInput = aStream;
  mErrorsAreFatal = !aReplacementChar;
  return NS_OK;
}

NS_IMETHODIMP
nsConverterInputStream::Close() {
  nsresult rv = mInput ? mInput->Close() : NS_OK;
  mLineBuffer = nullptr;
  mInput = nullptr;
  mConverter = nullptr;
  mByteData.Clear();
  mUnicharData.Clear();
  return rv;
}

NS_IMETHODIMP
nsConverterInputStream::Read(char16_t* aBuf, uint32_t aCount,
                             uint32_t* aReadCount) {
  NS_ASSERTION(mUnicharDataLength >= mUnicharDataOffset, "unsigned madness");
  uint32_t readCount = mUnicharDataLength - mUnicharDataOffset;
  if (0 == readCount) {
    // Fill the unichar buffer
    readCount = Fill(&mLastErrorCode);
    if (readCount == 0) {
      *aReadCount = 0;
      return mLastErrorCode;
    }
  }
  if (readCount > aCount) {
    readCount = aCount;
  }
  memcpy(aBuf, mUnicharData.Elements() + mUnicharDataOffset,
         readCount * sizeof(char16_t));
  mUnicharDataOffset += readCount;
  *aReadCount = readCount;
  return NS_OK;
}

NS_IMETHODIMP
nsConverterInputStream::ReadSegments(nsWriteUnicharSegmentFun aWriter,
                                     void* aClosure, uint32_t aCount,
                                     uint32_t* aReadCount) {
  NS_ASSERTION(mUnicharDataLength >= mUnicharDataOffset, "unsigned madness");
  uint32_t bytesToWrite = mUnicharDataLength - mUnicharDataOffset;
  nsresult rv;
  if (0 == bytesToWrite) {
    // Fill the unichar buffer
    bytesToWrite = Fill(&rv);
    if (bytesToWrite <= 0) {
      *aReadCount = 0;
      return rv;
    }
    if (NS_FAILED(rv)) {
      return rv;
    }
  }

  if (bytesToWrite > aCount) bytesToWrite = aCount;

  uint32_t bytesWritten;
  uint32_t totalBytesWritten = 0;

  while (bytesToWrite) {
    rv = aWriter(this, aClosure, mUnicharData.Elements() + mUnicharDataOffset,
                 totalBytesWritten, bytesToWrite, &bytesWritten);
    if (NS_FAILED(rv)) {
      // don't propagate errors to the caller
      break;
    }

    bytesToWrite -= bytesWritten;
    totalBytesWritten += bytesWritten;
    mUnicharDataOffset += bytesWritten;
  }

  *aReadCount = totalBytesWritten;

  return NS_OK;
}

NS_IMETHODIMP
nsConverterInputStream::ReadString(uint32_t aCount, nsAString& aString,
                                   uint32_t* aReadCount) {
  NS_ASSERTION(mUnicharDataLength >= mUnicharDataOffset, "unsigned madness");
  uint32_t readCount = mUnicharDataLength - mUnicharDataOffset;
  if (0 == readCount) {
    // Fill the unichar buffer
    readCount = Fill(&mLastErrorCode);
    if (readCount == 0) {
      *aReadCount = 0;
      return mLastErrorCode;
    }
  }
  if (readCount > aCount) {
    readCount = aCount;
  }
  const char16_t* buf = mUnicharData.Elements() + mUnicharDataOffset;
  aString.Assign(buf, readCount);
  mUnicharDataOffset += readCount;
  *aReadCount = readCount;
  return NS_OK;
}

uint32_t nsConverterInputStream::Fill(nsresult* aErrorCode) {
  if (nullptr == mInput) {
    // We already closed the stream!
    *aErrorCode = NS_BASE_STREAM_CLOSED;
    return 0;
  }

  if (NS_FAILED(mLastErrorCode)) {
    // We failed to completely convert last time, and error-recovery
    // is disabled.  We will fare no better this time, so...
    *aErrorCode = mLastErrorCode;
    return 0;
  }

  // We assume a many to one conversion and are using equal sizes for
  // the two buffers.  However if an error happens at the very start
  // of a byte buffer we may end up in a situation where n bytes lead
  // to n+1 unicode chars.  Thus we need to keep track of the leftover
  // bytes as we convert.

  uint32_t nb;
  *aErrorCode = NS_FillArray(mByteData, mInput, mLeftOverBytes, &nb);
  if (nb == 0 && mLeftOverBytes == 0) {
    // No more data
    *aErrorCode = NS_OK;
    return 0;
  }

  NS_ASSERTION(uint32_t(nb) + mLeftOverBytes == mByteData.Length(),
               "mByteData is lying to us somewhere");

  // Now convert as much of the byte buffer to unicode as possible
  auto src = AsBytes(Span(mByteData));
  auto dst = Span(mUnicharData);
  // mUnicharData.Length() is the buffer length, not the fill status.
  // mUnicharDataLength reflects the current fill status.
  mUnicharDataLength = 0;
  // Whenever we convert, mUnicharData is logically empty.
  mUnicharDataOffset = 0;
  // Truncation from size_t to uint32_t below is OK, because the sizes
  // are bounded by the lengths of mByteData and mUnicharData.
  uint32_t result;
  size_t read;
  size_t written;
  bool hadErrors;
  // The design of this class is fundamentally bogus in that trailing
  // errors are ignored. Always passing false as the last argument to
  // Decode* calls below.
  if (mErrorsAreFatal) {
    Tie(result, read, written) =
        mConverter->DecodeToUTF16WithoutReplacement(src, dst, false);
  } else {
    Tie(result, read, written, hadErrors) =
        mConverter->DecodeToUTF16(src, dst, false);
  }
  Unused << hadErrors;
  mLeftOverBytes = mByteData.Length() - read;
  mUnicharDataLength = written;
  if (result == kInputEmpty || result == kOutputFull) {
    *aErrorCode = NS_OK;
  } else {
    MOZ_ASSERT(mErrorsAreFatal, "How come DecodeToUTF16() reported error?");
    *aErrorCode = NS_ERROR_UDEC_ILLEGALINPUT;
  }
  return mUnicharDataLength;
}

NS_IMETHODIMP
nsConverterInputStream::ReadLine(nsAString& aLine, bool* aResult) {
  if (!mLineBuffer) {
    mLineBuffer = MakeUnique<nsLineBuffer<char16_t>>();
  }
  return NS_ReadLine(this, mLineBuffer.get(), aLine, aResult);
}
