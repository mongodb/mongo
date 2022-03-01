/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsStringBundle.h"
#include "nsID.h"
#include "nsString.h"
#include "nsIStringBundle.h"
#include "nsStringBundleService.h"
#include "nsArrayEnumerator.h"
#include "nscore.h"
#include "nsMemory.h"
#include "nsNetUtil.h"
#include "nsComponentManagerUtils.h"
#include "nsServiceManagerUtils.h"
#include "nsIChannel.h"
#include "nsIInputStream.h"
#include "nsIURI.h"
#include "nsIObserverService.h"
#include "nsCOMArray.h"
#include "nsTextFormatter.h"
#include "nsErrorService.h"
#include "nsContentUtils.h"
#include "nsPersistentProperties.h"
#include "nsQueryObject.h"
#include "nsSimpleEnumerator.h"
#include "nsStringStream.h"
#include "mozilla/BinarySearch.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/ResultExtensions.h"
#include "mozilla/URLPreloader.h"
#include "mozilla/ResultExtensions.h"
#include "mozilla/dom/ContentParent.h"
#include "mozilla/dom/ipc/SharedStringMap.h"

// for async loading
#ifdef ASYNC_LOADING
#  include "nsIBinaryInputStream.h"
#  include "nsIStringStream.h"
#endif

using namespace mozilla;

using mozilla::dom::ContentParent;
using mozilla::dom::StringBundleDescriptor;
using mozilla::dom::ipc::SharedStringMap;
using mozilla::dom::ipc::SharedStringMapBuilder;
using mozilla::ipc::FileDescriptor;

/**
 * A set of string bundle URLs which are loaded by content processes, and
 * should be allocated in a shared memory region, and then sent to content
 * processes.
 *
 * Note: This layout is chosen to avoid having to create a separate char*
 * array pointing to the string constant values, which would require
 * per-process relocations. The second array size is the length of the longest
 * URL plus its null terminator. Shorter strings are null padded to this
 * length.
 *
 * This should be kept in sync with the similar array in nsContentUtils.cpp,
 * and updated with any other property files which need to be loaded in all
 * content processes.
 */
static const char kContentBundles[][52] = {
    "chrome://branding/locale/brand.properties",
    "chrome://global/locale/commonDialogs.properties",
    "chrome://global/locale/css.properties",
    "chrome://global/locale/dom/dom.properties",
    "chrome://global/locale/layout/HtmlForm.properties",
    "chrome://global/locale/layout/htmlparser.properties",
    "chrome://global/locale/layout_errors.properties",
    "chrome://global/locale/mathml/mathml.properties",
    "chrome://global/locale/printing.properties",
    "chrome://global/locale/security/csp.properties",
    "chrome://global/locale/security/security.properties",
    "chrome://global/locale/svg/svg.properties",
    "chrome://global/locale/xul.properties",
    "chrome://necko/locale/necko.properties",
};

static bool IsContentBundle(const nsCString& aUrl) {
  size_t index;
  return BinarySearchIf(
      kContentBundles, 0, MOZ_ARRAY_LENGTH(kContentBundles),
      [&](const char* aElem) { return aUrl.Compare(aElem); }, &index);
}

namespace {

#define STRINGBUNDLEPROXY_IID                        \
  {                                                  \
    0x537cf21b, 0x99fc, 0x4002, {                    \
      0x9e, 0xec, 0x97, 0xbe, 0x4d, 0xe0, 0xb3, 0xdc \
    }                                                \
  }

/**
 * A simple proxy class for a string bundle instance which will be replaced by
 * a different implementation later in the session.
 *
 * This is used when creating string bundles which should use shared memory,
 * but the content process has not yet received their shared memory buffer.
 * When the shared memory variant becomes available, this proxy is retarged to
 * that instance, and the original non-shared instance is destroyed.
 *
 * At that point, the cache entry for the proxy is replaced with the shared
 * memory instance, and callers which already have an instance of the proxy
 * are redirected to the new instance.
 */
class StringBundleProxy : public nsIStringBundle {
  NS_DECL_THREADSAFE_ISUPPORTS

  NS_DECLARE_STATIC_IID_ACCESSOR(STRINGBUNDLEPROXY_IID)

  explicit StringBundleProxy(already_AddRefed<nsIStringBundle> aTarget)
      : mMutex("StringBundleProxy::mMutex"), mTarget(aTarget) {}

  NS_FORWARD_NSISTRINGBUNDLE(Target()->);

