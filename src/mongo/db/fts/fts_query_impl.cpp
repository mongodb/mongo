// fts_query_impl.cpp

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

#include "mongo/platform/basic.h"

#include "mongo/db/fts/fts_query_impl.h"

#include "mongo/db/fts/fts_query_parser.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/fts/fts_tokenizer.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/stringutils.h"

namespace mongo {

namespace fts {

using namespace mongoutils;

using std::set;
using std::string;
using std::stringstream;
using std::vector;

Status FTSQueryImpl::parse(TextIndexVersion textIndexVersion) {
    StatusWithFTSLanguage ftsLanguage = FTSLanguage::make(getLanguage(), textIndexVersion);
    if (!ftsLanguage.getStatus().isOK()) {
        return ftsLanguage.getStatus();
    }

    // Build a space delimited list of words to have the FtsTokenizer tokenize
    string positiveTermSentence;
    string negativeTermSentence;

    bool inNegation = false;
    bool inPhrase = false;

    unsigned quoteOffset = 0;

    FTSQueryParser i(getQuery());
    while (i.more()) {
        QueryToken t = i.next();

        if (t.type == QueryToken::TEXT) {
            string s = t.data.toString();

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
                    StringData phrase = StringData(getQuery()).substr(phraseStart, phraseLength);
                    if (inNegation) {
                        _negatedPhrases.push_back(phrase.toString());
                    } else {
                        _positivePhrases.push_back(phrase.toString());
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
            invariant(false);
        }
    }

    std::unique_ptr<FTSTokenizer> tokenizer(ftsLanguage.getValue()->createTokenizer());

    _addTerms(tokenizer.get(), positiveTermSentence, false);
    _addTerms(tokenizer.get(), negativeTermSentence, true);

    return Status::OK();
}

std::unique_ptr<FTSQuery> FTSQueryImpl::clone() const {
    auto clonedQuery = stdx::make_unique<FTSQueryImpl>();
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

void FTSQueryImpl::_addTerms(FTSTokenizer* tokenizer, const string& sentence, bool negated) {
    tokenizer->reset(sentence.c_str(), FTSTokenizer::kFilterStopWords);

    auto& activeTerms = negated ? _negatedTerms : _positiveTerms;

    // First, get all the terms for indexing, ie, lower cased words
    // If we are case-insensitive, we can also used this for positive, and negative terms
    // Some terms may be expanded into multiple words in some non-English languages
    while (tokenizer->moveNext()) {
        string word = tokenizer->get().toString();

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
        string word = tokenizer->get().toString();

        activeTerms.insert(word);
    }
}

BSONObj FTSQueryImpl::toBSON() const {
    BSONObjBuilder bob;
    bob.append("terms", getPositiveTerms());
    bob.append("negatedTerms", getNegatedTerms());
    bob.append("phrases", getPositivePhr());
    bob.append("negatedPhrases", getNegatedPhr());
    return bob.obj();
}
}
}
