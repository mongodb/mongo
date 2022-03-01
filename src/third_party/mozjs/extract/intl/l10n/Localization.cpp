/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "Localization.h"
#include "nsImportModule.h"
#include "nsIObserverService.h"
#include "nsContentUtils.h"
#include "mozilla/BasePrincipal.h"
#include "mozilla/HoldDropJSObjects.h"
#include "mozilla/Preferences.h"
#include "mozilla/Services.h"

#define INTL_APP_LOCALES_CHANGED "intl:app-locales-changed"
#define L10N_PSEUDO_PREF "intl.l10n.pseudo"

static const char* kObservedPrefs[] = {L10N_PSEUDO_PREF, nullptr};

using namespace mozilla::intl;
using namespace mozilla::dom;

NS_IMPL_CYCLE_COLLECTION_MULTI_ZONE_JSHOLDER_CLASS(Localization)
NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN(Localization)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mLocalization)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mGlobal)
  NS_IMPL_CYCLE_COLLECTION_UNLINK_PRESERVED_WRAPPER
  NS_IMPL_CYCLE_COLLECTION_UNLINK_WEAK_REFERENCE
  tmp->Destroy();
  mozilla::DropJSObjects(tmp);
NS_IMPL_CYCLE_COLLECTION_UNLINK_END
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN(Localization)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mLocalization)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mGlobal)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_IMPL_CYCLE_COLLECTION_TRACE_BEGIN(Localization)
  NS_IMPL_CYCLE_COLLECTION_TRACE_PRESERVED_WRAPPER
  NS_IMPL_CYCLE_COLLECTION_TRACE_JS_MEMBER_CALLBACK(mGenerateBundles)
  NS_IMPL_CYCLE_COLLECTION_TRACE_JS_MEMBER_CALLBACK(mGenerateBundlesSync)
  NS_IMPL_CYCLE_COLLECTION_TRACE_JS_MEMBER_CALLBACK(mBundles)
NS_IMPL_CYCLE_COLLECTION_TRACE_END

NS_IMPL_CYCLE_COLLECTING_ADDREF(Localization)
NS_IMPL_CYCLE_COLLECTING_RELEASE(Localization)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(Localization)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY_AMBIGUOUS(nsISupports, nsIObserver)
  NS_INTERFACE_MAP_ENTRY(nsIObserver)
  NS_INTERFACE_MAP_ENTRY(nsISupportsWeakReference)
NS_INTERFACE_MAP_END

/* static */
already_AddRefed<Localization> Localization::Create(
    nsIGlobalObject* aGlobal, const bool aSync,
    const BundleGenerator& aBundleGenerator) {
  RefPtr<Localization> loc = new Localization(aGlobal, aSync, aBundleGenerator);

  loc->Init();

  return loc.forget();
}

Localization::Localization(nsIGlobalObject* aGlobal, const bool aSync,
                           const BundleGenerator& aBundleGenerator)
    : mGlobal(aGlobal), mIsSync(aSync) {
  if (aBundleGenerator.mGenerateBundles.WasPassed()) {
    GenerateBundles& generateBundles =
        aBundleGenerator.mGenerateBundles.Value();
    mGenerateBundles.setObject(*generateBundles.CallbackOrNull());
  }
  if (aBundleGenerator.mGenerateBundlesSync.WasPassed()) {
    GenerateBundlesSync& generateBundlesSync =
        aBundleGenerator.mGenerateBundlesSync.Value();
    mGenerateBundlesSync.setObject(*generateBundlesSync.CallbackOrNull());
  }
  mIsSync = aSync;
}

bool Localization::Init() {
  RegisterObservers();

  return true;
}

void Localization::Activate(const bool aEager) {
  mLocalization = do_ImportModule("resource://gre/modules/Localization.jsm",
                                  "Localization");
  MOZ_RELEASE_ASSERT(mLocalization);

  AutoJSContext cx;

  JS::Rooted<JS::Value> generateBundlesJS(cx, mGenerateBundles);
  JS::Rooted<JS::Value> generateBundlesSyncJS(cx, mGenerateBundlesSync);
  JS::Rooted<JS::Value> bundlesJS(cx);
  mLocalization->GenerateBundles(mResourceIds, mIsSync, aEager,
                                 generateBundlesJS, generateBundlesSyncJS,
                                 &bundlesJS);
  mBundles.set(bundlesJS);

  mozilla::HoldJSObjects(this);
}

already_AddRefed<Localization> Localization::Constructor(
    const GlobalObject& aGlobal, const Sequence<nsString>& aResourceIds,
    const bool aSync, const BundleGenerator& aBundleGenerator,
    ErrorResult& aRv) {
  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(aGlobal.GetAsSupports());
  if (!global) {
    aRv.Throw(NS_ERROR_FAILURE);
    return nullptr;
  }

  RefPtr<Localization> loc =
      Localization::Create(global, aSync, aBundleGenerator);

  if (aResourceIds.Length()) {
    loc->AddResourceIds(aResourceIds);
  }

  loc->Activate(true);

  return loc.forget();
}