  void Retarget(nsIStringBundle* aTarget) {
    MutexAutoLock automon(mMutex);
    mTarget = aTarget;
  }

  size_t SizeOfIncludingThis(
      mozilla::MallocSizeOf aMallocSizeOf) const override {
    return aMallocSizeOf(this);
  }

  size_t SizeOfIncludingThisIfUnshared(
      mozilla::MallocSizeOf aMallocSizeOf) const override {
    return mRefCnt == 1 ? SizeOfIncludingThis(aMallocSizeOf) : 0;
  }

 protected:
  virtual ~StringBundleProxy() = default;

 private:
  Mutex mMutex;
  nsCOMPtr<nsIStringBundle> mTarget;

  // Atomically reads mTarget and returns a strong reference to it. This
  // allows for safe multi-threaded use when the proxy may be retargetted by
  // the main thread during access.
  nsCOMPtr<nsIStringBundle> Target() {
    MutexAutoLock automon(mMutex);
    return mTarget;
  }
};

NS_DEFINE_STATIC_IID_ACCESSOR(StringBundleProxy, STRINGBUNDLEPROXY_IID)

NS_IMPL_ISUPPORTS(StringBundleProxy, nsIStringBundle, StringBundleProxy)

#define SHAREDSTRINGBUNDLE_IID                       \
  {                                                  \
    0x7a8df5f7, 0x9e50, 0x44f6, {                    \
      0xbf, 0x89, 0xc7, 0xad, 0x6c, 0x17, 0xf8, 0x5f \
    }                                                \
  }

/**
 * A string bundle backed by a read-only, shared memory buffer. This should
 * only be used for string bundles which are used in child processes.
 *
 * Important: The memory allocated by these string bundles will never be freed
 * before process shutdown, per the restrictions in SharedStringMap.h, so they
 * should never be used for short-lived bundles.
 */
class SharedStringBundle final : public nsStringBundleBase {
 public:
  /**
   * Initialize the string bundle with a file descriptor pointing to a
   * pre-populated key-value store for this string bundle. This should only be
   * called in child processes, for bundles initially created in the parent
   * process.
   */
  void SetMapFile(const FileDescriptor& aFile, size_t aSize);

  NS_DECL_ISUPPORTS_INHERITED
  NS_DECLARE_STATIC_IID_ACCESSOR(SHAREDSTRINGBUNDLE_IID)

  nsresult LoadProperties() override;

  /**
   * Returns a copy of the file descriptor pointing to the shared memory
   * key-values tore for this string bundle. This should only be called in the
   * parent process, and may be used to send shared string bundles to child
   * processes.
   */
  FileDescriptor CloneFileDescriptor() const {
    MOZ_ASSERT(XRE_IsParentProcess());
    if (mMapFile.isSome()) {
      return mMapFile.ref();
    }
    return mStringMap->CloneFileDescriptor();
  }

  size_t MapSize() const {
    if (mMapFile.isSome()) {
      return mMapSize;
    }
    if (mStringMap) {
      return mStringMap->MapSize();
    }
    return 0;
  }

  bool Initialized() const { return mStringMap || mMapFile.isSome(); }

  StringBundleDescriptor GetDescriptor() const {
    MOZ_ASSERT(Initialized());

    StringBundleDescriptor descriptor;
    descriptor.bundleURL() = BundleURL();
    descriptor.mapFile() = CloneFileDescriptor();
    descriptor.mapSize() = MapSize();
    return descriptor;
  }

  size_t SizeOfIncludingThis(
      mozilla::MallocSizeOf aMallocSizeOf) const override;

  static SharedStringBundle* Cast(nsIStringBundle* aStringBundle) {
    return static_cast<SharedStringBundle*>(aStringBundle);
  }

 protected:
  friend class nsStringBundleBase;

  explicit SharedStringBundle(const char* aURLSpec)
      : nsStringBundleBase(aURLSpec) {}

  ~SharedStringBundle() override = default;

  nsresult GetStringImpl(const nsACString& aName, nsAString& aResult) override;

  nsresult GetSimpleEnumerationImpl(nsISimpleEnumerator** elements) override;

 private:
  RefPtr<SharedStringMap> mStringMap;

  Maybe<FileDescriptor> mMapFile;
  size_t mMapSize;
};

NS_DEFINE_STATIC_IID_ACCESSOR(SharedStringBundle, SHAREDSTRINGBUNDLE_IID)

class StringMapEnumerator final : public nsSimpleEnumerator {
 public:
  NS_DECL_NSISIMPLEENUMERATOR

