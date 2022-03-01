/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsStringBundle_h__
#define nsStringBundle_h__

#include "mozilla/Mutex.h"
#include "nsIStringBundle.h"
#include "nsIMemoryReporter.h"
#include "nsCOMPtr.h"
#include "nsString.h"
#include "nsCOMArray.h"

class nsIPersistentProperties;

class nsStringBundleBase : public nsIStringBundle, public nsIMemoryReporter {
 public:
  MOZ_DEFINE_MALLOC_SIZE_OF(MallocSizeOf)

  nsresult ParseProperties(nsIPersistentProperties**);

  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSISTRINGBUNDLE
  NS_DECL_NSIMEMORYREPORTER

  virtual nsresult LoadProperties() = 0;

  const nsCString& BundleURL() const { return mPropertiesURL; }

  // Returns true if this bundle has more than one reference. If it has only
  // a single reference, it is assumed to be held alive by the bundle cache.
  bool IsShared() const { return mRefCnt > 1; }

  static nsStringBundleBase* Cast(nsIStringBundle* aBundle) {
    return static_cast<nsStringBundleBase*>(aBundle);
  }

  template <typename T, typename... Args>
  static already_AddRefed<T> Create(Args... args);

 protected:
  nsStringBundleBase(const char* aURLSpec);

  virtual ~nsStringBundleBase();

  virtual nsresult GetStringImpl(const nsACString& aName,
                                 nsAString& aResult) = 0;

  virtual nsresult GetSimpleEnumerationImpl(nsISimpleEnumerator** elements) = 0;

  void RegisterMemoryReporter();

  nsCString mPropertiesURL;
  mozilla::Mutex mMutex;
  bool mAttemptedLoad;
  bool mLoaded;

  size_t SizeOfIncludingThisIfUnshared(
      mozilla::MallocSizeOf aMallocSizeOf) const override;

 public:
  static nsresult FormatString(const char16_t* formatStr,
                               const nsTArray<nsString>& aParams,
                               nsAString& aResult);
};

class nsStringBundle : public nsStringBundleBase {
 public:
  NS_DECL_ISUPPORTS_INHERITED

  nsCOMPtr<nsIPersistentProperties> mProps;

  nsresult LoadProperties() override;

  size_t SizeOfIncludingThis(
      mozilla::MallocSizeOf aMallocSizeOf) const override;

 protected:
  friend class nsStringBundleBase;

  explicit nsStringBundle(const char* aURLSpec);

  virtual ~nsStringBundle();

  nsresult GetStringImpl(const nsACString& aName, nsAString& aResult) override;

  nsresult GetSimpleEnumerationImpl(nsISimpleEnumerator** elements) override;
};

#endif
