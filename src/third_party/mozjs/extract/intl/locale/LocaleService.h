/* -*- Mode: C++; tab-width: 2; indent-tabs-mode:nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_intl_LocaleService_h__
#define mozilla_intl_LocaleService_h__

#include "nsIObserver.h"
#include "nsString.h"
#include "nsTArray.h"
#include "nsWeakReference.h"
#include "MozLocaleBindings.h"

#include "mozILocaleService.h"

namespace mozilla {
namespace intl {

/**
 * LocaleService is a manager of language negotiation in Gecko.
 *
 * It's intended to be the core place for collecting available and
 * requested languages and negotiating them to produce a fallback
 * chain of locales for the application.
 *
 * Client / Server
 *
 * LocaleService may operate in one of two modes:
 *
 *   server
 *     in the server mode, LocaleService is collecting and negotiating
 *     languages. It also subscribes to relevant observers.
 *     There should be at most one server per application instance.
 *
 *   client
 *     in the client mode, LocaleService is not responsible for collecting
 *     or reacting to any system changes. It still distributes information
 *     about locales, but internally, it gets information from the server
 * instance instead of collecting it on its own. This prevents any data
 * desynchronization and minimizes the cost of running the service.
 *
 *   In both modes, all get* methods should work the same way and all
 *   static methods are available.
 *
 *   In the server mode, other components may inform LocaleService about their
 *   status either via calls to set* methods or via observer events.
 *   In the client mode, only the process communication should provide data
 *   to the LocaleService.
 *
 *   At the moment desktop apps use the parent process in the server mode, and
 *   content processes in the client mode.
 *
 * Locale / Language
 *
 * The terms `Locale ID` and `Language ID` are used slightly differently
 * by different organizations. Mozilla uses the term `Language ID` to describe
 * a string that contains information about the language itself, script,
 * region and variant. For example "en-Latn-US-mac" is a correct Language ID.
 *
 * Locale ID contains a Language ID plus a number of extension tags that
 * contain information that go beyond language inforamation such as
 * preferred currency, date/time formatting etc.
 *
 * An example of a Locale ID is `en-Latn-US-x-hc-h12-ca-gregory`
 *
 * At the moment we do not support full extension tag system, but we
 * try to be specific when naming APIs, so the service is for locales,
 * but we negotiate between languages etc.
 */
class LocaleService final : public mozILocaleService,
                            public nsIObserver,
                            public nsSupportsWeakReference {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIOBSERVER
  NS_DECL_MOZILOCALESERVICE

  /**
   * List of available language negotiation strategies.
   *
   * See the mozILocaleService.idl for detailed description of the
   * strategies.
   */
  static const int32_t kLangNegStrategyFiltering = 0;
  static const int32_t kLangNegStrategyMatching = 1;
  static const int32_t kLangNegStrategyLookup = 2;

  explicit LocaleService(bool aIsServer);

  /**
   * Create (if necessary) and return a raw pointer to the singleton instance.
   * Use this accessor in C++ code that just wants to call a method on the
   * instance, but does not need to hold a reference, as in
   *    nsAutoCString str;
   *    LocaleService::GetInstance()->GetAppLocaleAsLangTag(str);
   */
  static LocaleService* GetInstance();

  /**
   * Return an addRef'd pointer to the singleton instance. This is used by the
   * XPCOM constructor that exists to support usage from JS.
   */
  static already_AddRefed<LocaleService> GetInstanceAddRefed() {
    return RefPtr<LocaleService>(GetInstance()).forget();
  }

  /**
   * Canonicalize a Unicode Language Identifier string.
   *
   * The operation is:
   *   * Normalizing casing (`eN-Us-Windows` -> `en-US-windows`)
   *   * Switching `_` to `-` (`en_US` -> `en-US`)
   *   * Rejecting invalid identifiers (`e21-X` sets aLocale to `und` and
   * returns false)
   *   * Normalizing Mozilla's `ja-JP-mac` to `ja-JP-macos`
   *   * Cutting off POSIX dot postfix (`en-US.utf8` -> `en-US`)
   *
   * This operation should be used on any external input before
   * it gets used in internal operations.
   */
  static bool CanonicalizeLanguageId(nsACString& aLocale) {
    return ffi::unic_langid_canonicalize(&aLocale);
  }
  /**
   * This method should only be called in the client mode.
   *
   * It replaces all the language negotiation and is supposed to be called
   * in order to bring the client LocaleService in sync with the server
   * LocaleService.
   *
   * Currently, it's called by the IPC code.
   */
  void AssignAppLocales(const nsTArray<nsCString>& aAppLocales);
  void AssignRequestedLocales(const nsTArray<nsCString>& aRequestedLocales);

  /**
   * Those two functions allow to trigger cache invalidation on one of the
   * three cached values.
   *
   * In most cases, the functions will be called by the observer in
   * LocaleService itself, but in a couple special cases, we have the
   * other component call this manually instead of sending a global event.
   *
   * If the result differs from the previous list, it will additionally
   * trigger a corresponding event
   *
   * This code should be called only in the server mode..
   */
  void RequestedLocalesChanged();
  void LocalesChanged();

  /**
   * This function keeps the pref setting updated.
   */
  void WebExposedLocalesChanged();

  /**
   * Returns whether the locale is RTL.
   */
  static bool IsLocaleRTL(const nsACString& aLocale);

  /**
   * Returns whether the current app locale is RTL.
   *
   * This method respects this override:
   *  - `intl.l10n.pseudo`
   */
  bool IsAppLocaleRTL();

  static bool LanguagesMatch(const nsACString& aRequested,
                             const nsACString& aAvailable);

  bool IsServer();

 private:
  void NegotiateAppLocales(nsTArray<nsCString>& aRetVal);

  void InitPackagedLocales();

  void RemoveObservers();

  virtual ~LocaleService() = default;

  nsAutoCStringN<16> mDefaultLocale;
  nsTArray<nsCString> mAppLocales;
  nsTArray<nsCString> mRequestedLocales;
  nsTArray<nsCString> mAvailableLocales;
  nsTArray<nsCString> mPackagedLocales;
  nsTArray<nsCString> mWebExposedLocales;
  const bool mIsServer;

  static StaticRefPtr<LocaleService> sInstance;
};
}  // namespace intl
}  // namespace mozilla

#endif /* mozilla_intl_LocaleService_h__ */
