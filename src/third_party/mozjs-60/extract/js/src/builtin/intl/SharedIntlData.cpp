/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Runtime-wide Intl data shared across compartments. */

#include "builtin/intl/SharedIntlData.h"

#include "mozilla/Assertions.h"
#include "mozilla/HashFunctions.h"

#include <stdint.h>

#include "builtin/intl/CommonFunctions.h"
#include "builtin/intl/ICUStubs.h"
#include "builtin/intl/ScopedICUObject.h"
#include "builtin/intl/TimeZoneDataGenerated.h"
#include "builtin/String.h"
#include "js/Utility.h"
#include "vm/JSAtom.h"

using js::HashNumber;
using js::intl::StringsAreEqual;

template<typename Char>
static constexpr Char
ToUpperASCII(Char c)
{
    return ('a' <= c && c <= 'z')
           ? (c & ~0x20)
           : c;
}

static_assert(ToUpperASCII('a') == 'A', "verifying 'a' uppercases correctly");
static_assert(ToUpperASCII('m') == 'M', "verifying 'm' uppercases correctly");
static_assert(ToUpperASCII('z') == 'Z', "verifying 'z' uppercases correctly");
static_assert(ToUpperASCII(u'a') == u'A', "verifying u'a' uppercases correctly");
static_assert(ToUpperASCII(u'k') == u'K', "verifying u'k' uppercases correctly");
static_assert(ToUpperASCII(u'z') == u'Z', "verifying u'z' uppercases correctly");

template<typename Char>
static HashNumber
HashStringIgnoreCaseASCII(const Char* s, size_t length)
{
    uint32_t hash = 0;
    for (size_t i = 0; i < length; i++)
        hash = mozilla::AddToHash(hash, ToUpperASCII(s[i]));
    return hash;
}

js::intl::SharedIntlData::TimeZoneHasher::Lookup::Lookup(JSLinearString* timeZone)
  : js::intl::SharedIntlData::LinearStringLookup(timeZone)
{
    if (isLatin1)
        hash = HashStringIgnoreCaseASCII(latin1Chars, length);
    else
        hash = HashStringIgnoreCaseASCII(twoByteChars, length);
}

template<typename Char1, typename Char2>
static bool
EqualCharsIgnoreCaseASCII(const Char1* s1, const Char2* s2, size_t len)
{
    for (const Char1* s1end = s1 + len; s1 < s1end; s1++, s2++) {
        if (ToUpperASCII(*s1) != ToUpperASCII(*s2))
            return false;
    }
    return true;
}

bool
js::intl::SharedIntlData::TimeZoneHasher::match(TimeZoneName key, const Lookup& lookup)
{
    if (key->length() != lookup.length)
        return false;

    // Compare time zone names ignoring ASCII case differences.
    if (key->hasLatin1Chars()) {
        const Latin1Char* keyChars = key->latin1Chars(lookup.nogc);
        if (lookup.isLatin1)
            return EqualCharsIgnoreCaseASCII(keyChars, lookup.latin1Chars, lookup.length);
        return EqualCharsIgnoreCaseASCII(keyChars, lookup.twoByteChars, lookup.length);
    }

    const char16_t* keyChars = key->twoByteChars(lookup.nogc);
    if (lookup.isLatin1)
        return EqualCharsIgnoreCaseASCII(lookup.latin1Chars, keyChars, lookup.length);
    return EqualCharsIgnoreCaseASCII(keyChars, lookup.twoByteChars, lookup.length);
}

static bool
IsLegacyICUTimeZone(const char* timeZone)
{
    for (const auto& legacyTimeZone : js::timezone::legacyICUTimeZones) {
        if (StringsAreEqual(timeZone, legacyTimeZone))
            return true;
    }
    return false;
}

