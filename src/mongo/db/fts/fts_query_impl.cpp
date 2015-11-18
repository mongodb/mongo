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

#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/fts/fts_query_parser.h"
#include "mongo/db/fts/fts_tokenizer.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/stringutils.h"

namespace mongo {

namespace fts {

using namespace mongoutils;

using std::set;
using std::string;
using std::stringstream;
using std::vector;

Status FTSQueryImpl::parse(const string& query,
                           StringData language,
                           bool caseSensitive,
                           bool diacriticSensitive,
                           TextIndexVersion textIndexVersion) {
    StatusWithFTSLanguage swl = FTSLanguage::make(language, textIndexVersion);
    if (!swl.getStatus().isOK()) {
        return swl.getStatus();
    }
    _language = swl.getValue();
    _caseSensitive = caseSensitive;
    _diacriticSensitive = diacriticSensitive;

    // Build a space delimited list of words to have the FtsTokenizer tokenize
    string positiveTermSentence;
    string negativeTermSentence;

    bool inNegation = false;
    bool inPhrase = false;

    unsigned quoteOffset = 0;

    FTSQueryParser i(query);
    while (i.more()) {
        QueryToken t = i.next();

        if (t.type == QueryToken::TEXT) {
            string s = t.data.toString();

            if (inPhrase && inNegation) {
                // don't add term
            } else {
                if (inNegation) {
                    negativeTermSentence.append(s);
                    negativeTermSentence.push_back(' ');
                } else {
                    positiveTermSentence.append(s);
                    positiveTermSentence.push_back(' ');
                }
            }

            if (inNegation && !inPhrase)
                inNegation = false;
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
                    StringData phrase = StringData(query).substr(phraseStart, phraseLength);
                    if (inNegation) {
                        _negatedPhrases.push_back(phrase.toString());
                    } else {
                        _positivePhrases.push_back(phrase.toString());
                    }
                    inNegation = false;
                    inPhrase = false;
                } else {
                    // start of a phrase
                    inPhrase = true;
                    quoteOffset = t.offset;
                }
            }
        } else {
            invariant(false);
        }
    }

    std::unique_ptr<FTSTokenizer> tokenizer(_language->createTokenizer());

    _addTerms(tokenizer.get(), positiveTermSentence, false);
    _addTerms(tokenizer.get(), negativeTermSentence, true);

    return Status::OK();
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
        if (!_caseSensitive && !_diacriticSensitive) {
            activeTerms.insert(word);
        }
    }

    if (!_caseSensitive && !_diacriticSensitive) {
        return;
    }

    FTSTokenizer::Options newOptions = FTSTokenizer::kFilterStopWords;

    if (_caseSensitive) {
        newOptions |= FTSTokenizer::kGenerateCaseSensitiveTokens;
    }
    if (_diacriticSensitive) {
        newOptions |= FTSTokenizer::kGenerateDiacriticSensitiveTokens;
    }

    tokenizer->reset(sentence.c_str(), newOptions);

    // If we want case-sensitivity or diacritic sensitivity, get the correct token.
    while (tokenizer->moveNext()) {
        string word = tokenizer->get().toString();

        activeTerms.insert(word);
    }
}

namespace {
void _debugHelp(stringstream& ss, const set<string>& s, const string& sep) {
    bool first = true;
    for (set<string>::const_iterator i = s.begin(); i != s.end(); ++i) {
        if (first)
            first = false;
        else
            ss << sep;
        ss << *i;
    }
}

void _debugHelp(stringstream& ss, const vector<string>& v, const string& sep) {
    set<string> s(v.begin(), v.end());
    _debugHelp(ss, s, sep);
}
}

string FTSQueryImpl::toString() const {
    stringstream ss;
    ss << "FTSQueryImpl\n";

    ss << "  terms: ";
    _debugHelp(ss, getPositiveTerms(), ", ");
    ss << "\n";

    ss << "  negated terms: ";
    _debugHelp(ss, getNegatedTerms(), ", ");
    ss << "\n";

    ss << "  phrases: ";
    _debugHelp(ss, getPositivePhr(), ", ");
    ss << "\n";

    ss << "  negated phrases: ";
    _debugHelp(ss, getNegatedPhr(), ", ");
    ss << "\n";

    return ss.str();
}

string FTSQueryImpl::debugString() const {
    stringstream ss;

    _debugHelp(ss, getPositiveTerms(), "|");
    ss << "||";

    _debugHelp(ss, getNegatedTerms(), "|");
    ss << "||";

    _debugHelp(ss, getPositivePhr(), "|");
    ss << "||";

    _debugHelp(ss, getNegatedPhr(), "|");

    return ss.str();
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