  explicit StringMapEnumerator(SharedStringMap* aStringMap)
      : mStringMap(aStringMap) {}

  const nsID& DefaultInterface() override {
    return NS_GET_IID(nsIPropertyElement);
  }

 protected:
  virtual ~StringMapEnumerator() = default;

 private:
  RefPtr<SharedStringMap> mStringMap;

  uint32_t mIndex = 0;
};

template <typename T, typename... Args>
already_AddRefed<T> MakeBundle(Args... args) {
  return nsStringBundleBase::Create<T>(args...);
}

template <typename T, typename... Args>
RefPtr<T> MakeBundleRefPtr(Args... args) {
  return nsStringBundleBase::Create<T>(args...);
}

}  // anonymous namespace

NS_IMPL_ISUPPORTS(nsStringBundleBase, nsIStringBundle, nsIMemoryReporter)

NS_IMPL_ISUPPORTS_INHERITED0(nsStringBundle, nsStringBundleBase)
NS_IMPL_ISUPPORTS_INHERITED(SharedStringBundle, nsStringBundleBase,
                            SharedStringBundle)

nsStringBundleBase::nsStringBundleBase(const char* aURLSpec)
    : mPropertiesURL(aURLSpec),
      mMutex("nsStringBundle.mMutex"),
      mAttemptedLoad(false),
      mLoaded(false) {}

nsStringBundleBase::~nsStringBundleBase() {
  UnregisterWeakMemoryReporter(this);
}

void nsStringBundleBase::RegisterMemoryReporter() {
  RegisterWeakMemoryReporter(this);
}

template <typename T, typename... Args>
/* static */
already_AddRefed<T> nsStringBundleBase::Create(Args... args) {
  RefPtr<T> bundle = new T(args...);
  bundle->RegisterMemoryReporter();
  return bundle.forget();
}

nsStringBundle::nsStringBundle(const char* aURLSpec)
    : nsStringBundleBase(aURLSpec) {}

nsStringBundle::~nsStringBundle() = default;

NS_IMETHODIMP
nsStringBundleBase::AsyncPreload() {
  return NS_DispatchToCurrentThreadQueue(
      NewIdleRunnableMethod("nsStringBundleBase::LoadProperties", this,
                            &nsStringBundleBase::LoadProperties),
      EventQueuePriority::Idle);
}

size_t nsStringBundle::SizeOfIncludingThis(
    mozilla::MallocSizeOf aMallocSizeOf) const {
  size_t n = 0;
  if (mProps) {
    n += mProps->SizeOfIncludingThis(aMallocSizeOf);
  }
  return aMallocSizeOf(this) + n;
}

size_t nsStringBundleBase::SizeOfIncludingThisIfUnshared(
    mozilla::MallocSizeOf aMallocSizeOf) const {
  if (mRefCnt == 1) {
    return SizeOfIncludingThis(aMallocSizeOf);
  } else {
    return 0;
  }
}

size_t SharedStringBundle::SizeOfIncludingThis(
    mozilla::MallocSizeOf aMallocSizeOf) const {
  size_t n = 0;
  if (mStringMap) {
    n += aMallocSizeOf(mStringMap);
  }
  return aMallocSizeOf(this) + n;
}

NS_IMETHODIMP
nsStringBundleBase::CollectReports(nsIHandleReportCallback* aHandleReport,
                                   nsISupports* aData, bool aAnonymize) {
  // String bundle URLs are always local, and part of the distribution.
  // There's no need to anonymize.
  nsAutoCStringN<64> escapedURL(mPropertiesURL);
  escapedURL.ReplaceChar('/', '\\');

  size_t sharedSize = 0;
  size_t heapSize = SizeOfIncludingThis(MallocSizeOf);

  nsAutoCStringN<256> path("explicit/string-bundles/");
  if (RefPtr<SharedStringBundle> shared = do_QueryObject(this)) {
    path.AppendLiteral("SharedStringBundle");
    if (XRE_IsParentProcess()) {
      sharedSize = shared->MapSize();
    }
  } else {
    path.AppendLiteral("nsStringBundle");
  }

  path.AppendLiteral("(url=\"");
  path.Append(escapedURL);

  // Note: The memory reporter service holds a strong reference to reporters
  // while collecting reports, so we want to ignore the extra ref in reports.
  path.AppendLiteral("\", shared=");
  path.AppendASCII(mRefCnt > 2 ? "true" : "false");
  path.AppendLiteral(", refCount=");
  path.AppendInt(uint32_t(mRefCnt - 1));

  if (sharedSize) {
    path.AppendLiteral(", sharedMemorySize=");
    path.AppendInt(uint32_t(sharedSize));
  }

  path.AppendLiteral(")");

  constexpr auto desc =
      "A StringBundle instance representing the data in a (probably "
      "localized) .properties file. Data may be shared between "
      "processes."_ns;

  aHandleReport->Callback(""_ns, path, KIND_HEAP, UNITS_BYTES, heapSize, desc,
                          aData);

  if (sharedSize) {
    path.ReplaceLiteral(0, sizeof("explicit/") - 1, "shared-");

    aHandleReport->Callback(""_ns, path, KIND_OTHER, UNITS_BYTES, sharedSize,
                            desc, aData);
  }

  return NS_OK;
}

