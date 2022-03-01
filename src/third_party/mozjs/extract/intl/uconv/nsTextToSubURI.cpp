/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "nsString.h"
#include "nsITextToSubURI.h"
#include "nsEscape.h"
#include "nsTextToSubURI.h"
#include "nsCRT.h"
#include "mozilla/ArrayUtils.h"
#include "mozilla/Encoding.h"
#include "mozilla/Preferences.h"
#include "mozilla/TextUtils.h"
#include "mozilla/Utf8.h"

using namespace mozilla;

nsTextToSubURI::~nsTextToSubURI() = default;

NS_IMPL_ISUPPORTS(nsTextToSubURI, nsITextToSubURI)

NS_IMETHODIMP
nsTextToSubURI::ConvertAndEscape(const nsACString& aCharset,
                                 const nsAString& aText, nsACString& aOut) {
  auto encoding = Encoding::ForLabelNoReplacement(aCharset);
  if (!encoding) {
    aOut.Truncate();
    return NS_ERROR_UCONV_NOCONV;
  }
  nsresult rv;
  const Encoding* actualEncoding;
  nsAutoCString intermediate;
  Tie(rv, actualEncoding) = encoding->Encode(aText, intermediate);
  Unused << actualEncoding;
  if (NS_FAILED(rv)) {
    aOut.Truncate();
    return rv;
  }
  bool ok = NS_Escape(intermediate, aOut, url_XPAlphas);
  if (!ok) {
    aOut.Truncate();
    return NS_ERROR_OUT_OF_MEMORY;
  }
  return NS_OK;
}

NS_IMETHODIMP
nsTextToSubURI::UnEscapeAndConvert(const nsACString& aCharset,
                                   const nsACString& aText, nsAString& aOut) {
  auto encoding = Encoding::ForLabelNoReplacement(aCharset);
  if (!encoding) {
    aOut.Truncate();
    return NS_ERROR_UCONV_NOCONV;
  }
  nsAutoCString unescaped(aText);
  NS_UnescapeURL(unescaped);
  auto rv = encoding->DecodeWithoutBOMHandling(unescaped, aOut);
  if (NS_SUCCEEDED(rv)) {
    return NS_OK;
  }
  return rv;
}

static bool statefulCharset(const char* charset) {
  // HZ, UTF-7 and the CN and KR ISO-2022 variants are no longer in
  // mozilla-central but keeping them here just in case for the benefit of
  // comm-central.
  if (!nsCRT::strncasecmp(charset, "ISO-2022-", sizeof("ISO-2022-") - 1) ||
      !nsCRT::strcasecmp(charset, "UTF-7") ||
      !nsCRT::strcasecmp(charset, "HZ-GB-2312"))
    return true;

  return false;
}

nsresult nsTextToSubURI::convertURItoUnicode(const nsCString& aCharset,
                                             const nsCString& aURI,
                                             nsAString& aOut) {
  // check for 7bit encoding the data may not be ASCII after we decode
  bool isStatefulCharset = statefulCharset(aCharset.get());

  if (!isStatefulCharset) {
    if (IsAscii(aURI)) {
      CopyASCIItoUTF16(aURI, aOut);
      return NS_OK;
    }
    if (IsUtf8(aURI)) {
      CopyUTF8toUTF16(aURI, aOut);
      return NS_OK;
    }
  }

  // empty charset could indicate UTF-8, but aURI turns out not to be UTF-8.
  NS_ENSURE_FALSE(aCharset.IsEmpty(), NS_ERROR_INVALID_ARG);

  auto encoding = Encoding::ForLabelNoReplacement(aCharset);
  if (!encoding) {
    aOut.Truncate();
    return NS_ERROR_UCONV_NOCONV;
  }
  return encoding->DecodeWithoutBOMHandlingAndWithoutReplacement(aURI, aOut);
}

NS_IMETHODIMP nsTextToSubURI::UnEscapeURIForUI(const nsACString& aURIFragment,
                                               nsAString& _retval) {
  nsAutoCString unescapedSpec;
  // skip control octets (0x00 - 0x1f and 0x7f) when unescaping
  NS_UnescapeURL(PromiseFlatCString(aURIFragment),
                 esc_SkipControl | esc_AlwaysCopy, unescapedSpec);

  // in case of failure, return escaped URI
  // Test for != NS_OK rather than NS_FAILED, because incomplete multi-byte
  // sequences are also considered failure in this context
  if (convertURItoUnicode("UTF-8"_ns, unescapedSpec, _retval) != NS_OK) {
    // assume UTF-8 instead of ASCII  because hostname (IDN) may be in UTF-8
    CopyUTF8toUTF16(aURIFragment, _retval);
  }

  // If there are any characters that are unsafe for URIs, reescape those.
  if (mIDNBlocklist.IsEmpty()) {
    mozilla::net::InitializeBlocklist(mIDNBlocklist);
    // we allow SPACE and IDEOGRAPHIC SPACE in this method
    mozilla::net::RemoveCharFromBlocklist(u' ', mIDNBlocklist);
    mozilla::net::RemoveCharFromBlocklist(0x3000, mIDNBlocklist);
  }

  MOZ_ASSERT(!mIDNBlocklist.IsEmpty());
  const nsPromiseFlatString& unescapedResult = PromiseFlatString(_retval);
  nsString reescapedSpec;
  _retval = NS_EscapeURL(
      unescapedResult,
      [&](char16_t aChar) -> bool {
        return mozilla::net::CharInBlocklist(aChar, mIDNBlocklist);
      },
      reescapedSpec);

  return NS_OK;
}

NS_IMETHODIMP
nsTextToSubURI::UnEscapeNonAsciiURI(const nsACString& aCharset,
                                    const nsACString& aURIFragment,
                                    nsAString& _retval) {
  nsAutoCString unescapedSpec;
  NS_UnescapeURL(PromiseFlatCString(aURIFragment),
                 esc_AlwaysCopy | esc_OnlyNonASCII, unescapedSpec);
  // leave the URI as it is if it's not UTF-8 and aCharset is not a ASCII
  // superset since converting "http:" with such an encoding is always a bad
  // idea.
  if (!IsUtf8(unescapedSpec) &&
      (aCharset.LowerCaseEqualsLiteral("utf-16") ||
       aCharset.LowerCaseEqualsLiteral("utf-16be") ||
       aCharset.LowerCaseEqualsLiteral("utf-16le") ||
       aCharset.LowerCaseEqualsLiteral("utf-7") ||
       aCharset.LowerCaseEqualsLiteral("x-imap4-modified-utf7"))) {
    CopyASCIItoUTF16(aURIFragment, _retval);
    return NS_OK;
  }

  nsresult rv =
      convertURItoUnicode(PromiseFlatCString(aCharset), unescapedSpec, _retval);
  // NS_OK_UDEC_MOREINPUT is a success code, so caller can't catch the error
  // if the string ends with a valid (but incomplete) sequence.
  return rv == NS_OK_UDEC_MOREINPUT ? NS_ERROR_UDEC_ILLEGALINPUT : rv;
}

//----------------------------------------------------------------------