nsIGlobalObject* Localization::GetParentObject() const { return mGlobal; }

JSObject* Localization::WrapObject(JSContext* aCx,
                                   JS::Handle<JSObject*> aGivenProto) {
  return Localization_Binding::Wrap(aCx, this, aGivenProto);
}

Localization::~Localization() {
  nsCOMPtr<nsIObserverService> obs = mozilla::services::GetObserverService();
  if (obs) {
    obs->RemoveObserver(this, INTL_APP_LOCALES_CHANGED);
  }

  Preferences::RemoveObservers(this, kObservedPrefs);

  Destroy();
  mozilla::DropJSObjects(this);
}

void Localization::Destroy() {
  mGenerateBundles.setUndefined();
  mGenerateBundlesSync.setUndefined();
  mBundles.setUndefined();
}

/* Protected */

void Localization::RegisterObservers() {
  DebugOnly<nsresult> rv = Preferences::AddWeakObservers(this, kObservedPrefs);
  MOZ_ASSERT(NS_SUCCEEDED(rv), "Adding observers failed.");

  nsCOMPtr<nsIObserverService> obs = mozilla::services::GetObserverService();
  if (obs) {
    obs->AddObserver(this, INTL_APP_LOCALES_CHANGED, true);
  }
}

NS_IMETHODIMP
Localization::Observe(nsISupports* aSubject, const char* aTopic,
                      const char16_t* aData) {
  if (!strcmp(aTopic, INTL_APP_LOCALES_CHANGED)) {
    OnChange();
  } else {
    MOZ_ASSERT(!strcmp("nsPref:changed", aTopic));
    nsDependentString pref(aData);
    if (pref.EqualsLiteral(L10N_PSEUDO_PREF)) {
      OnChange();
    }
  }

  return NS_OK;
}

void Localization::OnChange() {
  if (mLocalization) {
    AutoJSContext cx;
    JS::Rooted<JS::Value> generateBundlesJS(cx, mGenerateBundles);
    JS::Rooted<JS::Value> generateBundlesSyncJS(cx, mGenerateBundlesSync);
    JS::Rooted<JS::Value> bundlesJS(cx);
    mLocalization->GenerateBundles(mResourceIds, mIsSync, false,
                                   generateBundlesJS, generateBundlesSyncJS,
                                   &bundlesJS);
    mBundles.set(bundlesJS);
  }
}

uint32_t Localization::AddResourceId(const nsAString& aResourceId) {
  if (!mResourceIds.Contains(aResourceId)) {
    mResourceIds.AppendElement(aResourceId);
    Localization::OnChange();
  }
  return mResourceIds.Length();
}

uint32_t Localization::RemoveResourceId(const nsAString& aResourceId) {
  if (mResourceIds.RemoveElement(aResourceId)) {
    Localization::OnChange();
  }
  return mResourceIds.Length();
}

/**
 * Localization API
 */

uint32_t Localization::AddResourceIds(const nsTArray<nsString>& aResourceIds) {
  bool added = false;

  for (const auto& resId : aResourceIds) {
    if (!mResourceIds.Contains(resId)) {
      mResourceIds.AppendElement(resId);
      added = true;
    }
  }
  if (added) {
    Localization::OnChange();
  }
  return mResourceIds.Length();
}

uint32_t Localization::RemoveResourceIds(
    const nsTArray<nsString>& aResourceIds) {
  bool removed = false;

  for (const auto& resId : aResourceIds) {
    if (mResourceIds.RemoveElement(resId)) {
      removed = true;
    }
  }
  if (removed) {
    Localization::OnChange();
  }
  return mResourceIds.Length();
}

already_AddRefed<Promise> Localization::FormatValue(
    JSContext* aCx, const nsACString& aId, const Optional<L10nArgs>& aArgs,
    ErrorResult& aRv) {
  if (!mLocalization) {
    Activate(false);
  }
  JS::Rooted<JS::Value> args(aCx);

  if (aArgs.WasPassed()) {
    ConvertL10nArgsToJSValue(aCx, aArgs.Value(), &args, aRv);
    if (NS_WARN_IF(aRv.Failed())) {
      return nullptr;
    }
  } else {
    args = JS::UndefinedValue();
  }

  RefPtr<Promise> promise;
  JS::Rooted<JS::Value> bundlesJS(aCx, mBundles);
  nsresult rv = mLocalization->FormatValue(mResourceIds, bundlesJS, aId, args,
                                           getter_AddRefs(promise));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    aRv.Throw(rv);
    return nullptr;
  }
  return MaybeWrapPromise(promise);
}