nsresult nsStringBundleBase::ParseProperties(nsIPersistentProperties** aProps) {
  // this is different than mLoaded, because we only want to attempt
  // to load once
  // we only want to load once, but if we've tried once and failed,
  // continue to throw an error!
  if (mAttemptedLoad) {
    if (mLoaded) return NS_OK;

    return NS_ERROR_UNEXPECTED;
  }

  MOZ_ASSERT(NS_IsMainThread(),
             "String bundles must be initialized on the main thread "
             "before they may be used off-main-thread");

  mAttemptedLoad = true;

  nsresult rv;

  // do it synchronously
  nsCOMPtr<nsIURI> uri;
  rv = NS_NewURI(getter_AddRefs(uri), mPropertiesURL);
  if (NS_FAILED(rv)) return rv;

  // whitelist check for local schemes
  nsCString scheme;
  uri->GetScheme(scheme);
  if (!scheme.EqualsLiteral("chrome") && !scheme.EqualsLiteral("jar") &&
      !scheme.EqualsLiteral("resource") && !scheme.EqualsLiteral("file") &&
      !scheme.EqualsLiteral("data")) {
    return NS_ERROR_ABORT;
  }

  nsCOMPtr<nsIInputStream> in;

  auto result = URLPreloader::ReadURI(uri);
  if (result.isOk()) {
    MOZ_TRY(NS_NewCStringInputStream(getter_AddRefs(in), result.unwrap()));
  } else {
    nsCOMPtr<nsIChannel> channel;
    rv = NS_NewChannel(getter_AddRefs(channel), uri,
                       nsContentUtils::GetSystemPrincipal(),
                       nsILoadInfo::SEC_ALLOW_CROSS_ORIGIN_SEC_CONTEXT_IS_NULL,
                       nsIContentPolicy::TYPE_OTHER);

    if (NS_FAILED(rv)) return rv;

    // It's a string bundle.  We expect a text/plain type, so set that as hint
    channel->SetContentType("text/plain"_ns);

    rv = channel->Open(getter_AddRefs(in));
    if (NS_FAILED(rv)) return rv;
  }

  auto props = MakeRefPtr<nsPersistentProperties>();

  mAttemptedLoad = true;

  MOZ_TRY(props->Load(in));
  props.forget(aProps);

  mLoaded = true;
  return NS_OK;
}

nsresult nsStringBundle::LoadProperties() {
  // Something such as Necko might use string bundle after ClearOnShutdown is
  // called. LocaleService etc is already down, so we cannot get bundle data.
  if (PastShutdownPhase(ShutdownPhase::XPCOMShutdown)) {
    return NS_ERROR_ILLEGAL_DURING_SHUTDOWN;
  }

  if (mProps) {
    return NS_OK;
  }
  return ParseProperties(getter_AddRefs(mProps));
}

