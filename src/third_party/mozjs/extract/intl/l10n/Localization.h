/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_intl_l10n_Localization_h
#define mozilla_intl_l10n_Localization_h

#include "nsWeakReference.h"
#include "nsIObserver.h"
#include "mozILocalization.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/LocalizationBinding.h"
#include "mozilla/dom/PromiseNativeHandler.h"

class nsIGlobalObject;

using namespace mozilla::dom;

namespace mozilla {
class ErrorResult;

namespace intl {

typedef Record<nsCString, Nullable<OwningUTF8StringOrDouble>> L10nArgs;

class Localization : public nsIObserver,
                     public nsSupportsWeakReference,
                     public nsWrapperCache {
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS_AMBIGUOUS(Localization,
                                                         nsIObserver)
  NS_DECL_NSIOBSERVER

  static already_AddRefed<Localization> Create(
      nsIGlobalObject* aGlobal, const bool aSync,
      const BundleGenerator& aBundleGenerator);

  void Activate(const bool aEager);

  void Destroy();

  static already_AddRefed<Localization> Constructor(
      const GlobalObject& aGlobal, const Sequence<nsString>& aResourceIds,
      const bool aSync, const BundleGenerator& aBundleGenerator,
      ErrorResult& aRv);

  nsIGlobalObject* GetParentObject() const;

  virtual JSObject* WrapObject(JSContext* aCx,
                               JS::Handle<JSObject*> aGivenProto) override;

  uint32_t AddResourceId(const nsAString& aResourceId);
  uint32_t RemoveResourceId(const nsAString& aResourceId);

  /**
   * Localization API
   *
   * Methods documentation in Localization.webidl
   */
  uint32_t AddResourceIds(const nsTArray<nsString>& aResourceIds);

  uint32_t RemoveResourceIds(const nsTArray<nsString>& aResourceIds);

  already_AddRefed<Promise> FormatValue(JSContext* aCx, const nsACString& aId,
                                        const Optional<L10nArgs>& aArgs,
                                        ErrorResult& aRv);

  already_AddRefed<Promise> FormatValues(
      JSContext* aCx, const Sequence<OwningUTF8StringOrL10nIdArgs>& aKeys,
      ErrorResult& aRv);

  already_AddRefed<Promise> FormatMessages(
      JSContext* aCx, const Sequence<OwningUTF8StringOrL10nIdArgs>& aKeys,
      ErrorResult& aRv);

  void SetIsSync(const bool aIsSync);

  void FormatValueSync(JSContext* aCx, const nsACString& aId,
                       const Optional<L10nArgs>& aArgs, nsACString& aRetVal,
                       ErrorResult& aRv);
  void FormatValuesSync(JSContext* aCx,
                        const Sequence<OwningUTF8StringOrL10nIdArgs>& aKeys,
                        nsTArray<nsCString>& aRetVal, ErrorResult& aRv);
  void FormatMessagesSync(JSContext* aCx,
                          const Sequence<OwningUTF8StringOrL10nIdArgs>& aKeys,
                          nsTArray<Nullable<L10nMessage>>& aRetVal,
                          ErrorResult& aRv);

 protected:
  Localization(nsIGlobalObject* aGlobal, const bool aSync,
               const BundleGenerator& aBundleGenerator);
  virtual bool Init();

  virtual ~Localization();
  void RegisterObservers();
  virtual void OnChange();
  already_AddRefed<Promise> MaybeWrapPromise(Promise* aInnerPromise);
  void ConvertL10nArgsToJSValue(JSContext* aCx, const L10nArgs& aArgs,
                                JS::MutableHandle<JS::Value> aRetVal,
                                ErrorResult& aRv);

  nsCOMPtr<nsIGlobalObject> mGlobal;
  nsCOMPtr<mozILocalization> mLocalization;

  bool mIsSync;
  nsTArray<nsString> mResourceIds;

  JS::Heap<JS::Value> mBundles;
  JS::Heap<JS::Value> mGenerateBundles;
  JS::Heap<JS::Value> mGenerateBundlesSync;
};

}  // namespace intl
}  // namespace mozilla

#endif
