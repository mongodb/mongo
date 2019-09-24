/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <memory>
#include <string>
#include <utility>

#include "mongo/base/status_with.h"
#include "mongo/db/fts/fts_basic_phrase_matcher.h"
#include "mongo/db/fts/fts_phrase_matcher.h"
#include "mongo/db/fts/fts_unicode_phrase_matcher.h"
#include "mongo/db/fts/fts_util.h"

namespace mongo {

namespace fts {

class FTSTokenizer;

/**
 * A FTSLanguage represents a language for a text-indexed document or a text search.
 * FTSLanguage objects are not copyable.
 *
 * Recommended usage:
 *
 *     const auto& language = FTSLanguage::make( "en", TEXT_INDEX_VERSION_3 );
 *     if ( !lang.isOK() ) {
 *         // Error.
 *     }
 *     else {
 *         const FTSLanguage& language = swl.getValue();
 *         // Use language.
 *     }
 */
class FTSLanguage {
public:
    FTSLanguage(std::string canonical, std::unique_ptr<FTSPhraseMatcher> phraseMatcher)
        : _canonicalName{std::move(canonical)}, _phraseMatcher{std::move(phraseMatcher)} {}

    virtual ~FTSLanguage() {}

    // Use make() instead of copying.
    FTSLanguage(const FTSLanguage&) = delete;
    FTSLanguage& operator=(const FTSLanguage&) = delete;

    /**
     * Returns the language in canonical form (lowercased English name).
     */
    const std::string& str() const {
        return _canonicalName;
    }

    /**
     * Returns a new FTSTokenizer instance for this language.
     * Lifetime is scoped to FTSLanguage (which are currently all process lifetime).
     */
    virtual std::unique_ptr<FTSTokenizer> createTokenizer() const = 0;

    /**
     * Returns a reference to the phrase matcher instance that this language owns.
     */
    const FTSPhraseMatcher& getPhraseMatcher() const {
        return *_phraseMatcher;
    }

    /**
     * Return the FTSLanguage associated with the given language string and the given text index
     * version.  Throws an AssertionError if an invalid langName is passed.
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
    static const FTSLanguage& make(StringData langName, TextIndexVersion textIndexVersion);

private:
    std::string _canonicalName;
    std::unique_ptr<FTSPhraseMatcher> _phraseMatcher;
};

/**
 * FTSLanguage implementation that returns a BasicFTSTokenizer and BasicFTSPhraseMatcher for ASCII
 * aware case folding in FTS.
 */
class BasicFTSLanguage : public FTSLanguage {
public:
    explicit BasicFTSLanguage(const std::string& languageName)
        : FTSLanguage(languageName, std::make_unique<BasicFTSPhraseMatcher>()) {}
    std::unique_ptr<FTSTokenizer> createTokenizer() const final;
};

/**
 * FTSLanguage implementation that returns a UnicodeFTSTokenizer and UnicodeFTSPhraseMatcher for
 * Unicode aware case folding and diacritic removal in FTS.
 */
class UnicodeFTSLanguage : public FTSLanguage {
public:
    explicit UnicodeFTSLanguage(const std::string& languageName)
        : FTSLanguage(languageName, std::make_unique<UnicodeFTSPhraseMatcher>(languageName)) {}
    std::unique_ptr<FTSTokenizer> createTokenizer() const final;
};

}  // namespace fts
}  // namespace mongo
