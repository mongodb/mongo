/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsConverterInputStream_h
#define nsConverterInputStream_h

#include "nsIInputStream.h"
#include "nsIConverterInputStream.h"
#include "nsIUnicharLineInputStream.h"
#include "nsTArray.h"
#include "nsCOMPtr.h"
#include "nsReadLine.h"
#include "mozilla/Encoding.h"
#include "mozilla/UniquePtr.h"

#define NS_CONVERTERINPUTSTREAM_CONTRACTID \
  "@mozilla.org/intl/converter-input-stream;1"

// {2BC2AD62-AD5D-4b7b-A9DB-F74AE203C527}
#define NS_CONVERTERINPUTSTREAM_CID                 \
  {                                                 \
    0x2bc2ad62, 0xad5d, 0x4b7b, {                   \
      0xa9, 0xdb, 0xf7, 0x4a, 0xe2, 0x3, 0xc5, 0x27 \
    }                                               \
  }

class nsConverterInputStream : public nsIConverterInputStream,
                               public nsIUnicharLineInputStream {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIUNICHARINPUTSTREAM
  NS_DECL_NSIUNICHARLINEINPUTSTREAM
  NS_DECL_NSICONVERTERINPUTSTREAM

  nsConverterInputStream()
      : mLastErrorCode(NS_OK),
        mLeftOverBytes(0),
        mUnicharDataOffset(0),
        mUnicharDataLength(0),
        mErrorsAreFatal(false),
        mLineBuffer(nullptr) {}

 private:
  virtual ~nsConverterInputStream() { Close(); }

  uint32_t Fill(nsresult* aErrorCode);

  mozilla::UniquePtr<mozilla::Decoder> mConverter;
  FallibleTArray<char> mByteData;
  FallibleTArray<char16_t> mUnicharData;
  nsCOMPtr<nsIInputStream> mInput;

  nsresult mLastErrorCode;
  uint32_t mLeftOverBytes;
  uint32_t mUnicharDataOffset;
  uint32_t mUnicharDataLength;
  bool mErrorsAreFatal;

  mozilla::UniquePtr<nsLineBuffer<char16_t> > mLineBuffer;
};

#endif
