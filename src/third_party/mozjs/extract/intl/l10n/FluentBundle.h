/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_intl_l10n_FluentBundle_h
#define mozilla_intl_l10n_FluentBundle_h

#include "mozilla/dom/BindingDeclarations.h"
#include "nsCycleCollectionParticipant.h"
#include "nsWrapperCache.h"
#include "mozilla/dom/FluentBinding.h"
#include "mozilla/dom/LocalizationBinding.h"
#include "mozilla/intl/FluentResource.h"
#include "mozilla/intl/FluentBindings.h"

class nsIGlobalObject;

namespace mozilla {
class ErrorResult;

namespace dom {
struct FluentMessage;
struct L10nMessage;
class OwningUTF8StringOrDouble;
class UTF8StringOrUTF8StringSequence;
struct FluentBundleOptions;
struct FluentBundleAddResourceOptions;
}  // namespace dom

namespace intl {

using L10nArgs =
    dom::Record<nsCString, dom::Nullable<dom::OwningUTF8StringOrDouble>>;

class FluentPattern : public nsWrapperCache {
 public:
  NS_INLINE_DECL_CYCLE_COLLECTING_NATIVE_REFCOUNTING(FluentPattern)
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_NATIVE_CLASS(FluentPattern)

  FluentPattern(nsISupports* aParent, const nsACString& aId);
  FluentPattern(nsISupports* aParent, const nsACString& aId,
                const nsACString& aAttrName);
  JSObject* WrapObject(JSContext* aCx,
                       JS::Handle<JSObject*> aGivenProto) override;
  nsISupports* GetParentObject() const { return mParent; }

  nsCString mId;
  nsCString mAttrName;

 protected:
  virtual ~FluentPattern();

  nsCOMPtr<nsISupports> mParent;
};

class FluentBundle final : public nsWrapperCache {
 public:
  NS_INLINE_DECL_CYCLE_COLLECTING_NATIVE_REFCOUNTING(FluentBundle)
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_NATIVE_CLASS(FluentBundle)

  static already_AddRefed<FluentBundle> Constructor(
      const dom::GlobalObject& aGlobal,
      const dom::UTF8StringOrUTF8StringSequence& aLocales,
      const dom::FluentBundleOptions& aOptions, ErrorResult& aRv);
  JSObject* WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) final;
  nsISupports* GetParentObject() const { return mParent; }

  void GetLocales(nsTArray<nsCString>& aLocales);

  void AddResource(FluentResource& aResource,
                   const dom::FluentBundleAddResourceOptions& aOptions);
  bool HasMessage(const nsACString& aId);
  void GetMessage(const nsACString& aId,
                  dom::Nullable<dom::FluentMessage>& aRetVal);
  void FormatPattern(JSContext* aCx, const FluentPattern& aPattern,
                     const dom::Nullable<L10nArgs>& aArgs,
                     const dom::Optional<JS::Handle<JSObject*>>& aErrors,
                     nsACString& aRetVal, ErrorResult& aRv);

 protected:
  explicit FluentBundle(nsISupports* aParent,
                        UniquePtr<ffi::FluentBundleRc> aRaw);
  virtual ~FluentBundle();

  nsCOMPtr<nsISupports> mParent;
  UniquePtr<ffi::FluentBundleRc> mRaw;
};

}  // namespace intl
}  // namespace mozilla

#endif
