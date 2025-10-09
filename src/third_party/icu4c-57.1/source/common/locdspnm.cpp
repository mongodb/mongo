/*
*******************************************************************************
* Copyright (C) 2010-2016, International Business Machines Corporation and
* others. All Rights Reserved.
*******************************************************************************
*/

#include "unicode/utypes.h"

#if !UCONFIG_NO_FORMATTING

#include "unicode/locdspnm.h"
#include "unicode/simpleformatter.h"
#include "unicode/ures.h"
#include "unicode/udisplaycontext.h"
#include "unicode/brkiter.h"
#include "unicode/ucurr.h"
#include "cmemory.h"
#include "cstring.h"
#include "mutex.h"
#include "ulocimp.h"
#include "umutex.h"
#include "ureslocs.h"
#include "uresimp.h"

#include <stdarg.h>

/**
 * Concatenate a number of null-terminated strings to buffer, leaving a
 * null-terminated string.  The last argument should be the null pointer.
 * Return the length of the string in the buffer, not counting the trailing
 * null.  Return -1 if there is an error (buffer is null, or buflen < 1).
 */
static int32_t ncat(char *buffer, uint32_t buflen, ...) {
  va_list args;
  char *str;
  char *p = buffer;
  const char* e = buffer + buflen - 1;

  if (buffer == NULL || buflen < 1) {
    return -1;
  }

  va_start(args, buflen);
  while ((str = va_arg(args, char *))) {
    char c;
    while (p != e && (c = *str++)) {
      *p++ = c;
    }
  }
  *p = 0;
  va_end(args);

  return p - buffer;
}

U_NAMESPACE_BEGIN

////////////////////////////////////////////////////////////////////////////////////////////////////

// Access resource data for locale components.
// Wrap code in uloc.c for now.
class ICUDataTable {
    const char* path;
    Locale locale;

public:
    ICUDataTable(const char* path, const Locale& locale);
    ~ICUDataTable();

    const Locale& getLocale();

    UnicodeString& get(const char* tableKey, const char* itemKey,
                        UnicodeString& result) const;
    UnicodeString& get(const char* tableKey, const char* subTableKey, const char* itemKey,
                        UnicodeString& result) const;

    UnicodeString& getNoFallback(const char* tableKey, const char* itemKey,
                                UnicodeString &result) const;
    UnicodeString& getNoFallback(const char* tableKey, const char* subTableKey, const char* itemKey,
                                UnicodeString &result) const;
};

inline UnicodeString &
ICUDataTable::get(const char* tableKey, const char* itemKey, UnicodeString& result) const {
    return get(tableKey, NULL, itemKey, result);
}

inline UnicodeString &
ICUDataTable::getNoFallback(const char* tableKey, const char* itemKey, UnicodeString& result) const {
    return getNoFallback(tableKey, NULL, itemKey, result);
}

ICUDataTable::ICUDataTable(const char* path, const Locale& locale)
    : path(NULL), locale(Locale::getRoot())
{
  if (path) {
    int32_t len = uprv_strlen(path);
    this->path = (const char*) uprv_malloc(len + 1);
    if (this->path) {
      uprv_strcpy((char *)this->path, path);
      this->locale = locale;
    }
  }
}

ICUDataTable::~ICUDataTable() {
  if (path) {
    uprv_free((void*) path);
    path = NULL;
  }
}

const Locale&
ICUDataTable::getLocale() {
  return locale;
}

UnicodeString &
ICUDataTable::get(const char* tableKey, const char* subTableKey, const char* itemKey,
                  UnicodeString &result) const {
  UErrorCode status = U_ZERO_ERROR;
  int32_t len = 0;

  const UChar *s = uloc_getTableStringWithFallback(path, locale.getName(),
                                                   tableKey, subTableKey, itemKey,
                                                   &len, &status);
  if (U_SUCCESS(status) && len > 0) {
    return result.setTo(s, len);
  }
  return result.setTo(UnicodeString(itemKey, -1, US_INV));
}

