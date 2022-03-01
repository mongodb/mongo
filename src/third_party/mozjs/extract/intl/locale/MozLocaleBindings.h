/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_intl_locale_MozLocaleBindings_h
#define mozilla_intl_locale_MozLocaleBindings_h

#include "mozilla/intl/unic_langid_ffi_generated.h"
#include "mozilla/intl/fluent_langneg_ffi_generated.h"

#include "mozilla/UniquePtr.h"

namespace mozilla {

template <>
class DefaultDelete<intl::ffi::LanguageIdentifier> {
 public:
  void operator()(intl::ffi::LanguageIdentifier* aPtr) const {
    unic_langid_destroy(aPtr);
  }
};

}  // namespace mozilla

#endif
