/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_intl_SharedIntlData_h
#define builtin_intl_SharedIntlData_h

#include "mozilla/MemoryReporting.h"
#include "mozilla/UniquePtr.h"

#include <stddef.h>

#include "js/AllocPolicy.h"
#include "js/GCAPI.h"
#include "js/GCHashTable.h"
#include "js/Result.h"
#include "js/RootingAPI.h"
#include "js/Utility.h"
#include "vm/StringType.h"

namespace mozilla::intl {
class DateTimePatternGenerator;
}  // namespace mozilla::intl

namespace js {

class ArrayObject;

namespace intl {

/**
 * This deleter class exists so that mozilla::intl::DateTimePatternGenerator
 * can be a forward declaration, but still be used inside of a UniquePtr.
 */
class DateTimePatternGeneratorDeleter {
 public:
  void operator()(mozilla::intl::DateTimePatternGenerator* ptr);
};

/**
 * Stores Intl data which can be shared across compartments (but not contexts).
 *
 * Used for data which is expensive when computed repeatedly or is not
 * available through ICU.
 */
class SharedIntlData {
  struct LinearStringLookup {
    union {
      const JS::Latin1Char* latin1Chars;
      const char16_t* twoByteChars;
    };
    bool isLatin1;
    size_t length;
    JS::AutoCheckCannotGC nogc;
    HashNumber hash = 0;

    explicit LinearStringLookup(JSLinearString* string)
        : isLatin1(string->hasLatin1Chars()), length(string->length()) {
      if (isLatin1) {
        latin1Chars = string->latin1Chars(nogc);
      } else {
        twoByteChars = string->twoByteChars(nogc);
      }
    }

    LinearStringLookup(const char* chars, size_t length)
        : isLatin1(true), length(length) {
      latin1Chars = reinterpret_cast<const JS::Latin1Char*>(chars);
    }
  };

 public:
  /**
   * Information tracking the set of the supported time zone names, derived
   * from the IANA time zone database <https://www.iana.org/time-zones>.
   *
   * There are two kinds of IANA time zone names: Zone and Link (denoted as
   * such in database source files). Zone names are the canonical, preferred
   * name for a time zone, e.g. Asia/Kolkata. Link names simply refer to
   * target Zone names for their meaning, e.g. Asia/Calcutta targets
   * Asia/Kolkata. That a name is a Link doesn't *necessarily* reflect a
   * sense of deprecation: some Link names also exist partly for convenience,
   * e.g. UTC and GMT as Link names targeting the Zone name Etc/UTC.
   *
   * Two data sources determine the time zone names we support: those ICU
   * supports and IANA's zone information.
   *
   * Unfortunately the names ICU and IANA support, and their Link
   * relationships from name to target, aren't identical, so we can't simply
   * implicitly trust ICU's name handling. We must perform various
   * preprocessing of user-provided zone names and post-processing of
   * ICU-provided zone names to implement ECMA-402's IANA-consistent behavior.
   *
   * Also see <https://ssl.icu-project.org/trac/ticket/12044> and
   * <http://unicode.org/cldr/trac/ticket/9892>.
   */

  using TimeZoneName = JSAtom*;

  struct TimeZoneHasher {
    struct Lookup : LinearStringLookup {
      explicit Lookup(JSLinearString* timeZone);
    };

    static js::HashNumber hash(const Lookup& lookup) { return lookup.hash; }
    static bool match(TimeZoneName key, const Lookup& lookup);
  };

  using TimeZoneSet =
      GCHashSet<TimeZoneName, TimeZoneHasher, SystemAllocPolicy>;
  using TimeZoneMap =
      GCHashMap<TimeZoneName, TimeZoneName, TimeZoneHasher, SystemAllocPolicy>;

