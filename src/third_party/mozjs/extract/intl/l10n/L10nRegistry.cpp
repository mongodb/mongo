/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/RefPtr.h"
#include "mozilla/URLPreloader.h"
#include "nsIChannel.h"
#include "nsILoadInfo.h"
#include "nsIStreamLoader.h"
#include "nsNetUtil.h"
#include "nsString.h"

namespace mozilla {
namespace intl {

class ResourceLoader final : public nsIStreamLoaderObserver {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSISTREAMLOADEROBSERVER

  typedef nsresult (*Callback)(void* aClosure, nsACString* aString,
                               nsresult aSuccess);

  ResourceLoader(Callback aCallback, void* aClosure)
      : mCallback(aCallback), mClosure(aClosure) {}

 protected:
  ~ResourceLoader() = default;

 private:
  Callback mCallback;
  void* mClosure;
};

NS_IMPL_ISUPPORTS(ResourceLoader, nsIStreamLoaderObserver)

// nsIStreamLoaderObserver
NS_IMETHODIMP
ResourceLoader::OnStreamComplete(nsIStreamLoader* aLoader,
                                 nsISupports* aContext, nsresult aStatus,
                                 uint32_t aStringLen, const uint8_t* aString) {
  MOZ_ASSERT(NS_IsMainThread());

  if (NS_FAILED(aStatus)) {
    mCallback(mClosure, nullptr, aStatus);
    return NS_OK;
  }

  nsCString data;
  data.Adopt(reinterpret_cast<char*>(const_cast<uint8_t*>(aString)),
             aStringLen);
  mCallback(mClosure, &data, NS_OK);

  return NS_SUCCESS_ADOPTED_DATA;
}

class L10nRegistry {
 public:
  static nsresult Load(const nsACString& aPath,
                       ResourceLoader::Callback aCallback, void* aClosure) {
    nsCOMPtr<nsIURI> uri;
    nsresult rv = NS_NewURI(getter_AddRefs(uri), aPath);
    NS_ENSURE_SUCCESS(rv, rv);
    NS_ENSURE_TRUE(uri, NS_ERROR_INVALID_ARG);

    RefPtr<ResourceLoader> listener =
        MakeRefPtr<ResourceLoader>(aCallback, aClosure);

    // TODO: What is the lifetime requirement for loader?
    RefPtr<nsIStreamLoader> loader;
    rv = NS_NewStreamLoader(
        getter_AddRefs(loader), uri, listener,
        nsContentUtils::GetSystemPrincipal(),
        nsILoadInfo::SEC_ALLOW_CROSS_ORIGIN_SEC_CONTEXT_IS_NULL,
        nsIContentPolicy::TYPE_OTHER);

    return rv;
  }

  static nsresult LoadSync(const nsACString& aPath, nsACString& aRetVal) {
    nsCOMPtr<nsIURI> uri;

    nsresult rv = NS_NewURI(getter_AddRefs(uri), aPath);
    NS_ENSURE_SUCCESS(rv, rv);

    NS_ENSURE_TRUE(uri, NS_ERROR_INVALID_ARG);

    auto result = URLPreloader::ReadURI(uri);
    if (result.isOk()) {
      aRetVal = result.unwrap();
      return NS_OK;
    }

    auto err = result.unwrapErr();
    if (err != NS_ERROR_INVALID_ARG && err != NS_ERROR_NOT_INITIALIZED) {
      return err;
    }

    nsCOMPtr<nsIChannel> channel;
    rv = NS_NewChannel(
        getter_AddRefs(channel), uri, nsContentUtils::GetSystemPrincipal(),
        nsILoadInfo::SEC_ALLOW_CROSS_ORIGIN_SEC_CONTEXT_IS_NULL,
        nsIContentPolicy::TYPE_OTHER, nullptr, /* nsICookieJarSettings */
        nullptr,                               /* aPerformanceStorage */
        nullptr,                               /* aLoadGroup */
        nullptr,                               /* aCallbacks */
        nsIRequest::LOAD_BACKGROUND);
    NS_ENSURE_SUCCESS(rv, rv);

    nsCOMPtr<nsIInputStream> input;
    rv = channel->Open(getter_AddRefs(input));
    NS_ENSURE_SUCCESS(rv, NS_ERROR_INVALID_ARG);

    return NS_ReadInputStreamToString(input, aRetVal, -1);
  }
};

}  // namespace intl
}  // namespace mozilla

extern "C" {
nsresult L10nRegistryLoad(const nsACString* aPath,
                          mozilla::intl::ResourceLoader::Callback aCallback,
                          void* aClosure) {
  if (!aPath || !aCallback) {
    return NS_ERROR_INVALID_ARG;
  }

  return mozilla::intl::L10nRegistry::Load(*aPath, aCallback, aClosure);
}

nsresult L10nRegistryLoadSync(const nsACString* aPath, nsACString* aRetVal) {
  return mozilla::intl::L10nRegistry::LoadSync(*aPath, *aRetVal);
}
}
