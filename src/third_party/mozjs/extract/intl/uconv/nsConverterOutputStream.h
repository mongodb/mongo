/* vim:set expandtab ts=4 sw=2 sts=2 cin: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef NSCONVERTEROUTPUTSTREAM_H_
#define NSCONVERTEROUTPUTSTREAM_H_

#include "nsIConverterOutputStream.h"
#include "nsCOMPtr.h"
#include "mozilla/Attributes.h"
#include "mozilla/Encoding.h"

class nsIOutputStream;

/* ff8780a5-bbb1-4bc5-8ee7-057e7bc5c925 */
#define NS_CONVERTEROUTPUTSTREAM_CID                 \
  {                                                  \
    0xff8780a5, 0xbbb1, 0x4bc5, {                    \
      0x8e, 0xe7, 0x05, 0x7e, 0x7b, 0xc5, 0xc9, 0x25 \
    }                                                \
  }

class nsConverterOutputStream final : public nsIConverterOutputStream {
 public:
  nsConverterOutputStream() = default;

  NS_DECL_ISUPPORTS
  NS_DECL_NSIUNICHAROUTPUTSTREAM
  NS_DECL_NSICONVERTEROUTPUTSTREAM

 private:
  ~nsConverterOutputStream();

  mozilla::UniquePtr<mozilla::Encoder> mConverter;
  nsCOMPtr<nsIOutputStream> mOutStream;
};

#endif
