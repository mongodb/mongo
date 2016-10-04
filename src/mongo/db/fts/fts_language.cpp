// fts_language.cpp

/**
 *    Copyright (C) 2013 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/fts/fts_language.h"

#include <string>

#include "mongo/base/init.h"
#include "mongo/db/fts/fts_basic_phrase_matcher.h"
#include "mongo/db/fts/fts_basic_tokenizer.h"
#include "mongo/db/fts/fts_unicode_phrase_matcher.h"
#include "mongo/db/fts/fts_unicode_tokenizer.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/string_map.h"
#include "mongo/util/stringutils.h"

namespace mongo {

namespace fts {

namespace {

/**
 * Case-insensitive StringData comparator.
 */
struct LanguageStringCompare {
    /** Returns true if lhs < rhs. */
    bool operator()(std::string lhs, std::string rhs) const {
        size_t minSize = std::min(lhs.size(), rhs.size());

        for (size_t x = 0; x < minSize; x++) {
            char a = tolower(lhs[x]);
            char b = tolower(rhs[x]);
            if (a < b) {
                return true;
            }
            if (a > b) {
                return false;
            }
        }

        return lhs.size() < rhs.size();
    }
};

// Lookup table from user language string (case-insensitive) to FTSLanguage.
// Populated by initializers in initializer FTSRegisterV2LanguagesAndLater and initializer
// FTSRegisterLanguageAliases.  For use with TEXT_INDEX_VERSION_2 text indexes and above.
typedef std::map<std::string, const FTSLanguage*, LanguageStringCompare> LanguageMap;

LanguageMap languageMapV3;
LanguageMap languageMapV2;

// Like languageMapV2, but for use with TEXT_INDEX_VERSION_1 text indexes.
// Case-sensitive by lookup key.
typedef std::map<StringData, const FTSLanguage*> LanguageMapLegacy;
LanguageMapLegacy languageMapV1;
}

MONGO_INITIALIZER_GROUP(FTSAllLanguagesRegistered, MONGO_NO_PREREQUISITES, MONGO_NO_DEPENDENTS);

// FTS Language map. These languages are available with TEXT_INDEX_VERSION_2 and above.
//
// Parameters:
// - C++ unique identifier suffix
// - lower case string name
// - language alias
//
#define MONGO_FTS_LANGUAGE_LIST(MONGO_FTS_LANGUAGE_DECL)    \
    MONGO_FTS_LANGUAGE_DECL(Danish, "danish", "da")         \
    MONGO_FTS_LANGUAGE_DECL(Dutch, "dutch", "nl")           \
    MONGO_FTS_LANGUAGE_DECL(English, "english", "en")       \
    MONGO_FTS_LANGUAGE_DECL(Finnish, "finnish", "fi")       \
    MONGO_FTS_LANGUAGE_DECL(French, "french", "fr")         \
    MONGO_FTS_LANGUAGE_DECL(German, "german", "de")         \
    MONGO_FTS_LANGUAGE_DECL(Hungarian, "hungarian", "hu")   \
    MONGO_FTS_LANGUAGE_DECL(Italian, "italian", "it")       \
    MONGO_FTS_LANGUAGE_DECL(Norwegian, "norwegian", "nb")   \
    MONGO_FTS_LANGUAGE_DECL(Portuguese, "portuguese", "pt") \
    MONGO_FTS_LANGUAGE_DECL(Romanian, "romanian", "ro")     \
    MONGO_FTS_LANGUAGE_DECL(Russian, "russian", "ru")       \
    MONGO_FTS_LANGUAGE_DECL(Spanish, "spanish", "es")       \
    MONGO_FTS_LANGUAGE_DECL(Swedish, "swedish", "sv")       \
    MONGO_FTS_LANGUAGE_DECL(Turkish, "turkish", "tr")


// Declare compilation unit local language object.
// Must be declared statically as global language map only keeps a pointer to the language
// instance.
//
#define LANGUAGE_DECLV2(id, name, alias) BasicFTSLanguage language##id##V2;