 private:
  /**
   * As a threshold matter, available time zones are those time zones ICU
   * supports, via ucal_openTimeZones. But ICU supports additional non-IANA
   * time zones described in intl/icu/source/tools/tzcode/icuzones (listed in
   * IntlTimeZoneData.cpp's |legacyICUTimeZones|) for its own backwards
   * compatibility purposes. This set consists of ICU's supported time zones,
   * minus all backwards-compatibility time zones.
   */
  TimeZoneSet availableTimeZones;

  /**
   * IANA treats some time zone names as Zones, that ICU instead treats as
   * Links. For example, IANA considers "America/Indiana/Indianapolis" to be
   * a Zone and "America/Fort_Wayne" a Link that targets it, but ICU
   * considers the former a Link that targets "America/Indianapolis" (which
   * IANA treats as a Link).
   *
   * ECMA-402 requires that we respect IANA data, so if we're asked to
   * canonicalize a time zone name in this set, we must *not* return ICU's
   * canonicalization.
   */
  TimeZoneSet ianaZonesTreatedAsLinksByICU;

  /**
   * IANA treats some time zone names as Links to one target, that ICU
   * instead treats as either Zones, or Links to different targets. An
   * example of the former is "Asia/Calcutta, which IANA assigns the target
   * "Asia/Kolkata" but ICU considers its own Zone. An example of the latter
   * is "America/Virgin", which IANA assigns the target
   * "America/Port_of_Spain" but ICU assigns the target "America/St_Thomas".
   *
   * ECMA-402 requires that we respect IANA data, so if we're asked to
   * canonicalize a time zone name that's a key in this map, we *must* return
   * the corresponding value and *must not* return ICU's canonicalization.
   */
  TimeZoneMap ianaLinksCanonicalizedDifferentlyByICU;

  bool timeZoneDataInitialized = false;

  /**
   * Precomputes the available time zone names, because it's too expensive to
   * call ucal_openTimeZones() repeatedly.
   */
  bool ensureTimeZones(JSContext* cx);

 public:
  /**
   * Returns the validated time zone name in |result|. If the input time zone
   * isn't a valid IANA time zone name, |result| remains unchanged.
   */
  bool validateTimeZoneName(JSContext* cx, JS::Handle<JSString*> timeZone,
                            JS::MutableHandle<JSAtom*> result);

  /**
   * Returns the canonical time zone name in |result|. If no canonical name
   * was found, |result| remains unchanged.
   *
   * This method only handles time zones which are canonicalized differently
   * by ICU when compared to IANA.
   */
  bool tryCanonicalizeTimeZoneConsistentWithIANA(
      JSContext* cx, JS::Handle<JSString*> timeZone,
      JS::MutableHandle<JSAtom*> result);

  /**
   * Returns an iterator over all available time zones supported by ICU. The
   * returned time zone names aren't canonicalized.
   */
  JS::Result<TimeZoneSet::Iterator> availableTimeZonesIteration(JSContext* cx);

 private:
  using Locale = JSAtom*;

  struct LocaleHasher {
    struct Lookup : LinearStringLookup {
      explicit Lookup(JSLinearString* locale);
      Lookup(const char* chars, size_t length);
    };

    static js::HashNumber hash(const Lookup& lookup) { return lookup.hash; }
    static bool match(Locale key, const Lookup& lookup);
  };

  using LocaleSet = GCHashSet<Locale, LocaleHasher, SystemAllocPolicy>;

  // Set of supported locales for all Intl service constructors except Collator,
  // which uses its own set.
  //
  // UDateFormat:
  // udat_[count,get]Available() return the same results as their
  // uloc_[count,get]Available() counterparts.
  //
  // UNumberFormatter:
  // unum_[count,get]Available() return the same results as their
  // uloc_[count,get]Available() counterparts.
  //
  // UListFormatter, UPluralRules, and URelativeDateTimeFormatter:
  // We're going to use ULocale availableLocales as per ICU recommendation:
  // https://unicode-org.atlassian.net/browse/ICU-12756
  LocaleSet supportedLocales;

