// fts_language.h

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

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/db/fts/fts_basic_phrase_matcher.h"
#include "mongo/db/fts/fts_phrase_matcher.h"
#include "mongo/db/fts/fts_unicode_phrase_matcher.h"
#include "mongo/db/fts/fts_util.h"

#include <string>

namespace mongo {

namespace fts {

class FTSTokenizer;

// Legacy language initialization.
#define MONGO_FTS_LANGUAGE_DECLARE(language, name, version)                                    \
    BasicFTSLanguage language;                                                                 \
    MONGO_INITIALIZER_GENERAL(language, MONGO_NO_PREREQUISITES, ("FTSAllLanguagesRegistered")) \
    (::mongo::InitializerContext * context) {                                                  \
        FTSLanguage::registerLanguage(name, version, &language);                               \
        return Status::OK();                                                                   \
    }

/**
 * A FTSLanguage represents a language for a text-indexed document or a text search.
 * FTSLanguage objects are not copyable.
 *
 * Recommended usage:
 *
 *     StatusWithFTSLanguage swl = FTSLanguage::make( "en", TEXT_INDEX_VERSION_3 );
 *     if ( !swl.getStatus().isOK() ) {
 *         // Error.
 *     }
 *     else {
 *         const FTSLanguage* language = swl.getValue();
 *         // Use language.
 *     }
 */
class FTSLanguage {
    // Use make() instead of copying.
    MONGO_DISALLOW_COPYING(FTSLanguage);

public:
    /** Create an uninitialized language. */
    FTSLanguage();

    virtual ~FTSLanguage() {}

    /**
     * Returns the language as a std::string in canonical form (lowercased English name).  It is
     * an error to call str() on an uninitialized language.
     */
    const std::string& str() const;

    /**
     * Returns a new FTSTokenizer instance for this language.
     * Lifetime is scoped to FTSLanguage (which are currently all process lifetime).
     */
    virtual std::unique_ptr<FTSTokenizer> createTokenizer() const = 0;

    /**
     * Returns a reference to the phrase matcher instance that this language owns.
     */
    virtual const FTSPhraseMatcher& getPhraseMatcher() const = 0;

    /**
     * Register std::string 'languageName' as a new language with the text index version
     * 'textIndexVersion'.  Saves the resulting language to out-argument 'languageOut'.
     * Subsequent calls to FTSLanguage::make() will recognize the newly-registered language string.
     */
    static void registerLanguage(StringData languageName,
                                 TextIndexVersion textIndexVersion,
                                 FTSLanguage* languageOut);

    /**
     * Register 'alias' as an alias for 'language' with text index version
     * 'textIndexVersion'.  Subsequent calls to FTSLanguage::make() will recognize the
     * newly-registered alias.
     */
    static void registerLanguageAlias(const FTSLanguage* language,
                                      StringData alias,
                                      TextIndexVersion textIndexVersion);

    /**
     * Return the FTSLanguage associated with the given language string and the given text index
     * version.  Returns an error Status if an invalid language std::string is passed.
     *
     * For textIndexVersion >= TEXT_INDEX_VERSION_2, language strings are
     * case-insensitive, and need to be in one of the two following forms:
     * - English name, like "spanish".
     * - Two-letter code, like "es".
     *
     * For textIndexVersion == TEXT_INDEX_VERSION_1, no validation or normalization of
     * language strings is performed.  This is necessary to preserve indexing behavior for
     * documents with language strings like "en": for compatibility, text data in these
     * documents needs to be processed with the English stemmer and the empty stopword list
     * (since "en" is recognized by Snowball but not the stopword processing logic).
     */
    static StatusWith<const FTSLanguage*> make(StringData langName,
                                               TextIndexVersion textIndexVersion);

private:
    // std::string representation of language in canonical form.
    std::string _canonicalName;
};

typedef StatusWith<const FTSLanguage*> StatusWithFTSLanguage;

/**
 * FTSLanguage implementation that returns a BasicFTSTokenizer and BasicFTSPhraseMatcher for ASCII
 * aware case folding in FTS.
 */
class BasicFTSLanguage : public FTSLanguage {
public:
    std::unique_ptr<FTSTokenizer> createTokenizer() const final;
    const FTSPhraseMatcher& getPhraseMatcher() const final;

private:
    BasicFTSPhraseMatcher _basicPhraseMatcher;
};

/**
 * FTSLanguage implementation that returns a UnicodeFTSTokenizer and UnicodeFTSPhraseMatcher for
 * Unicode aware case folding and diacritic removal in FTS.
 */
class UnicodeFTSLanguage : public FTSLanguage {
public:
    UnicodeFTSLanguage(const std::string& languageName) : _unicodePhraseMatcher(languageName) {}
    std::unique_ptr<FTSTokenizer> createTokenizer() const final;
    const FTSPhraseMatcher& getPhraseMatcher() const final;

private:
    UnicodeFTSPhraseMatcher _unicodePhraseMatcher;
};

extern BasicFTSLanguage languagePorterV1;
extern BasicFTSLanguage languageEnglishV2;
extern BasicFTSLanguage languageFrenchV2;
}
}
