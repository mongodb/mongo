// -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
// vim: set ts=2 et sw=2 tw=80:
// This Source Code is subject to the terms of the Mozilla Public License
// version 2.0 (the "License"). You can obtain a copy of the License at
// http://mozilla.org/MPL/2.0/.
#ifndef nsTextToSubURI_h__
#define nsTextToSubURI_h__

#include "nsITextToSubURI.h"
#include "nsString.h"
#include "nsTArray.h"
#include "mozilla/net/IDNBlocklistUtils.h"

class nsTextToSubURI : public nsITextToSubURI {
  NS_DECL_ISUPPORTS
  NS_DECL_NSITEXTTOSUBURI

 private:
  virtual ~nsTextToSubURI();

  // We assume that the URI is encoded as UTF-8.
  nsresult convertURItoUnicode(const nsCString& aCharset, const nsCString& aURI,
                               nsAString& _retval);

  // Characters defined in netwerk/dns/IDNCharacterBlocklist.inc or via the
  // network.IDN.extra_allowed_chars and network.IDN.extra_blocked_chars prefs.
  nsTArray<mozilla::net::BlocklistRange> mIDNBlocklist;
};

#endif  // nsTextToSubURI_h__
