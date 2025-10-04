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

#include "mongo/db/fts/fts_matcher.h"

#include "mongo/base/string_data.h"
#include "mongo/db/fts/fts_element_iterator.h"
#include "mongo/db/fts/fts_phrase_matcher.h"
#include "mongo/db/fts/fts_tokenizer.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <set>
#include <vector>

namespace mongo {

namespace fts {

using std::string;

FTSMatcher::FTSMatcher(const FTSQueryImpl& query, const FTSSpec& spec)
    : _query(query), _spec(spec) {}

bool FTSMatcher::matches(const BSONObj& obj) const {
    if (canSkipPositiveTermCheck()) {
        // We can assume that 'obj' has at least one positive term, and dassert as a sanity
        // check.
        dassert(hasPositiveTerm(obj));
    } else {
        if (!hasPositiveTerm(obj)) {
            return false;
        }
    }

    if (hasNegativeTerm(obj)) {
        return false;
    }

    if (!positivePhrasesMatch(obj)) {
        return false;
    }

    return negativePhrasesMatch(obj);
}

bool FTSMatcher::hasPositiveTerm(const BSONObj& obj) const {
    FTSElementIterator it(_spec, obj);

    while (it.more()) {
        FTSIteratorValue val = it.next();
        if (_hasPositiveTerm_string(val._language, val._text)) {
            return true;
        }
    }

    return false;
}

bool FTSMatcher::_hasPositiveTerm_string(const FTSLanguage* language, const string& raw) const {
    std::unique_ptr<FTSTokenizer> tokenizer(language->createTokenizer());
    tokenizer->reset(raw.c_str(), _getTokenizerOptions());

    while (tokenizer->moveNext()) {
        string word = std::string{tokenizer->get()};
        if (_query.getPositiveTerms().count(word) > 0) {
            return true;
        }
    }
    return false;
}

bool FTSMatcher::hasNegativeTerm(const BSONObj& obj) const {
    if (_query.getNegatedTerms().size() == 0) {
        return false;
    }

    FTSElementIterator it(_spec, obj);

    while (it.more()) {
        FTSIteratorValue val = it.next();
        if (_hasNegativeTerm_string(val._language, val._text)) {
            return true;
        }
    }

    return false;
}

bool FTSMatcher::_hasNegativeTerm_string(const FTSLanguage* language, const string& raw) const {
    std::unique_ptr<FTSTokenizer> tokenizer(language->createTokenizer());
    tokenizer->reset(raw.c_str(), _getTokenizerOptions());

    while (tokenizer->moveNext()) {
        string word = std::string{tokenizer->get()};
        if (_query.getNegatedTerms().count(word) > 0) {
            return true;
        }
    }
    return false;
}

bool FTSMatcher::positivePhrasesMatch(const BSONObj& obj) const {
    for (size_t i = 0; i < _query.getPositivePhr().size(); i++) {
        if (!_phraseMatch(_query.getPositivePhr()[i], obj)) {
            return false;
        }
    }

    return true;
}

bool FTSMatcher::negativePhrasesMatch(const BSONObj& obj) const {
    for (size_t i = 0; i < _query.getNegatedPhr().size(); i++) {
        if (_phraseMatch(_query.getNegatedPhr()[i], obj)) {
            return false;
        }
    }

    return true;
}

size_t FTSMatcher::getApproximateSize() const {
    auto size = sizeof(FTSMatcher);
    size += _query.getApproximateSize() - sizeof(_query);
    size += _spec.getApproximateSize() - sizeof(_spec);
    return size;
}

bool FTSMatcher::_phraseMatch(const string& phrase, const BSONObj& obj) const {
    FTSElementIterator it(_spec, obj);

    while (it.more()) {
        FTSIteratorValue val = it.next();

        FTSPhraseMatcher::Options matcherOptions = FTSPhraseMatcher::kNone;

        if (_query.getCaseSensitive()) {
            matcherOptions |= FTSPhraseMatcher::kCaseSensitive;
        }
        if (_query.getDiacriticSensitive()) {
            matcherOptions |= FTSPhraseMatcher::kDiacriticSensitive;
        }

        if (val._language->getPhraseMatcher().phraseMatches(phrase, val._text, matcherOptions)) {
            return true;
        }
    }

    return false;
}

FTSTokenizer::Options FTSMatcher::_getTokenizerOptions() const {
    FTSTokenizer::Options tokenizerOptions = FTSTokenizer::kNone;

    if (_query.getCaseSensitive()) {
        tokenizerOptions |= FTSTokenizer::kGenerateCaseSensitiveTokens;
    }
    if (_query.getDiacriticSensitive()) {
        tokenizerOptions |= FTSTokenizer::kGenerateDiacriticSensitiveTokens;
    }

    return tokenizerOptions;
}
}  // namespace fts
}  // namespace mongo