nsresult SharedStringBundle::LoadProperties() {
  if (mStringMap) return NS_OK;

  if (mMapFile.isSome()) {
    mStringMap = new SharedStringMap(mMapFile.ref(), mMapSize);
    mMapFile.reset();
    return NS_OK;
  }

  MOZ_ASSERT(NS_IsMainThread(),
             "String bundles must be initialized on the main thread "
             "before they may be used off-main-thread");

  // We can't access the locale service after shutdown has started, which
  // means we can't attempt to load chrome: locale resources (which most of
  // our string bundles come from). Since shared string bundles won't be
  // useful after shutdown has started anyway (and we almost certainly got
  // here from a pre-load attempt in an idle task), just bail out.
  if (PastShutdownPhase(ShutdownPhase::XPCOMShutdown)) {
    return NS_ERROR_ILLEGAL_DURING_SHUTDOWN;
  }

  // We should only populate shared memory string bundles in the parent
  // process. Instances in the child process should always be instantiated
  // with a shared memory file descriptor sent from the parent.
  MOZ_ASSERT(XRE_IsParentProcess());

  nsCOMPtr<nsIPersistentProperties> props;
  MOZ_TRY(ParseProperties(getter_AddRefs(props)));

  SharedStringMapBuilder builder;

  nsCOMPtr<nsISimpleEnumerator> iter;
  MOZ_TRY(props->Enumerate(getter_AddRefs(iter)));
  bool hasMore;
  while (NS_SUCCEEDED(iter->HasMoreElements(&hasMore)) && hasMore) {
    nsCOMPtr<nsISupports> next;
    MOZ_TRY(iter->GetNext(getter_AddRefs(next)));

    nsresult rv;
    nsCOMPtr<nsIPropertyElement> elem = do_QueryInterface(next, &rv);
    MOZ_TRY(rv);

    nsCString key;
    nsString value;
    MOZ_TRY(elem->GetKey(key));
    MOZ_TRY(elem->GetValue(value));

    builder.Add(key, value);
  }

  mStringMap = new SharedStringMap(std::move(builder));

  ContentParent::BroadcastStringBundle(GetDescriptor());

  return NS_OK;
}

void SharedStringBundle::SetMapFile(const FileDescriptor& aFile, size_t aSize) {
  MOZ_ASSERT(XRE_IsContentProcess());
  mStringMap = nullptr;
  mMapFile.emplace(aFile);
  mMapSize = aSize;
}

NS_IMETHODIMP
nsStringBundleBase::GetStringFromID(int32_t aID, nsAString& aResult) {
  nsAutoCString idStr;
  idStr.AppendInt(aID, 10);
  return GetStringFromName(idStr.get(), aResult);
}

NS_IMETHODIMP
nsStringBundleBase::GetStringFromAUTF8Name(const nsACString& aName,
                                           nsAString& aResult) {
  return GetStringFromName(PromiseFlatCString(aName).get(), aResult);
}

NS_IMETHODIMP
nsStringBundleBase::GetStringFromName(const char* aName, nsAString& aResult) {
  NS_ENSURE_ARG_POINTER(aName);

  MutexAutoLock autolock(mMutex);

  return GetStringImpl(nsDependentCString(aName), aResult);
}

nsresult nsStringBundle::GetStringImpl(const nsACString& aName,
                                       nsAString& aResult) {
  MOZ_TRY(LoadProperties());

  return mProps->GetStringProperty(aName, aResult);
}

nsresult SharedStringBundle::GetStringImpl(const nsACString& aName,
                                           nsAString& aResult) {
  MOZ_TRY(LoadProperties());

  if (mStringMap->Get(PromiseFlatCString(aName), aResult)) {
    return NS_OK;
  }
  return NS_ERROR_FAILURE;
}

NS_IMETHODIMP
nsStringBundleBase::FormatStringFromID(int32_t aID,
                                       const nsTArray<nsString>& aParams,
                                       nsAString& aResult) {
  nsAutoCString idStr;
  idStr.AppendInt(aID, 10);
  return FormatStringFromName(idStr.get(), aParams, aResult);
}

// this function supports at most 10 parameters.. see below for why
NS_IMETHODIMP
nsStringBundleBase::FormatStringFromAUTF8Name(const nsACString& aName,
                                              const nsTArray<nsString>& aParams,
                                              nsAString& aResult) {
  return FormatStringFromName(PromiseFlatCString(aName).get(), aParams,
                              aResult);
}

// this function supports at most 10 parameters.. see below for why
NS_IMETHODIMP
nsStringBundleBase::FormatStringFromName(const char* aName,
                                         const nsTArray<nsString>& aParams,
                                         nsAString& aResult) {
  NS_ASSERTION(!aParams.IsEmpty(),
               "FormatStringFromName() without format parameters: use "
               "GetStringFromName() instead");

  nsAutoString formatStr;
  nsresult rv = GetStringFromName(aName, formatStr);
  if (NS_FAILED(rv)) return rv;

  return FormatString(formatStr.get(), aParams, aResult);
}

NS_IMETHODIMP
nsStringBundleBase::GetSimpleEnumeration(nsISimpleEnumerator** aElements) {
  NS_ENSURE_ARG_POINTER(aElements);

  return GetSimpleEnumerationImpl(aElements);
}