void Localization::SetIsSync(const bool aIsSync) { mIsSync = aIsSync; }

already_AddRefed<Promise> Localization::FormatValues(
    JSContext* aCx, const Sequence<OwningUTF8StringOrL10nIdArgs>& aKeys,
    ErrorResult& aRv) {
  if (!mLocalization) {
    Activate(false);
  }
  nsTArray<JS::Value> jsKeys;
  SequenceRooter<JS::Value> rooter(aCx, &jsKeys);
  for (auto& key : aKeys) {
    JS::RootedValue jsKey(aCx);
    if (!ToJSValue(aCx, key, &jsKey)) {
      aRv.NoteJSContextException(aCx);
      return nullptr;
    }
    jsKeys.AppendElement(jsKey);
  }

  RefPtr<Promise> promise;
  JS::Rooted<JS::Value> bundlesJS(aCx, mBundles);
  aRv = mLocalization->FormatValues(mResourceIds, bundlesJS, jsKeys,
                                    getter_AddRefs(promise));
  if (NS_WARN_IF(aRv.Failed())) {
    return nullptr;
  }

  return MaybeWrapPromise(promise);
}

already_AddRefed<Promise> Localization::FormatMessages(
    JSContext* aCx, const Sequence<OwningUTF8StringOrL10nIdArgs>& aKeys,
    ErrorResult& aRv) {
  if (!mLocalization) {
    Activate(false);
  }
  nsTArray<JS::Value> jsKeys;
  SequenceRooter<JS::Value> rooter(aCx, &jsKeys);
  for (auto& key : aKeys) {
    JS::RootedValue jsKey(aCx);
    if (!ToJSValue(aCx, key, &jsKey)) {
      aRv.NoteJSContextException(aCx);
      return nullptr;
    }
    jsKeys.AppendElement(jsKey);
  }

  RefPtr<Promise> promise;
  JS::Rooted<JS::Value> bundlesJS(aCx, mBundles);
  aRv = mLocalization->FormatMessages(mResourceIds, bundlesJS, jsKeys,
                                      getter_AddRefs(promise));
  if (NS_WARN_IF(aRv.Failed())) {
    return nullptr;
  }

  return MaybeWrapPromise(promise);
}

void Localization::FormatValueSync(JSContext* aCx, const nsACString& aId,
                                   const Optional<L10nArgs>& aArgs,
                                   nsACString& aRetVal, ErrorResult& aRv) {
  if (!mIsSync) {
    aRv.ThrowInvalidStateError(
        "Can't use formatValueSync when state is async.");
    return;
  }
  if (!mLocalization) {
    Activate(false);
  }
  JS::Rooted<JS::Value> args(aCx);

  if (aArgs.WasPassed()) {
    ConvertL10nArgsToJSValue(aCx, aArgs.Value(), &args, aRv);
    if (NS_WARN_IF(aRv.Failed())) {
      return;
    }
  } else {
    args = JS::UndefinedValue();
  }

  JS::Rooted<JS::Value> bundlesJS(aCx, mBundles);
  aRv = mLocalization->FormatValueSync(mResourceIds, bundlesJS, aId, args,
                                       aRetVal);
}

void Localization::FormatValuesSync(
    JSContext* aCx, const Sequence<OwningUTF8StringOrL10nIdArgs>& aKeys,
    nsTArray<nsCString>& aRetVal, ErrorResult& aRv) {
  if (!mIsSync) {
    aRv.ThrowInvalidStateError(
        "Can't use formatValuesSync when state is async.");
    return;
  }
  if (!mLocalization) {
    Activate(false);
  }
  nsTArray<JS::Value> jsKeys;
  SequenceRooter<JS::Value> rooter(aCx, &jsKeys);
  for (auto& key : aKeys) {
    JS::RootedValue jsKey(aCx);
    if (!ToJSValue(aCx, key, &jsKey)) {
      aRv.NoteJSContextException(aCx);
      return;
    }
    jsKeys.AppendElement(jsKey);
  }

  JS::Rooted<JS::Value> bundlesJS(aCx, mBundles);
  aRv =
      mLocalization->FormatValuesSync(mResourceIds, bundlesJS, jsKeys, aRetVal);
}

