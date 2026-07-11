// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/db/fts/fts_basic_phrase_matcher.h"
#include "mongo/db/fts/fts_phrase_matcher.h"
#include "mongo/db/fts/fts_unicode_phrase_matcher.h"
#include "mongo/db/fts/fts_util.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

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
    static const FTSLanguage& make(std::string_view langName, TextIndexVersion textIndexVersion);

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
