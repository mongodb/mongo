// fts_matcher.h

/**
*    Copyright (C) 2012 10gen Inc.
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

#include "mongo/db/fts/fts_query_impl.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/fts/fts_tokenizer.h"
#include "mongo/db/fts/tokenizer.h"

namespace mongo {

namespace fts {

class FTSMatcher {
    MONGO_DISALLOW_COPYING(FTSMatcher);

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
    bool _hasPositiveTerm_string(const FTSLanguage* language, const std::string& raw) const;

    /**
     * Returns whether the string 'raw' contains any negative terms from the query.
     * 'language' specifies the language for 'raw'.
     */
    bool _hasNegativeTerm_string(const FTSLanguage* language, const std::string& raw) const;

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
}
}
