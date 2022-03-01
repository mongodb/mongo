
/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsString.h"
#include "nsIScriptableUConv.h"
#include "nsScriptableUConv.h"
#include "nsIStringStream.h"
#include "nsComponentManagerUtils.h"

using namespace mozilla;

/* Implementation file */
NS_IMPL_ISUPPORTS(nsScriptableUnicodeConverter, nsIScriptableUnicodeConverter)

nsScriptableUnicodeConverter::nsScriptableUnicodeConverter()
    : mIsInternal(false) {}

nsScriptableUnicodeConverter::~nsScriptableUnicodeConverter() = default;

NS_IMETHODIMP
nsScriptableUnicodeConverter::ConvertFromUnicode(const nsAString& aSrc,
                                                 nsACString& _retval) {
  if (!mEncoder) return NS_ERROR_FAILURE;

  // We can compute the length without replacement, because the
  // the replacement is only one byte long and a mappable character
  // would always output something, i.e. at least one byte.
  // When encoding to ISO-2022-JP, unmappables shouldn't be able
  // to cause more escape sequences to be emitted than the mappable
  // worst case where every input character causes an escape into
  // a different state.
  CheckedInt<size_t> needed =
      mEncoder->MaxBufferLengthFromUTF16WithoutReplacement(aSrc.Length());
  if (!needed.isValid() || needed.value() > UINT32_MAX) {
    return NS_ERROR_OUT_OF_MEMORY;
  }

  auto dstChars = _retval.GetMutableData(needed.value(), fallible);
  if (!dstChars) {
    return NS_ERROR_OUT_OF_MEMORY;
  }

  auto src = Span(aSrc);
  auto dst = AsWritableBytes(*dstChars);
  size_t totalWritten = 0;
  for (;;) {
    uint32_t result;
    size_t read;
    size_t written;
    Tie(result, read, written) =
        mEncoder->EncodeFromUTF16WithoutReplacement(src, dst, false);
    if (result != kInputEmpty && result != kOutputFull) {
      MOZ_RELEASE_ASSERT(written < dst.Length(),
                         "Unmappables with one-byte replacement should not "
                         "exceed mappable worst case.");
      dst[written++] = '?';
    }
    totalWritten += written;
    if (result == kInputEmpty) {
      MOZ_ASSERT(totalWritten <= UINT32_MAX);
      if (!_retval.SetLength(totalWritten, fallible)) {
        return NS_ERROR_OUT_OF_MEMORY;
      }
      return NS_OK;
    }
    src = src.From(read);
    dst = dst.From(written);
  }
}

NS_IMETHODIMP
nsScriptableUnicodeConverter::Finish(nsACString& _retval) {
  // The documentation for this method says it should be called after
  // ConvertFromUnicode(). However, our own tests called it after
  // convertFromByteArray(), i.e. when *decoding*.
  // Assuming that there exists extensions that similarly call
  // this at the wrong time, let's deal. In general, it is a design
  // error for this class to handle conversions in both directions.
  if (!mEncoder) {
    _retval.Truncate();
    mDecoder->Encoding()->NewDecoderWithBOMRemovalInto(*mDecoder);
    return NS_OK;
  }
  // If we are encoding to ISO-2022-JP, potentially
  // transition back to the ASCII state. The buffer
  // needs to be large enough for an additional NCR,
  // though.
  _retval.SetLength(13);
  auto dst = AsWritableBytes(_retval.GetMutableData(13));
  Span<char16_t> src(nullptr);
  uint32_t result;
  size_t read;
  size_t written;
  bool hadErrors;
  Tie(result, read, written, hadErrors) =
      mEncoder->EncodeFromUTF16(src, dst, true);
  Unused << hadErrors;
  MOZ_ASSERT(!read);
  MOZ_ASSERT(result == kInputEmpty);
  _retval.SetLength(written);

  mDecoder->Encoding()->NewDecoderWithBOMRemovalInto(*mDecoder);
  mEncoder->Encoding()->NewEncoderInto(*mEncoder);
  return NS_OK;
}