nsresult nsStringBundle::GetSimpleEnumerationImpl(
    nsISimpleEnumerator** elements) {
  MOZ_TRY(LoadProperties());

  return mProps->Enumerate(elements);
}

nsresult SharedStringBundle::GetSimpleEnumerationImpl(
    nsISimpleEnumerator** aEnumerator) {
  MOZ_TRY(LoadProperties());

  auto iter = MakeRefPtr<StringMapEnumerator>(mStringMap);
  iter.forget(aEnumerator);
  return NS_OK;
}

NS_IMETHODIMP
StringMapEnumerator::HasMoreElements(bool* aHasMore) {
  *aHasMore = mIndex < mStringMap->Count();
  return NS_OK;
}

NS_IMETHODIMP
StringMapEnumerator::GetNext(nsISupports** aNext) {
  if (mIndex >= mStringMap->Count()) {
    return NS_ERROR_FAILURE;
  }

  auto elem = MakeRefPtr<nsPropertyElement>(mStringMap->GetKeyAt(mIndex),
                                            mStringMap->GetValueAt(mIndex));

  elem.forget(aNext);

  mIndex++;
  return NS_OK;
}

nsresult nsStringBundleBase::FormatString(const char16_t* aFormatStr,
                                          const nsTArray<nsString>& aParams,
                                          nsAString& aResult) {
  auto length = aParams.Length();
  NS_ENSURE_ARG(length <= 10);  // enforce 10-parameter limit

  // implementation note: you would think you could use vsmprintf
  // to build up an arbitrary length array.. except that there
  // is no way to build up a va_list at runtime!
  // Don't believe me? See:
  //   http://www.eskimo.com/~scs/C-faq/q15.13.html
  // -alecf
  nsTextFormatter::ssprintf(aResult, aFormatStr,
                            length >= 1 ? aParams[0].get() : nullptr,
                            length >= 2 ? aParams[1].get() : nullptr,
                            length >= 3 ? aParams[2].get() : nullptr,
                            length >= 4 ? aParams[3].get() : nullptr,
                            length >= 5 ? aParams[4].get() : nullptr,
                            length >= 6 ? aParams[5].get() : nullptr,
                            length >= 7 ? aParams[6].get() : nullptr,
                            length >= 8 ? aParams[7].get() : nullptr,
                            length >= 9 ? aParams[8].get() : nullptr,
                            length >= 10 ? aParams[9].get() : nullptr);

  return NS_OK;
}

/////////////////////////////////////////////////////////////////////////////////////////

#define MAX_CACHED_BUNDLES 16

struct bundleCacheEntry_t final : public LinkedListElement<bundleCacheEntry_t> {
  nsCString mHashKey;
  nsCOMPtr<nsIStringBundle> mBundle;

  MOZ_COUNTED_DEFAULT_CTOR(bundleCacheEntry_t)

  MOZ_COUNTED_DTOR(bundleCacheEntry_t)
};

nsStringBundleService::nsStringBundleService()
    : mBundleMap(MAX_CACHED_BUNDLES) {
  mErrorService = nsErrorService::GetOrCreate();
  MOZ_ALWAYS_TRUE(mErrorService);
}

NS_IMPL_ISUPPORTS(nsStringBundleService, nsIStringBundleService, nsIObserver,
                  nsISupportsWeakReference, nsIMemoryReporter)

nsStringBundleService::~nsStringBundleService() {
  UnregisterWeakMemoryReporter(this);
  flushBundleCache(/* ignoreShared = */ false);
}

nsresult nsStringBundleService::Init() {
  nsCOMPtr<nsIObserverService> os = mozilla::services::GetObserverService();
  if (os) {
    os->AddObserver(this, "memory-pressure", true);
    os->AddObserver(this, "profile-do-change", true);
    os->AddObserver(this, "chrome-flush-caches", true);
    os->AddObserver(this, "intl:app-locales-changed", true);
  }

  RegisterWeakMemoryReporter(this);

  return NS_OK;
}

size_t nsStringBundleService::SizeOfIncludingThis(
    mozilla::MallocSizeOf aMallocSizeOf) const {
  size_t n = mBundleMap.ShallowSizeOfExcludingThis(aMallocSizeOf);
  for (const auto& data : mBundleMap.Values()) {
    n += aMallocSizeOf(data);
    n += data->mHashKey.SizeOfExcludingThisIfUnshared(aMallocSizeOf);
  }
  return aMallocSizeOf(this) + n;
}