bool
js::intl::SharedIntlData::ensureTimeZones(JSContext* cx)
{
    if (timeZoneDataInitialized)
        return true;

    // If ensureTimeZones() was called previously, but didn't complete due to
    // OOM, clear all sets/maps and start from scratch.
    if (availableTimeZones.initialized())
        availableTimeZones.finish();
    if (!availableTimeZones.init()) {
        ReportOutOfMemory(cx);
        return false;
    }

    UErrorCode status = U_ZERO_ERROR;
    UEnumeration* values = ucal_openTimeZones(&status);
    if (U_FAILURE(status)) {
        ReportInternalError(cx);
        return false;
    }
    ScopedICUObject<UEnumeration, uenum_close> toClose(values);

    RootedAtom timeZone(cx);
    while (true) {
        int32_t size;
        const char* rawTimeZone = uenum_next(values, &size, &status);
        if (U_FAILURE(status)) {
            ReportInternalError(cx);
            return false;
        }

        if (rawTimeZone == nullptr)
            break;

        // Skip legacy ICU time zone names.
        if (IsLegacyICUTimeZone(rawTimeZone))
            continue;

        MOZ_ASSERT(size >= 0);
        timeZone = Atomize(cx, rawTimeZone, size_t(size));
        if (!timeZone)
            return false;

        TimeZoneHasher::Lookup lookup(timeZone);
        TimeZoneSet::AddPtr p = availableTimeZones.lookupForAdd(lookup);

        // ICU shouldn't report any duplicate time zone names, but if it does,
        // just ignore the duplicate name.
        if (!p && !availableTimeZones.add(p, timeZone)) {
            ReportOutOfMemory(cx);
            return false;
        }
    }

    if (ianaZonesTreatedAsLinksByICU.initialized())
        ianaZonesTreatedAsLinksByICU.finish();
    if (!ianaZonesTreatedAsLinksByICU.init()) {
        ReportOutOfMemory(cx);
        return false;
    }

    for (const char* rawTimeZone : timezone::ianaZonesTreatedAsLinksByICU) {
        MOZ_ASSERT(rawTimeZone != nullptr);
        timeZone = Atomize(cx, rawTimeZone, strlen(rawTimeZone));
        if (!timeZone)
            return false;

        TimeZoneHasher::Lookup lookup(timeZone);
        TimeZoneSet::AddPtr p = ianaZonesTreatedAsLinksByICU.lookupForAdd(lookup);
        MOZ_ASSERT(!p, "Duplicate entry in timezone::ianaZonesTreatedAsLinksByICU");

        if (!ianaZonesTreatedAsLinksByICU.add(p, timeZone)) {
            ReportOutOfMemory(cx);
            return false;
        }
    }

    if (ianaLinksCanonicalizedDifferentlyByICU.initialized())
        ianaLinksCanonicalizedDifferentlyByICU.finish();
    if (!ianaLinksCanonicalizedDifferentlyByICU.init()) {
        ReportOutOfMemory(cx);
        return false;
    }

    RootedAtom linkName(cx);
    RootedAtom& target = timeZone;
    for (const auto& linkAndTarget : timezone::ianaLinksCanonicalizedDifferentlyByICU) {
        const char* rawLinkName = linkAndTarget.link;
        const char* rawTarget = linkAndTarget.target;

        MOZ_ASSERT(rawLinkName != nullptr);
        linkName = Atomize(cx, rawLinkName, strlen(rawLinkName));
        if (!linkName)
            return false;

        MOZ_ASSERT(rawTarget != nullptr);
        target = Atomize(cx, rawTarget, strlen(rawTarget));
        if (!target)
            return false;

        TimeZoneHasher::Lookup lookup(linkName);
        TimeZoneMap::AddPtr p = ianaLinksCanonicalizedDifferentlyByICU.lookupForAdd(lookup);
        MOZ_ASSERT(!p, "Duplicate entry in timezone::ianaLinksCanonicalizedDifferentlyByICU");

        if (!ianaLinksCanonicalizedDifferentlyByICU.add(p, linkName, target)) {
            ReportOutOfMemory(cx);
            return false;
        }
    }

    MOZ_ASSERT(!timeZoneDataInitialized, "ensureTimeZones is neither reentrant nor thread-safe");
    timeZoneDataInitialized = true;

    return true;
}

bool
js::intl::SharedIntlData::validateTimeZoneName(JSContext* cx, HandleString timeZone,
                                               MutableHandleAtom result)
{
    if (!ensureTimeZones(cx))
        return false;

    RootedLinearString timeZoneLinear(cx, timeZone->ensureLinear(cx));
    if (!timeZoneLinear)
        return false;

    TimeZoneHasher::Lookup lookup(timeZoneLinear);
    if (TimeZoneSet::Ptr p = availableTimeZones.lookup(lookup))
        result.set(*p);

    return true;
}

bool
js::intl::SharedIntlData::tryCanonicalizeTimeZoneConsistentWithIANA(JSContext* cx,
                                                                    HandleString timeZone,
                                                                    MutableHandleAtom result)
{
    if (!ensureTimeZones(cx))
        return false;

    RootedLinearString timeZoneLinear(cx, timeZone->ensureLinear(cx));
    if (!timeZoneLinear)
        return false;

    TimeZoneHasher::Lookup lookup(timeZoneLinear);
    MOZ_ASSERT(availableTimeZones.has(lookup), "Invalid time zone name");

    if (TimeZoneMap::Ptr p = ianaLinksCanonicalizedDifferentlyByICU.lookup(lookup)) {
        // The effectively supported time zones aren't known at compile time,
        // when
        // 1. SpiderMonkey was compiled with "--with-system-icu".
        // 2. ICU's dynamic time zone data loading feature was used.
        //    (ICU supports loading time zone files at runtime through the
        //    ICU_TIMEZONE_FILES_DIR environment variable.)
        // Ensure ICU supports the new target zone before applying the update.
        TimeZoneName targetTimeZone = p->value();
        TimeZoneHasher::Lookup targetLookup(targetTimeZone);
        if (availableTimeZones.has(targetLookup))
            result.set(targetTimeZone);
    } else if (TimeZoneSet::Ptr p = ianaZonesTreatedAsLinksByICU.lookup(lookup)) {
        result.set(*p);
    }

    return true;
}

