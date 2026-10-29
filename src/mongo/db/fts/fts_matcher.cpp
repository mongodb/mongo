// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/fts/fts_matcher.h"

#include "mongo/db/fts/fts_element_iterator.h"
#include "mongo/db/fts/fts_phrase_matcher.h"
#include "mongo/db/fts/fts_tokenizer.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <set>
#include <string_view>
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
        if (_hasPositiveTerm_string(val.language(), val.text())) {
            return true;
        }
    }

    return false;
}

bool FTSMatcher::_hasPositiveTerm_string(const FTSLanguage* language, std::string_view raw) const {
    std::unique_ptr<FTSTokenizer> tokenizer(language->createTokenizer());
    tokenizer->reset(raw, _getTokenizerOptions());

    while (tokenizer->moveNext()) {
        // This map can heterogeneously lookup `std::string_view` directly.
        if (_query.getPositiveTerms().count(tokenizer->get())) {
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
        if (_hasNegativeTerm_string(val.language(), val.text())) {
            return true;
        }
    }

    return false;
}

bool FTSMatcher::_hasNegativeTerm_string(const FTSLanguage* language, std::string_view raw) const {
    std::unique_ptr<FTSTokenizer> tokenizer(language->createTokenizer());
    tokenizer->reset(raw, _getTokenizerOptions());

    while (tokenizer->moveNext()) {
        if (_query.getNegatedTerms().count(tokenizer->get())) {
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

        if (val.language()->getPhraseMatcher().phraseMatches(phrase, val.text(), matcherOptions)) {
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