NS_IMETHODIMP
nsStringBundleService::Observe(nsISupports* aSubject, const char* aTopic,
                               const char16_t* aSomeData) {
  if (strcmp("profile-do-change", aTopic) == 0 ||
      strcmp("chrome-flush-caches", aTopic) == 0 ||
      strcmp("intl:app-locales-changed", aTopic) == 0) {
    flushBundleCache(/* ignoreShared = */ false);
  } else if (strcmp("memory-pressure", aTopic) == 0) {
    flushBundleCache(/* ignoreShared = */ true);
  }

  return NS_OK;
}

void nsStringBundleService::flushBundleCache(bool ignoreShared) {
  LinkedList<bundleCacheEntry_t> newList;

  while (!mBundleCache.isEmpty()) {
    UniquePtr<bundleCacheEntry_t> entry(mBundleCache.popFirst());
    auto* bundle = nsStringBundleBase::Cast(entry->mBundle);

    if (ignoreShared && bundle->IsShared()) {
      newList.insertBack(entry.release());
    } else {
      mBundleMap.Remove(entry->mHashKey);
    }
  }

  mBundleCache = std::move(newList);
}

NS_IMETHODIMP
nsStringBundleService::FlushBundles() {
  flushBundleCache(/* ignoreShared = */ false);
  return NS_OK;
}

void nsStringBundleService::SendContentBundles(
    ContentParent* aContentParent) const {
  nsTArray<StringBundleDescriptor> bundles;

  for (auto* entry : mSharedBundles) {
    auto bundle = SharedStringBundle::Cast(entry->mBundle);

    if (bundle->Initialized()) {
      bundles.AppendElement(bundle->GetDescriptor());
    }
  }

  Unused << aContentParent->SendRegisterStringBundles(std::move(bundles));
}

void nsStringBundleService::RegisterContentBundle(
    const nsCString& aBundleURL, const FileDescriptor& aMapFile,
    size_t aMapSize) {
  RefPtr<StringBundleProxy> proxy;

  bundleCacheEntry_t* cacheEntry = mBundleMap.Get(aBundleURL);
  if (cacheEntry) {
    if (RefPtr<SharedStringBundle> shared =
            do_QueryObject(cacheEntry->mBundle)) {
      return;
    }

    proxy = do_QueryObject(cacheEntry->mBundle);
    MOZ_ASSERT(proxy);
    cacheEntry->remove();
    delete cacheEntry;
  }

  auto bundle = MakeBundleRefPtr<SharedStringBundle>(aBundleURL.get());
  bundle->SetMapFile(aMapFile, aMapSize);

  if (proxy) {
    proxy->Retarget(bundle);
  }

  cacheEntry = insertIntoCache(bundle.forget(), aBundleURL);
  mSharedBundles.insertBack(cacheEntry);
}

void nsStringBundleService::getStringBundle(const char* aURLSpec,
                                            nsIStringBundle** aResult) {
  nsDependentCString key(aURLSpec);
  bundleCacheEntry_t* cacheEntry = mBundleMap.Get(key);

  RefPtr<SharedStringBundle> shared;

  if (cacheEntry) {
    // Remove the entry from the list so it can be re-inserted at the back.
    cacheEntry->remove();

    shared = do_QueryObject(cacheEntry->mBundle);
  } else {
    nsCOMPtr<nsIStringBundle> bundle;
    bool isContent = IsContentBundle(key);
    if (!isContent || !XRE_IsParentProcess()) {
      bundle = MakeBundle<nsStringBundle>(aURLSpec);
    }

    // If this is a bundle which is used by the content processes, we want to
    // load it into a shared memory region.
    //
    // If we're in the parent process, just create a new SharedStringBundle,
    // and populate it from the properties file.
    //
    // If we're in a child process, the fact that the bundle is not already in
    // the cache means that we haven't received its shared memory descriptor
    // from the parent yet. There's not much we can do about that besides
    // wait, but we need to return a bundle now. So instead of a shared memory
    // bundle, we create a temporary proxy, which points to a non-shared
    // bundle initially, and is retarged to a shared memory bundle when it
    // becomes available.
    if (isContent) {
      if (XRE_IsParentProcess()) {
        shared = MakeBundle<SharedStringBundle>(aURLSpec);
        bundle = shared;
      } else {
        bundle = new StringBundleProxy(bundle.forget());
      }
    }

    cacheEntry = insertIntoCache(bundle.forget(), key);
  }

  if (shared) {
    mSharedBundles.insertBack(cacheEntry);
  } else {
    mBundleCache.insertBack(cacheEntry);
  }

  // finally, return the value
  *aResult = cacheEntry->mBundle;
  NS_ADDREF(*aResult);
}

