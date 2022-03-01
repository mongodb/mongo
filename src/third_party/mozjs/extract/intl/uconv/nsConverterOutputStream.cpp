/* vim:set expandtab ts=4 sw=2 sts=2 cin: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsCOMPtr.h"

#include "nsIOutputStream.h"
#include "nsString.h"

#include "nsConverterOutputStream.h"
#include "mozilla/Encoding.h"
#include "mozilla/Unused.h"

using namespace mozilla;

NS_IMPL_ISUPPORTS(nsConverterOutputStream, nsIUnicharOutputStream,
                  nsIConverterOutputStream)

nsConverterOutputStream::~nsConverterOutputStream() { Close(); }

NS_IMETHODIMP
nsConverterOutputStream::Init(nsIOutputStream* aOutStream,
                              const char* aCharset) {
  MOZ_ASSERT(aOutStream, "Null output stream!");

  const Encoding* encoding;
  if (!aCharset) {
    encoding = UTF_8_ENCODING;
  } else {
    encoding = Encoding::ForLabelNoReplacement(MakeStringSpan(aCharset));
    if (!encoding || encoding == UTF_16LE_ENCODING ||
        encoding == UTF_16BE_ENCODING) {
      return NS_ERROR_UCONV_NOCONV;
    }
  }

  mConverter = encoding->NewEncoder();

  mOutStream = aOutStream;

  return NS_OK;
}

NS_IMETHODIMP
nsConverterOutputStream::Write(uint32_t aCount, const char16_t* aChars,
                               bool* aSuccess) {
  if (!mOutStream) {
    NS_ASSERTION(!mConverter, "Closed streams shouldn't have converters");
    return NS_BASE_STREAM_CLOSED;
  }
  MOZ_ASSERT(mConverter, "Must have a converter when not closed");
  uint8_t buffer[4096];
  auto dst = Span(buffer);
  auto src = Span(aChars, aCount);
  for (;;) {
    uint32_t result;
    size_t read;
    size_t written;
    bool hadErrors;
    Tie(result, read, written, hadErrors) =
        mConverter->EncodeFromUTF16(src, dst, false);
    Unused << hadErrors;
    src = src.From(read);
    uint32_t streamWritten;
    nsresult rv = mOutStream->Write(reinterpret_cast<char*>(dst.Elements()),
                                    written, &streamWritten);
    *aSuccess = NS_SUCCEEDED(rv) && written == streamWritten;
    if (!(*aSuccess)) {
      return rv;
    }
    if (result == kInputEmpty) {
      return NS_OK;
    }
  }
}

NS_IMETHODIMP
nsConverterOutputStream::WriteString(const nsAString& aString, bool* aSuccess) {
  int32_t inLen = aString.Length();
  nsAString::const_iterator i;
  aString.BeginReading(i);
  return Write(inLen, i.get(), aSuccess);
}

NS_IMETHODIMP
nsConverterOutputStream::Flush() {
  if (!mOutStream) return NS_OK;  // Already closed.

  // If we are encoding to ISO-2022-JP, potentially
  // transition back to the ASCII state. The buffer
  // needs to be large enough for an additional NCR,
  // though.
  uint8_t buffer[12];
  auto dst = Span(buffer);
  Span<char16_t> src(nullptr);
  uint32_t result;
  size_t read;
  size_t written;
  bool hadErrors;
  Tie(result, read, written, hadErrors) =
      mConverter->EncodeFromUTF16(src, dst, true);
  Unused << hadErrors;
  MOZ_ASSERT(result == kInputEmpty);
  uint32_t streamWritten;
  if (!written) {
    return NS_OK;
  }
  return mOutStream->Write(reinterpret_cast<char*>(dst.Elements()), written,
                           &streamWritten);
}

NS_IMETHODIMP
nsConverterOutputStream::Close() {
  if (!mOutStream) return NS_OK;  // Already closed.

  nsresult rv1 = Flush();

  nsresult rv2 = mOutStream->Close();
  mOutStream = nullptr;
  mConverter = nullptr;
  return NS_FAILED(rv1) ? rv1 : rv2;
}