  // ucol_[count,get]Available() return different results compared to
  // uloc_[count,get]Available(), we can't use |supportedLocales| here.
  LocaleSet collatorSupportedLocales;

  bool supportedLocalesInitialized = false;

  // CountAvailable and GetAvailable describe the signatures used for ICU API
  // to determine available locales for various functionality.
  using CountAvailable = int32_t (*)();
  using GetAvailable = const char* (*)(int32_t localeIndex);

  template <class AvailableLocales>
  static bool getAvailableLocales(JSContext* cx, LocaleSet& locales,
                                  const AvailableLocales& availableLocales);

  /**
   * Precomputes the available locales sets.
   */
  bool ensureSupportedLocales(JSContext* cx);

 public:
  enum class SupportedLocaleKind {
    Collator,
    DateTimeFormat,
    DisplayNames,
    ListFormat,
    NumberFormat,
    PluralRules,
    RelativeTimeFormat
  };

  /**
   * Sets |supported| to true if |locale| is supported by the requested Intl
   * service constructor. Otherwise sets |supported| to false.
   */
  [[nodiscard]] bool isSupportedLocale(JSContext* cx, SupportedLocaleKind kind,
                                       JS::Handle<JSString*> locale,
                                       bool* supported);

  /**
   * Returns all available locales for |kind|.
   */
  ArrayObject* availableLocalesOf(JSContext* cx, SupportedLocaleKind kind);

 private:
  /**
   * The case first parameter (BCP47 key "kf") allows to switch the order of
   * upper- and lower-case characters. ICU doesn't directly provide an API
   * to query the default case first value of a given locale, but instead
   * requires to instantiate a collator object and then query the case first
   * attribute (UCOL_CASE_FIRST).
   * To avoid instantiating an additional collator object whenever we need
   * to retrieve the default case first value of a specific locale, we
   * compute the default case first value for every supported locale only
   * once and then keep a list of all locales which don't use the default
   * case first setting.
   * There is almost no difference between lower-case first and when case
   * first is disabled (UCOL_LOWER_FIRST resp. UCOL_OFF), so we only need to
   * track locales which use upper-case first as their default setting.
   *
   * Instantiating collator objects for each available locale is slow
   * (bug 1527879), therefore we're hardcoding the two locales using upper-case
   * first ("da" (Danish) and "mt" (Maltese)) and only assert in debug-mode
   * these two locales match the upper-case first locales returned by ICU. A
   * system-ICU may support a different set of locales, therefore we're always
   * calling into ICU to find the upper-case first locales in that case.
   */

#if DEBUG || MOZ_SYSTEM_ICU
  LocaleSet upperCaseFirstLocales;

  bool upperCaseFirstInitialized = false;

  /**
   * Precomputes the available locales which use upper-case first sorting.
   */
  bool ensureUpperCaseFirstLocales(JSContext* cx);
#endif

 public:
  /**
   * Sets |isUpperFirst| to true if |locale| sorts upper-case characters
   * before lower-case characters.
   */
  bool isUpperCaseFirst(JSContext* cx, JS::Handle<JSString*> locale,
                        bool* isUpperFirst);

 private:
  using UniqueDateTimePatternGenerator =
      mozilla::UniquePtr<mozilla::intl::DateTimePatternGenerator,
                         DateTimePatternGeneratorDeleter>;

  UniqueDateTimePatternGenerator dateTimePatternGenerator;
  JS::UniqueChars dateTimePatternGeneratorLocale;

 public:
  /**
   * Get a non-owned cached instance of the DateTimePatternGenerator, which is
   * expensive to instantiate.
   *
   * See: https://bugzilla.mozilla.org/show_bug.cgi?id=1549578
   */
  mozilla::intl::DateTimePatternGenerator* getDateTimePatternGenerator(
      JSContext* cx, const char* locale);

 public:
  void destroyInstance();

  void trace(JSTracer* trc);

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
};

}  // namespace intl

}  // namespace js

#endif /* builtin_intl_SharedIntlData_h */