UnicodeString &
ICUDataTable::getNoFallback(const char* tableKey, const char* subTableKey, const char* itemKey,
                            UnicodeString& result) const {
  UErrorCode status = U_ZERO_ERROR;
  int32_t len = 0;

  const UChar *s = uloc_getTableStringWithFallback(path, locale.getName(),
                                                   tableKey, subTableKey, itemKey,
                                                   &len, &status);
  if (U_SUCCESS(status)) {
    return result.setTo(s, len);
  }

  result.setToBogus();
  return result;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

LocaleDisplayNames::~LocaleDisplayNames() {}

////////////////////////////////////////////////////////////////////////////////////////////////////

#if 0  // currently unused

class DefaultLocaleDisplayNames : public LocaleDisplayNames {
  UDialectHandling dialectHandling;

public:
  // constructor
  DefaultLocaleDisplayNames(UDialectHandling dialectHandling);

  virtual ~DefaultLocaleDisplayNames();

  virtual const Locale& getLocale() const;
  virtual UDialectHandling getDialectHandling() const;

  virtual UnicodeString& localeDisplayName(const Locale& locale,
                                           UnicodeString& result) const;
  virtual UnicodeString& localeDisplayName(const char* localeId,
                                           UnicodeString& result) const;
  virtual UnicodeString& languageDisplayName(const char* lang,
                                             UnicodeString& result) const;
  virtual UnicodeString& scriptDisplayName(const char* script,
                                           UnicodeString& result) const;
  virtual UnicodeString& scriptDisplayName(UScriptCode scriptCode,
                                           UnicodeString& result) const;
  virtual UnicodeString& regionDisplayName(const char* region,
                                           UnicodeString& result) const;
  virtual UnicodeString& variantDisplayName(const char* variant,
                                            UnicodeString& result) const;
  virtual UnicodeString& keyDisplayName(const char* key,
                                        UnicodeString& result) const;
  virtual UnicodeString& keyValueDisplayName(const char* key,
                                             const char* value,
                                             UnicodeString& result) const;
};

DefaultLocaleDisplayNames::DefaultLocaleDisplayNames(UDialectHandling dialectHandling)
    : dialectHandling(dialectHandling) {
}

DefaultLocaleDisplayNames::~DefaultLocaleDisplayNames() {
}

const Locale&
DefaultLocaleDisplayNames::getLocale() const {
  return Locale::getRoot();
}

UDialectHandling
DefaultLocaleDisplayNames::getDialectHandling() const {
  return dialectHandling;
}

UnicodeString&
DefaultLocaleDisplayNames::localeDisplayName(const Locale& locale,
                                             UnicodeString& result) const {
  return result = UnicodeString(locale.getName(), -1, US_INV);
}

UnicodeString&
DefaultLocaleDisplayNames::localeDisplayName(const char* localeId,
                                             UnicodeString& result) const {
  return result = UnicodeString(localeId, -1, US_INV);
}

UnicodeString&
DefaultLocaleDisplayNames::languageDisplayName(const char* lang,
                                               UnicodeString& result) const {
  return result = UnicodeString(lang, -1, US_INV);
}

UnicodeString&
DefaultLocaleDisplayNames::scriptDisplayName(const char* script,
                                             UnicodeString& result) const {
  return result = UnicodeString(script, -1, US_INV);
}

UnicodeString&
DefaultLocaleDisplayNames::scriptDisplayName(UScriptCode scriptCode,
                                             UnicodeString& result) const {
  const char* name = uscript_getName(scriptCode);
  if (name) {
    return result = UnicodeString(name, -1, US_INV);
  }
  return result.remove();
}

UnicodeString&
DefaultLocaleDisplayNames::regionDisplayName(const char* region,
                                             UnicodeString& result) const {
  return result = UnicodeString(region, -1, US_INV);
}

UnicodeString&
DefaultLocaleDisplayNames::variantDisplayName(const char* variant,
                                              UnicodeString& result) const {
  return result = UnicodeString(variant, -1, US_INV);
}

UnicodeString&
DefaultLocaleDisplayNames::keyDisplayName(const char* key,
                                          UnicodeString& result) const {
  return result = UnicodeString(key, -1, US_INV);
}

UnicodeString&
DefaultLocaleDisplayNames::keyValueDisplayName(const char* /* key */,
                                               const char* value,
                                               UnicodeString& result) const {
  return result = UnicodeString(value, -1, US_INV);
}

#endif  // currently unused class DefaultLocaleDisplayNames

////////////////////////////////////////////////////////////////////////////////////////////////////

class LocaleDisplayNamesImpl : public LocaleDisplayNames {
    Locale locale;
    UDialectHandling dialectHandling;
    ICUDataTable langData;
    ICUDataTable regionData;
    SimpleFormatter separatorFormat;
    SimpleFormatter format;
    SimpleFormatter keyTypeFormat;
    UDisplayContext capitalizationContext;
    BreakIterator* capitalizationBrkIter;
    static UMutex  capitalizationBrkIterLock;
    UnicodeString formatOpenParen;
    UnicodeString formatReplaceOpenParen;
    UnicodeString formatCloseParen;
    UnicodeString formatReplaceCloseParen;
    UDisplayContext nameLength;

    // Constants for capitalization context usage types.
    enum CapContextUsage {
        kCapContextUsageLanguage,
        kCapContextUsageScript,
        kCapContextUsageTerritory,
        kCapContextUsageVariant,
        kCapContextUsageKey,
        kCapContextUsageKeyValue,
        kCapContextUsageCount
    };
    // Capitalization transforms. For each usage type, indicates whether to titlecase for
    // the context specified in capitalizationContext (which we know at construction time)
     UBool fCapitalization[kCapContextUsageCount];

public:
    // constructor
    LocaleDisplayNamesImpl(const Locale& locale, UDialectHandling dialectHandling);
    LocaleDisplayNamesImpl(const Locale& locale, UDisplayContext *contexts, int32_t length);
    virtual ~LocaleDisplayNamesImpl();

    virtual const Locale& getLocale() const;
    virtual UDialectHandling getDialectHandling() const;
    virtual UDisplayContext getContext(UDisplayContextType type) const;

    virtual UnicodeString& localeDisplayName(const Locale& locale,
                                                UnicodeString& result) const;
    virtual UnicodeString& localeDisplayName(const char* localeId,
                                                UnicodeString& result) const;
    virtual UnicodeString& languageDisplayName(const char* lang,
                                               UnicodeString& result) const;
    virtual UnicodeString& scriptDisplayName(const char* script,
                                                UnicodeString& result) const;
    virtual UnicodeString& scriptDisplayName(UScriptCode scriptCode,
                                                UnicodeString& result) const;
    virtual UnicodeString& regionDisplayName(const char* region,
                                                UnicodeString& result) const;
    virtual UnicodeString& variantDisplayName(const char* variant,
                                                UnicodeString& result) const;
    virtual UnicodeString& keyDisplayName(const char* key,
                                                UnicodeString& result) const;
    virtual UnicodeString& keyValueDisplayName(const char* key,
                                                const char* value,
                                                UnicodeString& result) const;
private:
    UnicodeString& localeIdName(const char* localeId,
                                UnicodeString& result) const;
    UnicodeString& appendWithSep(UnicodeString& buffer, const UnicodeString& src) const;
    UnicodeString& adjustForUsageAndContext(CapContextUsage usage, UnicodeString& result) const;
    UnicodeString& scriptDisplayName(const char* script, UnicodeString& result, UBool skipAdjust) const;
    UnicodeString& regionDisplayName(const char* region, UnicodeString& result, UBool skipAdjust) const;
    UnicodeString& variantDisplayName(const char* variant, UnicodeString& result, UBool skipAdjust) const;
    UnicodeString& keyDisplayName(const char* key, UnicodeString& result, UBool skipAdjust) const;
    UnicodeString& keyValueDisplayName(const char* key, const char* value,
                                        UnicodeString& result, UBool skipAdjust) const;
    void initialize(void);
};

UMutex LocaleDisplayNamesImpl::capitalizationBrkIterLock = U_MUTEX_INITIALIZER;

LocaleDisplayNamesImpl::LocaleDisplayNamesImpl(const Locale& locale,
                                               UDialectHandling dialectHandling)
    : dialectHandling(dialectHandling)
    , langData(U_ICUDATA_LANG, locale)
    , regionData(U_ICUDATA_REGION, locale)
    , capitalizationContext(UDISPCTX_CAPITALIZATION_NONE)
    , capitalizationBrkIter(NULL)
    , nameLength(UDISPCTX_LENGTH_FULL)
{
    initialize();
}

LocaleDisplayNamesImpl::LocaleDisplayNamesImpl(const Locale& locale,
                                               UDisplayContext *contexts, int32_t length)
    : dialectHandling(ULDN_STANDARD_NAMES)
    , langData(U_ICUDATA_LANG, locale)
    , regionData(U_ICUDATA_REGION, locale)
    , capitalizationContext(UDISPCTX_CAPITALIZATION_NONE)
    , capitalizationBrkIter(NULL)
    , nameLength(UDISPCTX_LENGTH_FULL)
{
    while (length-- > 0) {
        UDisplayContext value = *contexts++;
        UDisplayContextType selector = (UDisplayContextType)((uint32_t)value >> 8);
        switch (selector) {
            case UDISPCTX_TYPE_DIALECT_HANDLING:
                dialectHandling = (UDialectHandling)value;
                break;
            case UDISPCTX_TYPE_CAPITALIZATION:
                capitalizationContext = value;
                break;
            case UDISPCTX_TYPE_DISPLAY_LENGTH:
                nameLength = value;
                break;
            default:
                break;
        }
    }
    initialize();
}

void
LocaleDisplayNamesImpl::initialize(void) {
    LocaleDisplayNamesImpl *nonConstThis = (LocaleDisplayNamesImpl *)this;
    nonConstThis->locale = langData.getLocale() == Locale::getRoot()
        ? regionData.getLocale()
        : langData.getLocale();

    UnicodeString sep;
    langData.getNoFallback("localeDisplayPattern", "separator", sep);
    if (sep.isBogus()) {
        sep = UnicodeString("{0}, {1}", -1, US_INV);
    }
    UErrorCode status = U_ZERO_ERROR;
    separatorFormat.applyPatternMinMaxArguments(sep, 2, 2, status);

    UnicodeString pattern;
    langData.getNoFallback("localeDisplayPattern", "pattern", pattern);
    if (pattern.isBogus()) {
        pattern = UnicodeString("{0} ({1})", -1, US_INV);
    }
    format.applyPatternMinMaxArguments(pattern, 2, 2, status);
    if (pattern.indexOf((UChar)0xFF08) >= 0) {
        formatOpenParen.setTo((UChar)0xFF08);         // fullwidth (
        formatReplaceOpenParen.setTo((UChar)0xFF3B);  // fullwidth [
        formatCloseParen.setTo((UChar)0xFF09);        // fullwidth )
        formatReplaceCloseParen.setTo((UChar)0xFF3D); // fullwidth ]
    } else {
        formatOpenParen.setTo((UChar)0x0028);         // (
        formatReplaceOpenParen.setTo((UChar)0x005B);  // [
        formatCloseParen.setTo((UChar)0x0029);        // )
        formatReplaceCloseParen.setTo((UChar)0x005D); // ]
    }

    UnicodeString ktPattern;
    langData.get("localeDisplayPattern", "keyTypePattern", ktPattern);
    if (ktPattern.isBogus()) {
        ktPattern = UnicodeString("{0}={1}", -1, US_INV);
    }
    keyTypeFormat.applyPatternMinMaxArguments(ktPattern, 2, 2, status);

    uprv_memset(fCapitalization, 0, sizeof(fCapitalization));
#if !UCONFIG_NO_BREAK_ITERATION
    // The following is basically copied from DateFormatSymbols::initializeData
    typedef struct {
        const char * usageName;
        LocaleDisplayNamesImpl::CapContextUsage usageEnum;
    } ContextUsageNameToEnum;
    const ContextUsageNameToEnum contextUsageTypeMap[] = {
       // Entries must be sorted by usageTypeName; entry with NULL name terminates list.
        { "key",        kCapContextUsageKey },
        { "keyValue",   kCapContextUsageKeyValue },
        { "languages",  kCapContextUsageLanguage },
        { "script",     kCapContextUsageScript },
        { "territory",  kCapContextUsageTerritory },
        { "variant",    kCapContextUsageVariant },
        { NULL,         (CapContextUsage)0 },
    };
    // Only get the context data if we need it! This is a const object so we know now...
    // Also check whether we will need a break iterator (depends on the data)
    UBool needBrkIter = FALSE;
    if (capitalizationContext == UDISPCTX_CAPITALIZATION_FOR_UI_LIST_OR_MENU || capitalizationContext == UDISPCTX_CAPITALIZATION_FOR_STANDALONE) {
        int32_t len = 0;
        UResourceBundle *localeBundle = ures_open(NULL, locale.getName(), &status);
        if (U_SUCCESS(status)) {
            UResourceBundle *contextTransforms = ures_getByKeyWithFallback(localeBundle, "contextTransforms", NULL, &status);
            if (U_SUCCESS(status)) {
                UResourceBundle *contextTransformUsage;
                while ( (contextTransformUsage = ures_getNextResource(contextTransforms, NULL, &status)) != NULL ) {
                    const int32_t * intVector = ures_getIntVector(contextTransformUsage, &len, &status);
                    if (U_SUCCESS(status) && intVector != NULL && len >= 2) {
                        const char* usageKey = ures_getKey(contextTransformUsage);
                        if (usageKey != NULL) {
                            const ContextUsageNameToEnum * typeMapPtr = contextUsageTypeMap;
                            int32_t compResult = 0;
                            // linear search; list is short and we cannot be sure that bsearch is available
                            while ( typeMapPtr->usageName != NULL && (compResult = uprv_strcmp(usageKey, typeMapPtr->usageName)) > 0 ) {
                                ++typeMapPtr;
                            }
                            if (typeMapPtr->usageName != NULL && compResult == 0) {
                                int32_t titlecaseInt = (capitalizationContext == UDISPCTX_CAPITALIZATION_FOR_UI_LIST_OR_MENU)? intVector[0]: intVector[1];
                                if (titlecaseInt != 0) {
                                    fCapitalization[typeMapPtr->usageEnum] = TRUE;;
                                    needBrkIter = TRUE;
                                }
                            }
                        }
                    }
                    status = U_ZERO_ERROR;
                    ures_close(contextTransformUsage);
                }
                ures_close(contextTransforms);
            }
            ures_close(localeBundle);
        }
    }
    // Get a sentence break iterator if we will need it
    if (needBrkIter || capitalizationContext == UDISPCTX_CAPITALIZATION_FOR_BEGINNING_OF_SENTENCE) {
        status = U_ZERO_ERROR;
        capitalizationBrkIter = BreakIterator::createSentenceInstance(locale, status);
        if (U_FAILURE(status)) {
            delete capitalizationBrkIter;
            capitalizationBrkIter = NULL;
        }
    }
#endif
}

LocaleDisplayNamesImpl::~LocaleDisplayNamesImpl() {
    delete capitalizationBrkIter;
 }

const Locale&
LocaleDisplayNamesImpl::getLocale() const {
    return locale;
}

UDialectHandling
LocaleDisplayNamesImpl::getDialectHandling() const {
    return dialectHandling;
}

UDisplayContext
LocaleDisplayNamesImpl::getContext(UDisplayContextType type) const {
    switch (type) {
        case UDISPCTX_TYPE_DIALECT_HANDLING:
            return (UDisplayContext)dialectHandling;
        case UDISPCTX_TYPE_CAPITALIZATION:
            return capitalizationContext;
        case UDISPCTX_TYPE_DISPLAY_LENGTH:
            return nameLength;
        default:
            break;
    }
    return (UDisplayContext)0;
}

UnicodeString&
LocaleDisplayNamesImpl::adjustForUsageAndContext(CapContextUsage usage,
                                                UnicodeString& result) const {
#if !UCONFIG_NO_BREAK_ITERATION
    // check to see whether we need to titlecase result
    if ( result.length() > 0 && u_islower(result.char32At(0)) && capitalizationBrkIter!= NULL &&
          ( capitalizationContext==UDISPCTX_CAPITALIZATION_FOR_BEGINNING_OF_SENTENCE || fCapitalization[usage] ) ) {
        // note fCapitalization[usage] won't be set unless capitalizationContext is UI_LIST_OR_MENU or STANDALONE
        Mutex lock(&capitalizationBrkIterLock);
        result.toTitle(capitalizationBrkIter, locale, U_TITLECASE_NO_LOWERCASE | U_TITLECASE_NO_BREAK_ADJUSTMENT);
    }
#endif
    return result;
}

UnicodeString&
LocaleDisplayNamesImpl::localeDisplayName(const Locale& locale,
                                          UnicodeString& result) const {
  if (locale.isBogus()) {
    result.setToBogus();
    return result;
  }
  UnicodeString resultName;

  const char* lang = locale.getLanguage();
  if (uprv_strlen(lang) == 0) {
    lang = "root";
  }
  const char* script = locale.getScript();
  const char* country = locale.getCountry();
  const char* variant = locale.getVariant();

  UBool hasScript = uprv_strlen(script) > 0;
  UBool hasCountry = uprv_strlen(country) > 0;
  UBool hasVariant = uprv_strlen(variant) > 0;

  if (dialectHandling == ULDN_DIALECT_NAMES) {
    char buffer[ULOC_FULLNAME_CAPACITY];
    do { // loop construct is so we can break early out of search
      if (hasScript && hasCountry) {
        ncat(buffer, ULOC_FULLNAME_CAPACITY, lang, "_", script, "_", country, (char *)0);
        localeIdName(buffer, resultName);
        if (!resultName.isBogus()) {
          hasScript = FALSE;
          hasCountry = FALSE;
          break;
        }
      }
      if (hasScript) {
        ncat(buffer, ULOC_FULLNAME_CAPACITY, lang, "_", script, (char *)0);
        localeIdName(buffer, resultName);
        if (!resultName.isBogus()) {
          hasScript = FALSE;
          break;
        }
      }
      if (hasCountry) {
        ncat(buffer, ULOC_FULLNAME_CAPACITY, lang, "_", country, (char*)0);
        localeIdName(buffer, resultName);
        if (!resultName.isBogus()) {
          hasCountry = FALSE;
          break;
        }
      }
    } while (FALSE);
  }
  if (resultName.isBogus() || resultName.isEmpty()) {
    localeIdName(lang, resultName);
  }

  UnicodeString resultRemainder;
  UnicodeString temp;
  UErrorCode status = U_ZERO_ERROR;

  if (hasScript) {
    resultRemainder.append(scriptDisplayName(script, temp, TRUE));
  }
  if (hasCountry) {
    appendWithSep(resultRemainder, regionDisplayName(country, temp, TRUE));
  }
  if (hasVariant) {
    appendWithSep(resultRemainder, variantDisplayName(variant, temp, TRUE));
  }
  resultRemainder.findAndReplace(formatOpenParen, formatReplaceOpenParen);
  resultRemainder.findAndReplace(formatCloseParen, formatReplaceCloseParen);

  LocalPointer<StringEnumeration> e(locale.createKeywords(status));
  if (e.isValid() && U_SUCCESS(status)) {
    UnicodeString temp2;
    char value[ULOC_KEYWORD_AND_VALUES_CAPACITY]; // sigh, no ULOC_VALUE_CAPACITY
    const char* key;
    while ((key = e->next((int32_t *)0, status)) != NULL) {
      value[0] = 0;
      locale.getKeywordValue(key, value, ULOC_KEYWORD_AND_VALUES_CAPACITY, status);
      if (U_FAILURE(status) || status == U_STRING_NOT_TERMINATED_WARNING) {
        return result;
      }
      keyDisplayName(key, temp, TRUE);
      temp.findAndReplace(formatOpenParen, formatReplaceOpenParen);
      temp.findAndReplace(formatCloseParen, formatReplaceCloseParen);
      keyValueDisplayName(key, value, temp2, TRUE);
      temp2.findAndReplace(formatOpenParen, formatReplaceOpenParen);
      temp2.findAndReplace(formatCloseParen, formatReplaceCloseParen);
      if (temp2 != UnicodeString(value, -1, US_INV)) {
        appendWithSep(resultRemainder, temp2);
      } else if (temp != UnicodeString(key, -1, US_INV)) {
        UnicodeString temp3;
        keyTypeFormat.format(temp, temp2, temp3, status);
        appendWithSep(resultRemainder, temp3);
      } else {
        appendWithSep(resultRemainder, temp)
          .append((UChar)0x3d /* = */)
          .append(temp2);
      }
    }
  }

  if (!resultRemainder.isEmpty()) {
    format.format(resultName, resultRemainder, result.remove(), status);
    return adjustForUsageAndContext(kCapContextUsageLanguage, result);
  }

  result = resultName;
  return adjustForUsageAndContext(kCapContextUsageLanguage, result);
}

UnicodeString&
LocaleDisplayNamesImpl::appendWithSep(UnicodeString& buffer, const UnicodeString& src) const {
    if (buffer.isEmpty()) {
        buffer.setTo(src);
    } else {
        const UnicodeString *values[2] = { &buffer, &src };
        UErrorCode status = U_ZERO_ERROR;
        separatorFormat.formatAndReplace(values, 2, buffer, NULL, 0, status);
    }
    return buffer;
}

UnicodeString&
LocaleDisplayNamesImpl::localeDisplayName(const char* localeId,
                                          UnicodeString& result) const {
    return localeDisplayName(Locale(localeId), result);
}

// private
UnicodeString&
LocaleDisplayNamesImpl::localeIdName(const char* localeId,
                                     UnicodeString& result) const {
    if (nameLength == UDISPCTX_LENGTH_SHORT) {
        langData.getNoFallback("Languages%short", localeId, result);
        if (!result.isBogus()) {
            return result;
        }
    }
    return langData.getNoFallback("Languages", localeId, result);
}

UnicodeString&
LocaleDisplayNamesImpl::languageDisplayName(const char* lang,
                                            UnicodeString& result) const {
    if (uprv_strcmp("root", lang) == 0 || uprv_strchr(lang, '_') != NULL) {
        return result = UnicodeString(lang, -1, US_INV);
    }
    if (nameLength == UDISPCTX_LENGTH_SHORT) {
        langData.get("Languages%short", lang, result);
        if (!result.isBogus()) {
            return adjustForUsageAndContext(kCapContextUsageLanguage, result);
        }
    }
    langData.get("Languages", lang, result);
    return adjustForUsageAndContext(kCapContextUsageLanguage, result);
}

UnicodeString&
LocaleDisplayNamesImpl::scriptDisplayName(const char* script,
                                          UnicodeString& result,
                                          UBool skipAdjust) const {
    if (nameLength == UDISPCTX_LENGTH_SHORT) {
        langData.get("Scripts%short", script, result);
        if (!result.isBogus()) {
            return skipAdjust? result: adjustForUsageAndContext(kCapContextUsageScript, result);
        }
    }
    langData.get("Scripts", script, result);
    return skipAdjust? result: adjustForUsageAndContext(kCapContextUsageScript, result);
}

UnicodeString&
LocaleDisplayNamesImpl::scriptDisplayName(const char* script,
                                          UnicodeString& result) const {
    return scriptDisplayName(script, result, FALSE);
}

UnicodeString&
LocaleDisplayNamesImpl::scriptDisplayName(UScriptCode scriptCode,
                                          UnicodeString& result) const {
    return scriptDisplayName(uscript_getName(scriptCode), result, FALSE);
}

UnicodeString&
LocaleDisplayNamesImpl::regionDisplayName(const char* region,
                                          UnicodeString& result,
                                          UBool skipAdjust) const {
    if (nameLength == UDISPCTX_LENGTH_SHORT) {
        regionData.get("Countries%short", region, result);
        if (!result.isBogus()) {
            return skipAdjust? result: adjustForUsageAndContext(kCapContextUsageTerritory, result);
        }
    }
    regionData.get("Countries", region, result);
    return skipAdjust? result: adjustForUsageAndContext(kCapContextUsageTerritory, result);
}

UnicodeString&
LocaleDisplayNamesImpl::regionDisplayName(const char* region,
                                          UnicodeString& result) const {
    return regionDisplayName(region, result, FALSE);
}


UnicodeString&
LocaleDisplayNamesImpl::variantDisplayName(const char* variant,
                                           UnicodeString& result,
                                           UBool skipAdjust) const {
    // don't have a resource for short variant names
    langData.get("Variants", variant, result);
    return skipAdjust? result: adjustForUsageAndContext(kCapContextUsageVariant, result);
}

UnicodeString&
LocaleDisplayNamesImpl::variantDisplayName(const char* variant,
                                           UnicodeString& result) const {
    return variantDisplayName(variant, result, FALSE);
}

UnicodeString&
LocaleDisplayNamesImpl::keyDisplayName(const char* key,
                                       UnicodeString& result,
                                       UBool skipAdjust) const {
    // don't have a resource for short key names
    langData.get("Keys", key, result);
    return skipAdjust? result: adjustForUsageAndContext(kCapContextUsageKey, result);
}

UnicodeString&
LocaleDisplayNamesImpl::keyDisplayName(const char* key,
                                       UnicodeString& result) const {
    return keyDisplayName(key, result, FALSE);
}

UnicodeString&
LocaleDisplayNamesImpl::keyValueDisplayName(const char* key,
                                            const char* value,
                                            UnicodeString& result,
                                            UBool skipAdjust) const {
    if (uprv_strcmp(key, "currency") == 0) {
        // ICU4C does not have ICU4J CurrencyDisplayInfo equivalent for now.
        UErrorCode sts = U_ZERO_ERROR;
        UnicodeString ustrValue(value, -1, US_INV);
        int32_t len;
        UBool isChoice = FALSE;
        const UChar *currencyName = ucurr_getName(ustrValue.getTerminatedBuffer(),
            locale.getBaseName(), UCURR_LONG_NAME, &isChoice, &len, &sts);
        if (U_FAILURE(sts)) {
            // Return the value as is on failure
            result = ustrValue;
            return result;
        }
        result.setTo(currencyName, len);
        return skipAdjust? result: adjustForUsageAndContext(kCapContextUsageKeyValue, result);
    }

    if (nameLength == UDISPCTX_LENGTH_SHORT) {
        langData.get("Types%short", key, value, result);
        if (!result.isBogus()) {
            return skipAdjust? result: adjustForUsageAndContext(kCapContextUsageKeyValue, result);
        }
    }
    langData.get("Types", key, value, result);
    return skipAdjust? result: adjustForUsageAndContext(kCapContextUsageKeyValue, result);
}

UnicodeString&
LocaleDisplayNamesImpl::keyValueDisplayName(const char* key,
                                            const char* value,
                                            UnicodeString& result) const {
    return keyValueDisplayName(key, value, result, FALSE);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

LocaleDisplayNames*
LocaleDisplayNames::createInstance(const Locale& locale,
                                   UDialectHandling dialectHandling) {
    return new LocaleDisplayNamesImpl(locale, dialectHandling);
}

LocaleDisplayNames*
LocaleDisplayNames::createInstance(const Locale& locale,
                                   UDisplayContext *contexts, int32_t length) {
    if (contexts == NULL) {
        length = 0;
    }
    return new LocaleDisplayNamesImpl(locale, contexts, length);
}

U_NAMESPACE_END

////////////////////////////////////////////////////////////////////////////////////////////////////

U_NAMESPACE_USE

U_CAPI ULocaleDisplayNames * U_EXPORT2
uldn_open(const char * locale,
          UDialectHandling dialectHandling,
          UErrorCode *pErrorCode) {
  if (U_FAILURE(*pErrorCode)) {
    return 0;
  }
  if (locale == NULL) {
    locale = uloc_getDefault();
  }
  return (ULocaleDisplayNames *)LocaleDisplayNames::createInstance(Locale(locale), dialectHandling);
}

U_CAPI ULocaleDisplayNames * U_EXPORT2
uldn_openForContext(const char * locale,
                    UDisplayContext *contexts, int32_t length,
                    UErrorCode *pErrorCode) {
  if (U_FAILURE(*pErrorCode)) {
    return 0;
  }
  if (locale == NULL) {
    locale = uloc_getDefault();
  }
  return (ULocaleDisplayNames *)LocaleDisplayNames::createInstance(Locale(locale), contexts, length);
}


U_CAPI void U_EXPORT2
uldn_close(ULocaleDisplayNames *ldn) {
  delete (LocaleDisplayNames *)ldn;
}

U_CAPI const char * U_EXPORT2
uldn_getLocale(const ULocaleDisplayNames *ldn) {
  if (ldn) {
    return ((const LocaleDisplayNames *)ldn)->getLocale().getName();
  }
  return NULL;
}

U_CAPI UDialectHandling U_EXPORT2
uldn_getDialectHandling(const ULocaleDisplayNames *ldn) {
  if (ldn) {
    return ((const LocaleDisplayNames *)ldn)->getDialectHandling();
  }
  return ULDN_STANDARD_NAMES;
}

U_CAPI UDisplayContext U_EXPORT2
uldn_getContext(const ULocaleDisplayNames *ldn,
              UDisplayContextType type,
              UErrorCode *pErrorCode) {
  if (U_FAILURE(*pErrorCode)) {
    return (UDisplayContext)0;
  }
  return ((const LocaleDisplayNames *)ldn)->getContext(type);
}

U_CAPI int32_t U_EXPORT2
uldn_localeDisplayName(const ULocaleDisplayNames *ldn,
                       const char *locale,
                       UChar *result,
                       int32_t maxResultSize,
                       UErrorCode *pErrorCode) {
  if (U_FAILURE(*pErrorCode)) {
    return 0;
  }
  if (ldn == NULL || locale == NULL || (result == NULL && maxResultSize > 0) || maxResultSize < 0) {
    *pErrorCode = U_ILLEGAL_ARGUMENT_ERROR;
    return 0;
  }
  UnicodeString temp(result, 0, maxResultSize);
  ((const LocaleDisplayNames *)ldn)->localeDisplayName(locale, temp);
  if (temp.isBogus()) {
    *pErrorCode = U_ILLEGAL_ARGUMENT_ERROR;
    return 0;
  }
  return temp.extract(result, maxResultSize, *pErrorCode);
}

U_CAPI int32_t U_EXPORT2
uldn_languageDisplayName(const ULocaleDisplayNames *ldn,
                         const char *lang,
                         UChar *result,
                         int32_t maxResultSize,
                         UErrorCode *pErrorCode) {
  if (U_FAILURE(*pErrorCode)) {
    return 0;
  }
  if (ldn == NULL || lang == NULL || (result == NULL && maxResultSize > 0) || maxResultSize < 0) {
    *pErrorCode = U_ILLEGAL_ARGUMENT_ERROR;
    return 0;
  }
  UnicodeString temp(result, 0, maxResultSize);
  ((const LocaleDisplayNames *)ldn)->languageDisplayName(lang, temp);
  return temp.extract(result, maxResultSize, *pErrorCode);
}

U_CAPI int32_t U_EXPORT2
uldn_scriptDisplayName(const ULocaleDisplayNames *ldn,
                       const char *script,
                       UChar *result,
                       int32_t maxResultSize,
                       UErrorCode *pErrorCode) {
  if (U_FAILURE(*pErrorCode)) {
    return 0;
  }
  if (ldn == NULL || script == NULL || (result == NULL && maxResultSize > 0) || maxResultSize < 0) {
    *pErrorCode = U_ILLEGAL_ARGUMENT_ERROR;
    return 0;
  }
  UnicodeString temp(result, 0, maxResultSize);
  ((const LocaleDisplayNames *)ldn)->scriptDisplayName(script, temp);
  return temp.extract(result, maxResultSize, *pErrorCode);
}

U_CAPI int32_t U_EXPORT2
uldn_scriptCodeDisplayName(const ULocaleDisplayNames *ldn,
                           UScriptCode scriptCode,
                           UChar *result,
                           int32_t maxResultSize,
                           UErrorCode *pErrorCode) {
  return uldn_scriptDisplayName(ldn, uscript_getName(scriptCode), result, maxResultSize, pErrorCode);
}

U_CAPI int32_t U_EXPORT2
uldn_regionDisplayName(const ULocaleDisplayNames *ldn,
                       const char *region,
                       UChar *result,
                       int32_t maxResultSize,
                       UErrorCode *pErrorCode) {
  if (U_FAILURE(*pErrorCode)) {
    return 0;
  }
  if (ldn == NULL || region == NULL || (result == NULL && maxResultSize > 0) || maxResultSize < 0) {
    *pErrorCode = U_ILLEGAL_ARGUMENT_ERROR;
    return 0;
  }
  UnicodeString temp(result, 0, maxResultSize);
  ((const LocaleDisplayNames *)ldn)->regionDisplayName(region, temp);
  return temp.extract(result, maxResultSize, *pErrorCode);
}

U_CAPI int32_t U_EXPORT2
uldn_variantDisplayName(const ULocaleDisplayNames *ldn,
                        const char *variant,
                        UChar *result,
                        int32_t maxResultSize,
                        UErrorCode *pErrorCode) {
  if (U_FAILURE(*pErrorCode)) {
    return 0;
  }
  if (ldn == NULL || variant == NULL || (result == NULL && maxResultSize > 0) || maxResultSize < 0) {
    *pErrorCode = U_ILLEGAL_ARGUMENT_ERROR;
    return 0;
  }
  UnicodeString temp(result, 0, maxResultSize);
  ((const LocaleDisplayNames *)ldn)->variantDisplayName(variant, temp);
  return temp.extract(result, maxResultSize, *pErrorCode);
}

U_CAPI int32_t U_EXPORT2
uldn_keyDisplayName(const ULocaleDisplayNames *ldn,
                    const char *key,
                    UChar *result,
                    int32_t maxResultSize,
                    UErrorCode *pErrorCode) {
  if (U_FAILURE(*pErrorCode)) {
    return 0;
  }
  if (ldn == NULL || key == NULL || (result == NULL && maxResultSize > 0) || maxResultSize < 0) {
    *pErrorCode = U_ILLEGAL_ARGUMENT_ERROR;
    return 0;
  }
  UnicodeString temp(result, 0, maxResultSize);
  ((const LocaleDisplayNames *)ldn)->keyDisplayName(key, temp);
  return temp.extract(result, maxResultSize, *pErrorCode);
}

U_CAPI int32_t U_EXPORT2
uldn_keyValueDisplayName(const ULocaleDisplayNames *ldn,
                         const char *key,
                         const char *value,
                         UChar *result,
                         int32_t maxResultSize,
                         UErrorCode *pErrorCode) {
  if (U_FAILURE(*pErrorCode)) {
    return 0;
  }
  if (ldn == NULL || key == NULL || value == NULL || (result == NULL && maxResultSize > 0)
      || maxResultSize < 0) {
    *pErrorCode = U_ILLEGAL_ARGUMENT_ERROR;
    return 0;
  }
  UnicodeString temp(result, 0, maxResultSize);
  ((const LocaleDisplayNames *)ldn)->keyValueDisplayName(key, value, temp);
  return temp.extract(result, maxResultSize, *pErrorCode);
}

#endif
