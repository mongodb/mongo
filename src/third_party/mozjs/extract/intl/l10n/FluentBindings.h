/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_intl_l10n_FluentBindings_h
#define mozilla_intl_l10n_FluentBindings_h

#include "mozilla/intl/fluent_ffi_generated.h"

#include "mozilla/RefPtr.h"

namespace mozilla {

template <>
struct RefPtrTraits<intl::ffi::FluentResource> {
  static void AddRef(const intl::ffi::FluentResource* aPtr) {
    intl::ffi::fluent_resource_addref(aPtr);
  }
  static void Release(const intl::ffi::FluentResource* aPtr) {
    intl::ffi::fluent_resource_release(aPtr);
  }
};

template <>
class DefaultDelete<intl::ffi::FluentBundleRc> {
 public:
  void operator()(intl::ffi::FluentBundleRc* aPtr) const {
    fluent_bundle_destroy(aPtr);
  }
};

}  // namespace mozilla

#endif