#define LANGUAGE_DECLV3(id, name, alias) UnicodeFTSLanguage language##id##V3(name);

BasicFTSLanguage languageNoneV2;
MONGO_FTS_LANGUAGE_LIST(LANGUAGE_DECLV2);

UnicodeFTSLanguage languageNoneV3("none");
MONGO_FTS_LANGUAGE_LIST(LANGUAGE_DECLV3);

// Registers each language and language aliases in the language map.
//
#define LANGUAGE_INITV2(id, name, alias) \
    FTSLanguage::registerLanguage(name, TEXT_INDEX_VERSION_2, &language##id##V2);

#define LANGUAGE_INITV3(id, name, alias) \
    FTSLanguage::registerLanguage(name, TEXT_INDEX_VERSION_3, &language##id##V3);

/**
 * Registers each language in the language map.
 */
MONGO_INITIALIZER_GENERAL(FTSRegisterV2LanguagesAndLater,
                          MONGO_NO_PREREQUISITES,
                          ("FTSAllLanguagesRegistered"))
(::mongo::InitializerContext* context) {
    FTSLanguage::registerLanguage("none", TEXT_INDEX_VERSION_2, &languageNoneV2);
    MONGO_FTS_LANGUAGE_LIST(LANGUAGE_INITV2);

    FTSLanguage::registerLanguage("none", TEXT_INDEX_VERSION_3, &languageNoneV3);
    MONGO_FTS_LANGUAGE_LIST(LANGUAGE_INITV3);
    return Status::OK();
}

#define LANGUAGE_ALIASV2(id, name, alias) \
    FTSLanguage::registerLanguageAlias(&language##id##V2, alias, TEXT_INDEX_VERSION_2);

#define LANGUAGE_ALIASV3(id, name, alias) \
    FTSLanguage::registerLanguageAlias(&language##id##V3, alias, TEXT_INDEX_VERSION_3);

/**
 * Registers each language alias in the language map.
 */
MONGO_INITIALIZER_WITH_PREREQUISITES(FTSRegisterLanguageAliases, ("FTSAllLanguagesRegistered"))
(InitializerContext* context) {
    // Register language aliases for TEXT_INDEX_VERSION_2.
    MONGO_FTS_LANGUAGE_LIST(LANGUAGE_ALIASV2);
    // Register language aliases for TEXT_INDEX_VERSION_3.
    MONGO_FTS_LANGUAGE_LIST(LANGUAGE_ALIASV3);
    return Status::OK();
}

//
// Register all Snowball language modules for TEXT_INDEX_VERSION_1.  Note that only the full
// names are recognized by the StopWords class (as such, the language string "dan" in
// TEXT_INDEX_VERSION_1 will generate the Danish stemmer and the empty stopword list).
//

MONGO_FTS_LANGUAGE_DECLARE(languageNoneV1, "none", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageDaV1, "da", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageDanV1, "dan", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageDanishV1, "danish", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageDeV1, "de", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageDeuV1, "deu", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageDutV1, "dut", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageDutchV1, "dutch", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageEnV1, "en", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageEngV1, "eng", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageEnglishV1, "english", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageEsV1, "es", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageEslV1, "esl", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageFiV1, "fi", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageFinV1, "fin", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageFinnishV1, "finnish", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageFrV1, "fr", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageFraV1, "fra", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageFreV1, "fre", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageFrenchV1, "french", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageGerV1, "ger", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageGermanV1, "german", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageHuV1, "hu", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageHunV1, "hun", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageHungarianV1, "hungarian", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageItV1, "it", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageItaV1, "ita", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageItalianV1, "italian", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageNlV1, "nl", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageNldV1, "nld", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageNoV1, "no", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageNorV1, "nor", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageNorwegianV1, "norwegian", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languagePorV1, "por", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languagePorterV1, "porter", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languagePortugueseV1, "portuguese", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languagePtV1, "pt", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageRoV1, "ro", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageRomanianV1, "romanian", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageRonV1, "ron", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageRuV1, "ru", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageRumV1, "rum", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageRusV1, "rus", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageRussianV1, "russian", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageSpaV1, "spa", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageSpanishV1, "spanish", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageSvV1, "sv", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageSweV1, "swe", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageSwedishV1, "swedish", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageTrV1, "tr", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageTurV1, "tur", TEXT_INDEX_VERSION_1);
MONGO_FTS_LANGUAGE_DECLARE(languageTurkishV1, "turkish", TEXT_INDEX_VERSION_1);

// static
void FTSLanguage::registerLanguage(StringData languageName,
                                   TextIndexVersion textIndexVersion,
                                   FTSLanguage* language) {
    verify(!languageName.empty());
    language->_canonicalName = languageName.toString();

    if (textIndexVersion >= TEXT_INDEX_VERSION_2) {
        LanguageMap* languageMap =
            (textIndexVersion == TEXT_INDEX_VERSION_3) ? &languageMapV3 : &languageMapV2;
        (*languageMap)[languageName.toString()] = language;
    } else {
        // Legacy text index.
        invariant(textIndexVersion == TEXT_INDEX_VERSION_1);
        verify(languageMapV1.find(languageName) == languageMapV1.end());
        languageMapV1[languageName] = language;
    }
}

// static
void FTSLanguage::registerLanguageAlias(const FTSLanguage* language,
                                        StringData alias,
                                        TextIndexVersion textIndexVersion) {
    if (textIndexVersion >= TEXT_INDEX_VERSION_2) {
        LanguageMap* languageMap =
            (textIndexVersion == TEXT_INDEX_VERSION_3) ? &languageMapV3 : &languageMapV2;
        (*languageMap)[alias.toString()] = language;
    } else {
        // Legacy text index.
        invariant(textIndexVersion == TEXT_INDEX_VERSION_1);
        verify(languageMapV1.find(alias) == languageMapV1.end());
        languageMapV1[alias] = language;
    }
}

FTSLanguage::FTSLanguage() : _canonicalName() {}

const std::string& FTSLanguage::str() const {
    verify(!_canonicalName.empty());
    return _canonicalName;
}

// static
StatusWithFTSLanguage FTSLanguage::make(StringData langName, TextIndexVersion textIndexVersion) {
    if (textIndexVersion >= TEXT_INDEX_VERSION_2) {
        LanguageMap* languageMap =
            (textIndexVersion == TEXT_INDEX_VERSION_3) ? &languageMapV3 : &languageMapV2;

        LanguageMap::const_iterator it = languageMap->find(langName.toString());

        if (it == languageMap->end()) {
            // TEXT_INDEX_VERSION_2 and above reject unrecognized language strings.
            Status status =
                Status(ErrorCodes::BadValue,
                       mongoutils::str::stream() << "unsupported language: \"" << langName
                                                 << "\" for text index version "
                                                 << textIndexVersion);
            return StatusWithFTSLanguage(status);
        }

        return StatusWithFTSLanguage(it->second);
    } else {
        // Legacy text index.
        invariant(textIndexVersion == TEXT_INDEX_VERSION_1);
        LanguageMapLegacy::const_iterator it = languageMapV1.find(langName);
        if (it == languageMapV1.end()) {
            // TEXT_INDEX_VERSION_1 treats unrecognized language strings as "none".
            return StatusWithFTSLanguage(&languageNoneV1);
        }
        return StatusWithFTSLanguage(it->second);
    }
}

std::unique_ptr<FTSTokenizer> BasicFTSLanguage::createTokenizer() const {
    return stdx::make_unique<BasicFTSTokenizer>(this);
}

const FTSPhraseMatcher& BasicFTSLanguage::getPhraseMatcher() const {
    return _basicPhraseMatcher;
}

std::unique_ptr<FTSTokenizer> UnicodeFTSLanguage::createTokenizer() const {
    return stdx::make_unique<UnicodeFTSTokenizer>(this);
}

const FTSPhraseMatcher& UnicodeFTSLanguage::getPhraseMatcher() const {
    return _unicodePhraseMatcher;
}
}
}
