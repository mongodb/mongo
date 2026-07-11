// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/fts/fts_query_impl.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/fts/fts_language.h"
#include "mongo/db/fts/fts_query_parser.h"
#include "mongo/db/fts/fts_tokenizer.h"
#include "mongo/util/assert_util.h"

#include <iosfwd>
#include <memory>
#include <string_view>
#include <utility>

namespace mongo {

namespace fts {

Status FTSQueryImpl::parse(TextIndexVersion textIndexVersion) {
    const FTSLanguage* ftsLanguage;
    try {
        ftsLanguage = &FTSLanguage::make(getLanguage(), textIndexVersion);
    } catch (const DBException& e) {
        return e.toStatus();
    }

    // Build a space delimited list of words to have the FtsTokenizer tokenize
    std::string positiveTermSentence;
    std::string negativeTermSentence;

    bool inNegation = false;
    bool inPhrase = false;

    unsigned quoteOffset = 0;

    FTSQueryParser i(getQuery());
    while (i.more()) {
        QueryToken t = i.next();

        if (t.type == QueryToken::TEXT) {
            std::string s{t.data};

            if (inPhrase && inNegation) {
                // don't add term
            } else {
                // A negation should only continue until the next whitespace character. For example,
                // "-foo" should negate "foo", "- foo" should not negate "foo", and "-foo-bar"
                // should negate both "foo" and "bar".
                if (inNegation && t.previousWhiteSpace) {
                    inNegation = false;
                }

                if (inNegation) {
                    negativeTermSentence.append(s);
                    negativeTermSentence.push_back(' ');
                } else {
                    positiveTermSentence.append(s);
                    positiveTermSentence.push_back(' ');
                }
            }
        } else if (t.type == QueryToken::DELIMITER) {
            char c = t.data[0];
            if (c == '-') {
                if (!inPhrase && t.previousWhiteSpace) {
                    // phrases can be negated, and terms not in phrases can be negated.
                    // terms in phrases can not be negated.
                    inNegation = true;
                }
            } else if (c == '"') {
                if (inPhrase) {
                    // end of a phrase
                    unsigned phraseStart = quoteOffset + 1;
                    unsigned phraseLength = t.offset - phraseStart;
                    std::string_view phrase =
                        std::string_view(getQuery()).substr(phraseStart, phraseLength);
                    if (inNegation) {
                        _negatedPhrases.push_back(std::string{phrase});
                    } else {
                        _positivePhrases.push_back(std::string{phrase});
                    }

                    // Do not reset 'inNegation' here, since a negation should continue until the
                    // next whitespace character. For example, '-"foo bar"-"baz quux"' should negate
                    // both the phrase "foo bar" and the phrase "baz quux".

                    inPhrase = false;
                } else {
                    // start of a phrase
                    inPhrase = true;
                    // A "-" should only be treated as a negation if there is no whitespace between
                    // the "-" and the start of the phrase.
                    if (inNegation && t.previousWhiteSpace) {
                        inNegation = false;
                    }
                    quoteOffset = t.offset;
                }
            }
        } else {
            MONGO_UNREACHABLE;
        }
    }

    std::unique_ptr<FTSTokenizer> tokenizer = ftsLanguage->createTokenizer();

    _addTerms(tokenizer.get(), positiveTermSentence, false);
    _addTerms(tokenizer.get(), negativeTermSentence, true);

    return Status::OK();
}

std::unique_ptr<FTSQuery> FTSQueryImpl::clone() const {
    auto clonedQuery = std::make_unique<FTSQueryImpl>();
    clonedQuery->setQuery(getQuery());
    clonedQuery->setLanguage(getLanguage());
    clonedQuery->setCaseSensitive(getCaseSensitive());
    clonedQuery->setDiacriticSensitive(getDiacriticSensitive());
    clonedQuery->_positiveTerms = _positiveTerms;
    clonedQuery->_negatedTerms = _negatedTerms;
    clonedQuery->_positivePhrases = _positivePhrases;
    clonedQuery->_negatedPhrases = _negatedPhrases;
    clonedQuery->_termsForBounds = _termsForBounds;
    return std::move(clonedQuery);
}

void FTSQueryImpl::_addTerms(FTSTokenizer* tokenizer, const std::string& sentence, bool negated) {
    tokenizer->reset(sentence.c_str(), FTSTokenizer::kFilterStopWords);

    auto& activeTerms = negated ? _negatedTerms : _positiveTerms;

    // First, get all the terms for indexing, ie, lower cased words
    // If we are case-insensitive, we can also used this for positive, and negative terms
    // Some terms may be expanded into multiple words in some non-English languages
    while (tokenizer->moveNext()) {
        std::string word{tokenizer->get()};

        if (!negated) {
            _termsForBounds.insert(word);
        }

        // Compute the string corresponding to 'token' that will be used for the matcher.
        // For case and diacritic insensitive queries, this is the same string as 'boundsTerm'
        // computed above.
        if (!getCaseSensitive() && !getDiacriticSensitive()) {
            activeTerms.insert(word);
        }
    }

    if (!getCaseSensitive() && !getDiacriticSensitive()) {
        return;
    }

    FTSTokenizer::Options newOptions = FTSTokenizer::kFilterStopWords;

    if (getCaseSensitive()) {
        newOptions |= FTSTokenizer::kGenerateCaseSensitiveTokens;
    }
    if (getDiacriticSensitive()) {
        newOptions |= FTSTokenizer::kGenerateDiacriticSensitiveTokens;
    }

    tokenizer->reset(sentence.c_str(), newOptions);

    // If we want case-sensitivity or diacritic sensitivity, get the correct token.
    while (tokenizer->moveNext()) {
        std::string word{tokenizer->get()};
        activeTerms.insert(word);
    }
}

BSONObj FTSQueryImpl::toBSON() const {
    BSONObjBuilder bob;
    auto appendRange = [&](std::string_view name, const auto& seq) {
        bob.append(name, seq.begin(), seq.end());
    };
    appendRange("terms", getPositiveTerms());
    appendRange("negatedTerms", getNegatedTerms());
    appendRange("phrases", getPositivePhr());
    appendRange("negatedPhrases", getNegatedPhr());
    return bob.obj();
}

size_t FTSQueryImpl::getApproximateSize() const {
    auto computeVectorSize = [](const std::vector<std::string>& v) {
        size_t size = 0;
        for (const auto& str : v) {
            size += sizeof(std::string) + str.size() + 1;
        }
        return size;
    };

    auto computeSetSize = [](auto&& s) {
        size_t size = 0;
        for (const auto& str : s) {
            size += sizeof(std::string) + str.size() + 1;
        }
        return size;
    };

    auto size = sizeof(FTSQueryImpl);
    size += FTSQuery::getApproximateSize() - sizeof(FTSQuery);
    size += computeSetSize(_positiveTerms);
    size += computeSetSize(_negatedTerms);
    size += computeVectorSize(_positivePhrases);
    size += computeVectorSize(_negatedPhrases);
    size += computeSetSize(_termsForBounds);
    return size;
}
}  // namespace fts
}  // namespace mongo