NS_IMETHODIMP
nsScriptableUnicodeConverter::ConvertToUnicode(const nsACString& aSrc,
                                               nsAString& _retval) {
  if (!mDecoder) return NS_ERROR_FAILURE;

  uint32_t length = aSrc.Length();

  CheckedInt<size_t> needed = mDecoder->MaxUTF16BufferLength(length);
  if (!needed.isValid() || needed.value() > UINT32_MAX) {
    return NS_ERROR_OUT_OF_MEMORY;
  }

  auto dst = _retval.GetMutableData(needed.value(), fallible);
  if (!dst) {
    return NS_ERROR_OUT_OF_MEMORY;
  }

  auto src =
      Span(reinterpret_cast<const uint8_t*>(aSrc.BeginReading()), length);
  uint32_t result;
  size_t read;
  size_t written;
  bool hadErrors;
  // The UTF-8 decoder used to throw regardless of the error behavior.
  // Simulating the old behavior for compatibility with legacy callers.
  // If callers want control over the behavior, they should switch to
  // TextDecoder.
  if (mDecoder->Encoding() == UTF_8_ENCODING) {
    Tie(result, read, written) =
        mDecoder->DecodeToUTF16WithoutReplacement(src, *dst, false);
    if (result != kInputEmpty) {
      return NS_ERROR_UDEC_ILLEGALINPUT;
    }
  } else {
    Tie(result, read, written, hadErrors) =
        mDecoder->DecodeToUTF16(src, *dst, false);
  }
  MOZ_ASSERT(result == kInputEmpty);
  MOZ_ASSERT(read == length);
  MOZ_ASSERT(written <= needed.value());
  Unused << hadErrors;
  if (!_retval.SetLength(written, fallible)) {
    return NS_ERROR_OUT_OF_MEMORY;
  }
  return NS_OK;
}

NS_IMETHODIMP
nsScriptableUnicodeConverter::ConvertToByteArray(const nsAString& aString,
                                                 uint32_t* aLen,
                                                 uint8_t** _aData) {
  if (!mEncoder) return NS_ERROR_FAILURE;

  CheckedInt<size_t> needed =
      mEncoder->MaxBufferLengthFromUTF16WithoutReplacement(aString.Length());
  if (!needed.isValid() || needed.value() > UINT32_MAX) {
    return NS_ERROR_OUT_OF_MEMORY;
  }

  uint8_t* data = (uint8_t*)malloc(needed.value());
  if (!data) {
    return NS_ERROR_OUT_OF_MEMORY;
  }
  auto src = Span(aString);
  auto dst = Span(data, needed.value());
  size_t totalWritten = 0;
  for (;;) {
    uint32_t result;
    size_t read;
    size_t written;
    Tie(result, read, written) =
        mEncoder->EncodeFromUTF16WithoutReplacement(src, dst, true);
    if (result != kInputEmpty && result != kOutputFull) {
      // There's always room for one byte in the case of
      // an unmappable character, because otherwise
      // we'd have gotten `kOutputFull`.
      dst[written++] = '?';
    }
    totalWritten += written;
    if (result == kInputEmpty) {
      *_aData = data;
      MOZ_ASSERT(totalWritten <= UINT32_MAX);
      *aLen = totalWritten;
      return NS_OK;
    }
    src = src.From(read);
    dst = dst.From(written);
  }
}

NS_IMETHODIMP
nsScriptableUnicodeConverter::ConvertToInputStream(const nsAString& aString,
                                                   nsIInputStream** _retval) {
  nsresult rv;
  nsCOMPtr<nsIStringInputStream> inputStream =
      do_CreateInstance("@mozilla.org/io/string-input-stream;1", &rv);
  if (NS_FAILED(rv)) return rv;

  uint8_t* data;
  uint32_t dataLen;
  rv = ConvertToByteArray(aString, &dataLen, &data);
  if (NS_FAILED(rv)) return rv;

  rv = inputStream->AdoptData(reinterpret_cast<char*>(data), dataLen);
  if (NS_FAILED(rv)) {
    free(data);
    return rv;
  }

  NS_ADDREF(*_retval = inputStream);
  return rv;
}

NS_IMETHODIMP
nsScriptableUnicodeConverter::GetCharset(nsACString& aCharset) {
  if (!mDecoder) {
    aCharset.Truncate();
  } else {
    mDecoder->Encoding()->Name(aCharset);
  }
  return NS_OK;
}

NS_IMETHODIMP
nsScriptableUnicodeConverter::SetCharset(const nsACString& aCharset) {
  return InitConverter(aCharset);
}

NS_IMETHODIMP
nsScriptableUnicodeConverter::GetIsInternal(bool* aIsInternal) {
  *aIsInternal = mIsInternal;
  return NS_OK;
}

NS_IMETHODIMP
nsScriptableUnicodeConverter::SetIsInternal(const bool aIsInternal) {
  mIsInternal = aIsInternal;
  return NS_OK;
}

nsresult nsScriptableUnicodeConverter::InitConverter(
    const nsACString& aCharset) {
  mEncoder = nullptr;
  mDecoder = nullptr;

  auto encoding = Encoding::ForLabelNoReplacement(aCharset);
  if (!encoding) {
    return NS_ERROR_UCONV_NOCONV;
  }
  if (!(encoding == UTF_16LE_ENCODING || encoding == UTF_16BE_ENCODING)) {
    mEncoder = encoding->NewEncoder();
  }
  mDecoder = encoding->NewDecoderWithBOMRemoval();
  return NS_OK;
}