js::intl::SharedIntlData::LocaleHasher::Lookup::Lookup(JSLinearString* locale)
  : js::intl::SharedIntlData::LinearStringLookup(locale)
{
    if (isLatin1)
        hash = mozilla::HashString(latin1Chars, length);
    else
        hash = mozilla::HashString(twoByteChars, length);
}

bool
js::intl::SharedIntlData::LocaleHasher::match(Locale key, const Lookup& lookup)
{
    if (key->length() != lookup.length)
        return false;

    if (key->hasLatin1Chars()) {
        const Latin1Char* keyChars = key->latin1Chars(lookup.nogc);
        if (lookup.isLatin1)
            return EqualChars(keyChars, lookup.latin1Chars, lookup.length);
        return EqualChars(keyChars, lookup.twoByteChars, lookup.length);
    }

    const char16_t* keyChars = key->twoByteChars(lookup.nogc);
    if (lookup.isLatin1)
        return EqualChars(lookup.latin1Chars, keyChars, lookup.length);
    return EqualChars(keyChars, lookup.twoByteChars, lookup.length);
}

bool
js::intl::SharedIntlData::ensureUpperCaseFirstLocales(JSContext* cx)
{
    if (upperCaseFirstInitialized)
        return true;

    // If ensureUpperCaseFirstLocales() was called previously, but didn't
    // complete due to OOM, clear all data and start from scratch.
    if (upperCaseFirstLocales.initialized())
        upperCaseFirstLocales.finish();
    if (!upperCaseFirstLocales.init()) {
        ReportOutOfMemory(cx);
        return false;
    }

    UErrorCode status = U_ZERO_ERROR;
    UEnumeration* available = ucol_openAvailableLocales(&status);
    if (U_FAILURE(status)) {
        ReportInternalError(cx);
        return false;
    }
    ScopedICUObject<UEnumeration, uenum_close> toClose(available);

    RootedAtom locale(cx);
    while (true) {
        int32_t size;
        const char* rawLocale = uenum_next(available, &size, &status);
        if (U_FAILURE(status)) {
            ReportInternalError(cx);
            return false;
        }

        if (rawLocale == nullptr)
            break;

        UCollator* collator = ucol_open(rawLocale, &status);
        if (U_FAILURE(status)) {
            ReportInternalError(cx);
            return false;
        }
        ScopedICUObject<UCollator, ucol_close> toCloseCollator(collator);

        UColAttributeValue caseFirst = ucol_getAttribute(collator, UCOL_CASE_FIRST, &status);
        if (U_FAILURE(status)) {
            ReportInternalError(cx);
            return false;
        }

        if (caseFirst != UCOL_UPPER_FIRST)
            continue;

        MOZ_ASSERT(size >= 0);
        locale = Atomize(cx, rawLocale, size_t(size));
        if (!locale)
            return false;

        LocaleHasher::Lookup lookup(locale);
        LocaleSet::AddPtr p = upperCaseFirstLocales.lookupForAdd(lookup);

        // ICU shouldn't report any duplicate locales, but if it does, just
        // ignore the duplicated locale.
        if (!p && !upperCaseFirstLocales.add(p, locale)) {
            ReportOutOfMemory(cx);
            return false;
        }
    }

    MOZ_ASSERT(!upperCaseFirstInitialized,
               "ensureUpperCaseFirstLocales is neither reentrant nor thread-safe");
    upperCaseFirstInitialized = true;

    return true;
}

bool
js::intl::SharedIntlData::isUpperCaseFirst(JSContext* cx, HandleString locale, bool* isUpperFirst)
{
    if (!ensureUpperCaseFirstLocales(cx))
        return false;

    RootedLinearString localeLinear(cx, locale->ensureLinear(cx));
    if (!localeLinear)
        return false;

    LocaleHasher::Lookup lookup(localeLinear);
    *isUpperFirst = upperCaseFirstLocales.has(lookup);

    return true;
}

void
js::intl::SharedIntlData::destroyInstance()
{
    availableTimeZones.finish();
    ianaZonesTreatedAsLinksByICU.finish();
    ianaLinksCanonicalizedDifferentlyByICU.finish();
    upperCaseFirstLocales.finish();
}

void
js::intl::SharedIntlData::trace(JSTracer* trc)
{
    // Atoms are always tenured.
    if (!JS::CurrentThreadIsHeapMinorCollecting()) {
        availableTimeZones.trace(trc);
        ianaZonesTreatedAsLinksByICU.trace(trc);
        ianaLinksCanonicalizedDifferentlyByICU.trace(trc);
        upperCaseFirstLocales.trace(trc);
    }
}

size_t
js::intl::SharedIntlData::sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const
{
    return availableTimeZones.sizeOfExcludingThis(mallocSizeOf) +
           ianaZonesTreatedAsLinksByICU.sizeOfExcludingThis(mallocSizeOf) +
           ianaLinksCanonicalizedDifferentlyByICU.sizeOfExcludingThis(mallocSizeOf) +
           upperCaseFirstLocales.sizeOfExcludingThis(mallocSizeOf);
}
