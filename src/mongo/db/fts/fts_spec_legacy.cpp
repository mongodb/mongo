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

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/dotted_path/dotted_path_support.h"
#include "mongo/db/fts/fts_language.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/fts/fts_util.h"
#include "mongo/db/fts/stemmer.h"
#include "mongo/db/fts/stop_words.h"
#include "mongo/db/fts/tokenizer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <map>
#include <string>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>

namespace mongo {

namespace fts {

//
// This file contains functionality specific to indexing documents from TEXT_INDEX_VERSION_1
// text indexes.
//

namespace {
void _addFTSStuff(BSONObjBuilder* b) {
    b->append("_fts", INDEX_NAME);
    b->append("_ftsx", 1);
}
}  // namespace

const FTSLanguage& FTSSpec::_getLanguageToUseV1(const BSONObj& userDoc) const {
    BSONElement e = userDoc[_languageOverrideField];
    if (e.type() == BSONType::string) {
        StringData x = e.valueStringData();
        if (e.size() > 0) {
            // make() w/ TEXT_INDEX_VERSION_1 guaranteed to not fail.
            return FTSLanguage::make(x, TEXT_INDEX_VERSION_1);
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

        std::string term = str::toLower(t.data);
        if (tools.stopwords->isStopWord(term))
            continue;
        term = std::string{tools.stemmer->stem(term)};

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
        const std::string& term = i->first;
        const ScoreHelperStruct& data = i->second;

        // in order to adjust weights as a function of term count as it
        // relates to total field length. ie. is this the only word or
        // a frequently occuring term? or does it only show up once in
        // a long block of text?

        double coeff = (0.5 * data.count / numTokens) + 0.5;

        // if term is identical to the raw form of the
        // field (untokenized) give it a small boost.
        double adjustment = 1;
        if (str::equalCaseInsensitive(raw, term))
            adjustment += 0.1;

        double& score = (*docScores)[term];
        score += (weight * data.freq * coeff * adjustment);
        MONGO_verify(score <= MAX_WEIGHT);
    }
}

bool FTSSpec::_weightV1(StringData field, double* out) const {
    Weights::const_iterator i = _weights.find(std::string{field});
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

        if (x.type() == BSONType::string) {
            double w = 1;
            _weightV1(x.fieldName(), &w);
            _scoreStringV1(tools, x.valueStringData(), term_freqs, w);
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
        BSONElement e = ::mongo::bson::extractElementAtDottedPath(obj, leftOverName);
        // weight associated to name of field
        double weight = i->second;

        if (e.eoo()) {
            // do nothing
        } else if (e.type() == BSONType::array) {
            BSONObjIterator j(e.Obj());
            while (j.more()) {
                BSONElement x = j.next();
                if (leftOverName[0] && x.isABSONObj())
                    x = ::mongo::bson::extractElementAtDottedPath(x.Obj(), leftOverName);
                if (x.type() == BSONType::string)
                    _scoreStringV1(tools, x.valueStringData(), term_freqs, weight);
            }
        } else if (e.type() == BSONType::string) {
            _scoreStringV1(tools, e.valueStringData(), term_freqs, weight);
        }
    }
}

StatusWith<BSONObj> FTSSpec::_fixSpecV1(const BSONObj& spec) {
    std::map<std::string, int> m;

    BSONObj keyPattern;
    {
        BSONObjBuilder b;
        bool addedFtsStuff = false;

        BSONObjIterator i(spec["key"].Obj());
        while (i.more()) {
            BSONElement e = i.next();
            if ((e.fieldNameStringData() == "_fts") || (e.fieldNameStringData() == "_ftsx")) {
                addedFtsStuff = true;
                b.append(e);
            } else if (e.type() == BSONType::string &&
                       (e.valueStringData() == "fts" || e.valueStringData() == "text")) {
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
        for (const auto& kv : m) {
            if (kv.second <= 0 || kv.second >= MAX_WORD_WEIGHT) {
                return {ErrorCodes::CannotCreateIndex,
                        str::stream() << "text index weight must be in the exclusive interval (0,"
                                      << MAX_WORD_WEIGHT << ") but found: " << kv.second};
            }
            b.append(kv.first, kv.second);
        }
        weights = b.obj();
    }

    std::string default_language(spec.getStringField("default_language"));
    if (default_language.empty())
        default_language = "english";

    std::string language_override(spec.getStringField("language_override"));
    if (language_override.empty())
        language_override = "language";

    int version = -1;
    int textIndexVersion = 1;

    BSONObjBuilder b;
    BSONObjIterator i(spec);
    while (i.more()) {
        BSONElement e = i.next();
        StringData fieldName = e.fieldNameStringData();
        if (fieldName == "key") {
            b.append("key", keyPattern);
        } else if (fieldName == "weights") {
            b.append("weights", weights);
            weights = BSONObj();
        } else if (fieldName == "default_language") {
            b.append("default_language", default_language);
            default_language = "";
        } else if (fieldName == "language_override") {
            b.append("language_override", language_override);
            language_override = "";
        } else if (fieldName == "v") {
            version = e.numberInt();
        } else if (fieldName == "textIndexVersion") {
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
}  // namespace fts
}  // namespace mongo