void Localization::FormatMessagesSync(
    JSContext* aCx, const Sequence<OwningUTF8StringOrL10nIdArgs>& aKeys,
    nsTArray<Nullable<L10nMessage>>& aRetVal, ErrorResult& aRv) {
  if (!mIsSync) {
    aRv.ThrowInvalidStateError(
        "Can't use formatMessagesSync when state is async.");
    return;
  }
  if (!mLocalization) {
    Activate(false);
  }
  nsTArray<JS::Value> jsKeys;
  SequenceRooter<JS::Value> rooter(aCx, &jsKeys);
  for (auto& key : aKeys) {
    JS::RootedValue jsKey(aCx);
    if (!ToJSValue(aCx, key, &jsKey)) {
      aRv.NoteJSContextException(aCx);
      return;
    }
    jsKeys.AppendElement(jsKey);
  }

  nsTArray<JS::Value> messages;

  SequenceRooter<JS::Value> messagesRooter(aCx, &messages);
  JS::Rooted<JS::Value> bundlesJS(aCx, mBundles);
  aRv = mLocalization->FormatMessagesSync(mResourceIds, bundlesJS, jsKeys,
                                          messages);
  if (NS_WARN_IF(aRv.Failed())) {
    return;
  }

  JS::Rooted<JS::Value> rootedMsg(aCx);
  for (auto& msg : messages) {
    rootedMsg.set(msg);
    Nullable<L10nMessage>* slotPtr = aRetVal.AppendElement(mozilla::fallible);
    if (!slotPtr) {
      aRv.Throw(NS_ERROR_OUT_OF_MEMORY);
      return;
    }

    if (rootedMsg.isNull()) {
      slotPtr->SetNull();
    } else {
      JS_WrapValue(aCx, &rootedMsg);
      if (!slotPtr->SetValue().Init(aCx, rootedMsg)) {
        aRv.NoteJSContextException(aCx);
        return;
      }
    }
  }
}

/**
 * PromiseResolver is a PromiseNativeHandler used
 * by MaybeWrapPromise method.
 */
class PromiseResolver final : public PromiseNativeHandler {
 public:
  NS_DECL_ISUPPORTS

  explicit PromiseResolver(Promise* aPromise);
  void ResolvedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue) override;
  void RejectedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue) override;

 protected:
  virtual ~PromiseResolver();

  RefPtr<Promise> mPromise;
};

NS_INTERFACE_MAP_BEGIN(PromiseResolver)
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

NS_IMPL_ADDREF(PromiseResolver)
NS_IMPL_RELEASE(PromiseResolver)

PromiseResolver::PromiseResolver(Promise* aPromise) { mPromise = aPromise; }

void PromiseResolver::ResolvedCallback(JSContext* aCx,
                                       JS::Handle<JS::Value> aValue) {
  mPromise->MaybeResolveWithClone(aCx, aValue);
}

void PromiseResolver::RejectedCallback(JSContext* aCx,
                                       JS::Handle<JS::Value> aValue) {
  mPromise->MaybeRejectWithClone(aCx, aValue);
}

PromiseResolver::~PromiseResolver() { mPromise = nullptr; }

/**
 * MaybeWrapPromise is a helper method used by Localization
 * API methods to clone the value returned by a promise
 * into a new context.
 *
 * This allows for a promise from a privileged context
 * to be returned into an unprivileged document.
 *
 * This method is only used for promises that carry values.
 */
already_AddRefed<Promise> Localization::MaybeWrapPromise(
    Promise* aInnerPromise) {
  // For system principal we don't need to wrap the
  // result promise at all.
  nsIPrincipal* principal = mGlobal->PrincipalOrNull();
  if (principal && principal->IsSystemPrincipal()) {
    return RefPtr<Promise>(aInnerPromise).forget();
  }

  ErrorResult result;
  RefPtr<Promise> docPromise = Promise::Create(mGlobal, result);
  if (NS_WARN_IF(result.Failed())) {
    return nullptr;
  }

  RefPtr<PromiseResolver> resolver = new PromiseResolver(docPromise);
  aInnerPromise->AppendNativeHandler(resolver);
  return docPromise.forget();
}

void Localization::ConvertL10nArgsToJSValue(
    JSContext* aCx, const L10nArgs& aArgs, JS::MutableHandle<JS::Value> aRetVal,
    ErrorResult& aRv) {
  // This method uses a temporary dictionary to automate
  // converting an IDL Record to a JS Value via a dictionary.
  //
  // Once we get ToJSValue for Record, we'll switch to that.
  L10nArgsHelperDict helperDict;
  for (auto& entry : aArgs.Entries()) {
    L10nArgs::EntryType* newEntry =
        helperDict.mArgs.Entries().AppendElement(fallible);
    if (!newEntry) {
      aRv.Throw(NS_ERROR_OUT_OF_MEMORY);
      return;
    }
    newEntry->mKey = entry.mKey;
    newEntry->mValue = entry.mValue;
  }
  JS::Rooted<JS::Value> jsVal(aCx);
  if (!ToJSValue(aCx, helperDict, &jsVal)) {
    aRv.Throw(NS_ERROR_UNEXPECTED);
    return;
  }
  JS::Rooted<JSObject*> jsObj(aCx, &jsVal.toObject());
  if (!JS_GetProperty(aCx, jsObj, "args", aRetVal)) {
    aRv.Throw(NS_ERROR_UNEXPECTED);
    return;
  }
}
