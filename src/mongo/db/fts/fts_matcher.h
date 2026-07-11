// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/fts/fts_language.h"
#include "mongo/db/fts/fts_query_impl.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/fts/fts_tokenizer.h"
#include "mongo/db/fts/tokenizer.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace mongo {

namespace fts {

class FTSMatcher {
    FTSMatcher(const FTSMatcher&) = delete;
    FTSMatcher& operator=(const FTSMatcher&) = delete;

public:
    FTSMatcher(const FTSQueryImpl& query, const FTSSpec& spec);

    /**
     * Returns whether 'obj' matches the query.  An object is considered to match the query
     * if all four of the following conditions hold:
     * 1) The object contains at least one positive term.
     * 2) The object contains zero negative terms.
     * 3) The object contains all positive phrases.
     * 4) The object contains zero negative phrases.
     */
    bool matches(const BSONObj& obj) const;

    /**
     * Returns whether 'obj' contains at least one positive term.
     */
    bool hasPositiveTerm(const BSONObj& obj) const;

    /**
     * Returns whether 'obj' contains at least one negative term.
     */
    bool hasNegativeTerm(const BSONObj& obj) const;

    /**
     * Returns whether 'obj' contains all positive phrases.
     */
    bool positivePhrasesMatch(const BSONObj& obj) const;

    /**
     * Returns whether 'obj' contains zero negative phrases.
     */
    bool negativePhrasesMatch(const BSONObj& obj) const;

    const FTSQueryImpl& query() const {
        return _query;
    }

    const FTSSpec& spec() const {
        return _spec;
    }

    size_t getApproximateSize() const;

private:
    /**
     * For matching, can we skip the positive term check?  This is done as optimization when
     * we have a-priori knowledge that all documents being matched pass the positive term
     * check.
     */
    bool canSkipPositiveTermCheck() const {
        return !_query.getCaseSensitive() && !_query.getDiacriticSensitive();
    }

    /**
     * Returns whether the string 'raw' contains any positive terms from the query.
     * 'language' specifies the language for 'raw'.
     */
    bool _hasPositiveTerm_string(const FTSLanguage* language, std::string_view raw) const;

    /**
     * Returns whether the string 'raw' contains any negative terms from the query.
     * 'language' specifies the language for 'raw'.
     */
    bool _hasNegativeTerm_string(const FTSLanguage* language, std::string_view raw) const;

    /**
     * Returns whether 'obj' contains the exact string 'phrase' in any indexed fields.
     */
    bool _phraseMatch(const std::string& phrase, const BSONObj& obj) const;

    /**
     * Helper method that returns the tokenizer options that this matcher should use, based on the
     * the query options.
     */
    FTSTokenizer::Options _getTokenizerOptions() const;

    // TODO These should be unowned pointers instead of owned copies.
    const FTSQueryImpl _query;
    const FTSSpec _spec;
};
}  // namespace fts
}  // namespace mongo
