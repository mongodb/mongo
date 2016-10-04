/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/fts/fts_spec.h"

#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/stringutils.h"

namespace mongo {

namespace fts {

//
// This file contains functionality specific to indexing documents from TEXT_INDEX_VERSION_1
// text indexes.
//

using std::map;
using std::string;
using namespace mongoutils;

namespace dps = ::mongo::dotted_path_support;

namespace {
void _addFTSStuff(BSONObjBuilder* b) {
    b->append("_fts", INDEX_NAME);
    b->append("_ftsx", 1);
}
}

const FTSLanguage& FTSSpec::_getLanguageToUseV1(const BSONObj& userDoc) const {
    BSONElement e = userDoc[_languageOverrideField];
    if (e.type() == String) {
        const char* x = e.valuestrsafe();
        if (strlen(x) > 0) {
            StatusWithFTSLanguage swl = FTSLanguage::make(x, TEXT_INDEX_VERSION_1);
            dassert(swl.isOK());  // make() w/ TEXT_INDEX_VERSION_1 guaranteed to not fail.
            return *swl.getValue();
        }
    }
    return *_defaultLanguage;
}

void FTSSpec::_scoreStringV1(const Tools& tools,
                             StringData raw,
                             TermFrequencyMap* docScores,
                             double weight) const {
    ScoreHelperMap terms;

    unsigned numTokens = 0;

    Tokenizer i(&tools.language, raw);
    while (i.more()) {
        Token t = i.next();
        if (t.type != Token::TEXT)
            continue;

        string term = tolowerString(t.data);
        if (tools.stopwords->isStopWord(term))
            continue;
        term = tools.stemmer->stem(term).toString();

        ScoreHelperStruct& data = terms[term];

        if (data.exp)
            data.exp *= 2;
        else
            data.exp = 1;
        data.count += 1;
        data.freq += (1 / data.exp);

        numTokens++;
    }

    for (ScoreHelperMap::const_iterator i = terms.begin(); i != terms.end(); ++i) {
        const string& term = i->first;
        const ScoreHelperStruct& data = i->second;

        // in order to adjust weights as a function of term count as it
        // relates to total field length. ie. is this the only word or
        // a frequently occuring term? or does it only show up once in
        // a long block of text?

        double coeff = (0.5 * data.count / numTokens) + 0.5;

        // if term is identical to the raw form of the
        // field (untokenized) give it a small boost.
        double adjustment = 1;
        if (raw.size() == term.length() && raw.equalCaseInsensitive(term))
            adjustment += 0.1;

        double& score = (*docScores)[term];
        score += (weight * data.freq * coeff * adjustment);
        verify(score <= MAX_WEIGHT);
    }
}

bool FTSSpec::_weightV1(StringData field, double* out) const {
    Weights::const_iterator i = _weights.find(field.toString());
    if (i == _weights.end())
        return false;
    *out = i->second;
    return true;
}

/*
 * Recurses over all fields of an obj (document in collection)
 *    and fills term,score map term_freqs
 * @param tokenizer, tokenizer to tokenize a string into terms
 * @param obj, object being parsed
 * term_freqs, map <term,score> to be filled up
 */
void FTSSpec::_scoreRecurseV1(const Tools& tools,
                              const BSONObj& obj,
                              TermFrequencyMap* term_freqs) const {
    BSONObjIterator j(obj);
    while (j.more()) {
        BSONElement x = j.next();

        if (languageOverrideField() == x.fieldName())
            continue;

        if (x.type() == String) {
            double w = 1;
            _weightV1(x.fieldName(), &w);
            _scoreStringV1(tools, x.valuestr(), term_freqs, w);
        } else if (x.isABSONObj()) {
            _scoreRecurseV1(tools, x.Obj(), term_freqs);
        }
    }
}

void FTSSpec::_scoreDocumentV1(const BSONObj& obj, TermFrequencyMap* term_freqs) const {
    const FTSLanguage& language = _getLanguageToUseV1(obj);

    Stemmer stemmer(&language);
    Tools tools(language, &stemmer, StopWords::getStopWords(&language));

    if (wildcard()) {
        // if * is specified for weight, we can recurse over all fields.
        _scoreRecurseV1(tools, obj, term_freqs);
        return;
    }

    // otherwise, we need to remember the different weights for each field
    // and act accordingly (in other words, call _score)
    for (Weights::const_iterator i = _weights.begin(); i != _weights.end(); i++) {
        const char* leftOverName = i->first.c_str();
        // name of field
        BSONElement e = dps::extractElementAtPath(obj, leftOverName);
        // weight associated to name of field
        double weight = i->second;

        if (e.eoo()) {
            // do nothing
        } else if (e.type() == Array) {
            BSONObjIterator j(e.Obj());
            while (j.more()) {
                BSONElement x = j.next();
                if (leftOverName[0] && x.isABSONObj())
                    x = dps::extractElementAtPath(x.Obj(), leftOverName);
                if (x.type() == String)
                    _scoreStringV1(tools, x.valuestr(), term_freqs, weight);
            }
        } else if (e.type() == String) {
            _scoreStringV1(tools, e.valuestr(), term_freqs, weight);
        }
    }
}

StatusWith<BSONObj> FTSSpec::_fixSpecV1(const BSONObj& spec) {
    map<string, int> m;

    BSONObj keyPattern;
    {
        BSONObjBuilder b;
        bool addedFtsStuff = false;

        BSONObjIterator i(spec["key"].Obj());
        while (i.more()) {
            BSONElement e = i.next();
            if (str::equals(e.fieldName(), "_fts") || str::equals(e.fieldName(), "_ftsx")) {
                addedFtsStuff = true;
                b.append(e);
            } else if (e.type() == String &&
                       (str::equals("fts", e.valuestr()) || str::equals("text", e.valuestr()))) {
                if (!addedFtsStuff) {
                    _addFTSStuff(&b);
                    addedFtsStuff = true;
                }

                m[e.fieldName()] = 1;
            } else {
                b.append(e);
            }
        }

        if (!addedFtsStuff)
            _addFTSStuff(&b);

        keyPattern = b.obj();
    }

    if (spec["weights"].isABSONObj()) {
        BSONObjIterator i(spec["weights"].Obj());
        while (i.more()) {
            BSONElement e = i.next();
            m[e.fieldName()] = e.numberInt();
        }
    } else if (spec["weights"].str() == WILDCARD) {
        m[WILDCARD] = 1;
    }

    BSONObj weights;
    {
        BSONObjBuilder b;
        for (map<string, int>::iterator i = m.begin(); i != m.end(); ++i) {
            if (i->second <= 0 || i->second >= MAX_WORD_WEIGHT) {
                return {ErrorCodes::CannotCreateIndex,
                        str::stream() << "text index weight must be in the exclusive interval (0,"
                                      << MAX_WORD_WEIGHT
                                      << ") but found: "
                                      << i->second};
            }
            b.append(i->first, i->second);
        }
        weights = b.obj();
    }

    string default_language(spec.getStringField("default_language"));
    if (default_language.empty())
        default_language = "english";

    string language_override(spec.getStringField("language_override"));
    if (language_override.empty())
        language_override = "language";

    int version = -1;
    int textIndexVersion = 1;

    BSONObjBuilder b;
    BSONObjIterator i(spec);
    while (i.more()) {
        BSONElement e = i.next();
        if (str::equals(e.fieldName(), "key")) {
            b.append("key", keyPattern);
        } else if (str::equals(e.fieldName(), "weights")) {
            b.append("weights", weights);
            weights = BSONObj();
        } else if (str::equals(e.fieldName(), "default_language")) {
            b.append("default_language", default_language);
            default_language = "";
        } else if (str::equals(e.fieldName(), "language_override")) {
            b.append("language_override", language_override);
            language_override = "";
        } else if (str::equals(e.fieldName(), "v")) {
            version = e.numberInt();
        } else if (str::equals(e.fieldName(), "textIndexVersion")) {
            textIndexVersion = e.numberInt();
            if (textIndexVersion != 1) {
                return {ErrorCodes::CannotCreateIndex,
                        str::stream() << "bad textIndexVersion: " << textIndexVersion};
            }
        } else {
            b.append(e);
        }
    }

    if (!weights.isEmpty())
        b.append("weights", weights);
    if (!default_language.empty())
        b.append("default_language", default_language);
    if (!language_override.empty())
        b.append("language_override", language_override);

    if (version >= 0)
        b.append("v", version);

    b.append("textIndexVersion", textIndexVersion);

    return b.obj();
}
}
}