UniquePtr<bundleCacheEntry_t> nsStringBundleService::evictOneEntry() {
  for (auto* entry : mBundleCache) {
    auto* bundle = nsStringBundleBase::Cast(entry->mBundle);
    if (!bundle->IsShared()) {
      entry->remove();
      mBundleMap.Remove(entry->mHashKey);
      return UniquePtr<bundleCacheEntry_t>(entry);
    }
  }
  return nullptr;
}

bundleCacheEntry_t* nsStringBundleService::insertIntoCache(
    already_AddRefed<nsIStringBundle> aBundle, const nsACString& aHashKey) {
  UniquePtr<bundleCacheEntry_t> cacheEntry;

  if (mBundleMap.Count() >= MAX_CACHED_BUNDLES) {
    cacheEntry = evictOneEntry();
  }

  if (!cacheEntry) {
    cacheEntry.reset(new bundleCacheEntry_t());
  }

  cacheEntry->mHashKey = aHashKey;
  cacheEntry->mBundle = aBundle;

  mBundleMap.InsertOrUpdate(cacheEntry->mHashKey, cacheEntry.get());

  return cacheEntry.release();
}

NS_IMETHODIMP
nsStringBundleService::CreateBundle(const char* aURLSpec,
                                    nsIStringBundle** aResult) {
  getStringBundle(aURLSpec, aResult);
  return NS_OK;
}

#define GLOBAL_PROPERTIES "chrome://global/locale/global-strres.properties"

nsresult nsStringBundleService::FormatWithBundle(
    nsIStringBundle* bundle, nsresult aStatus,
    const nsTArray<nsString>& argArray, nsAString& result) {
  nsresult rv;

  // try looking up the error message with the int key:
  uint16_t code = NS_ERROR_GET_CODE(aStatus);
  rv = bundle->FormatStringFromID(code, argArray, result);

  // If the int key fails, try looking up the default error message. E.g. print:
  //   An unknown error has occurred (0x804B0003).
  if (NS_FAILED(rv)) {
    AutoTArray<nsString, 1> otherArgArray;
    otherArgArray.AppendElement()->AppendInt(static_cast<uint32_t>(aStatus),
                                             16);
    uint16_t code = NS_ERROR_GET_CODE(NS_ERROR_FAILURE);
    rv = bundle->FormatStringFromID(code, otherArgArray, result);
  }

  return rv;
}

NS_IMETHODIMP
nsStringBundleService::FormatStatusMessage(nsresult aStatus,
                                           const char16_t* aStatusArg,
                                           nsAString& result) {
  nsresult rv;
  uint32_t i, argCount = 0;
  nsCOMPtr<nsIStringBundle> bundle;
  nsCString stringBundleURL;

  // XXX hack for mailnews who has already formatted their messages:
  if (aStatus == NS_OK && aStatusArg) {
    result.Assign(aStatusArg);
    return NS_OK;
  }

  if (aStatus == NS_OK) {
    return NS_ERROR_FAILURE;  // no message to format
  }

  // format the arguments:
  const nsDependentString args(aStatusArg);
  argCount = args.CountChar(char16_t('\n')) + 1;
  NS_ENSURE_ARG(argCount <= 10);  // enforce 10-parameter limit
  AutoTArray<nsString, 10> argArray;

  // convert the aStatusArg into an nsString array
  if (argCount == 1) {
    argArray.AppendElement(aStatusArg);
  } else if (argCount > 1) {
    int32_t offset = 0;
    for (i = 0; i < argCount; i++) {
      int32_t pos = args.FindChar('\n', offset);
      if (pos == -1) pos = args.Length();
      argArray.AppendElement(Substring(args, offset, pos - offset));
      offset = pos + 1;
    }
  }

  // find the string bundle for the error's module:
  rv = mErrorService->GetErrorStringBundle(NS_ERROR_GET_MODULE(aStatus),
                                           getter_Copies(stringBundleURL));
  if (NS_SUCCEEDED(rv)) {
    getStringBundle(stringBundleURL.get(), getter_AddRefs(bundle));
    rv = FormatWithBundle(bundle, aStatus, argArray, result);
  }
  if (NS_FAILED(rv)) {
    getStringBundle(GLOBAL_PROPERTIES, getter_AddRefs(bundle));
    rv = FormatWithBundle(bundle, aStatus, argArray, result);
  }

  return rv;
}
