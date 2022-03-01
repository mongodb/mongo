/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsContentUtils.h"
#include "FluentResource.h"

using namespace mozilla::dom;

namespace mozilla {
namespace intl {

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(FluentResource, mParent)

NS_IMPL_CYCLE_COLLECTION_ROOT_NATIVE(FluentResource, AddRef)
NS_IMPL_CYCLE_COLLECTION_UNROOT_NATIVE(FluentResource, Release)

FluentResource::FluentResource(nsISupports* aParent, const nsACString& aSource)
    : mParent(aParent),
      mRaw(dont_AddRef(ffi::fluent_resource_new(&aSource, &mHasErrors))) {
  MOZ_COUNT_CTOR(FluentResource);
}

already_AddRefed<FluentResource> FluentResource::Constructor(
    const GlobalObject& aGlobal, const nsACString& aSource) {
  RefPtr<FluentResource> res =
      new FluentResource(aGlobal.GetAsSupports(), aSource);

  if (res->mHasErrors) {
    nsContentUtils::LogSimpleConsoleError(
        u"Errors encountered while parsing Fluent Resource."_ns, "chrome",
        false, true /* from chrome context*/);
  }
  return res.forget();
}

JSObject* FluentResource::WrapObject(JSContext* aCx,
                                     JS::Handle<JSObject*> aGivenProto) {
  return FluentResource_Binding::Wrap(aCx, this, aGivenProto);
}

FluentResource::~FluentResource() { MOZ_COUNT_DTOR(FluentResource); };

}  // namespace intl
}  // namespace mozilla
