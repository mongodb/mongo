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

#include "mongo/platform/basic.h"

#include <third_party/murmurhash3/MurmurHash3.h>

#include "mongo/base/init.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/fts/fts_index_format.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/server_options.h"
#include "mongo/util/hex.h"
#include "mongo/util/md5.hpp"
#include "mongo/util/str.h"

namespace mongo {

namespace fts {

using std::string;
using std::vector;

namespace dps = ::mongo::dotted_path_support;

namespace {
BSONObj nullObj;
BSONElement nullElt;

// New in textIndexVersion 2.
// If the term is longer than 32 characters, it may
// result in the generated key being too large
// for the index. In that case, we generate a 64-character key
// from the concatenation of the first 32 characters
// and the hex string of the murmur3 hash value of the entire
// term value.
const size_t termKeyPrefixLengthV2 = 32U;
// 128-bit hash value expressed in hex = 32 characters
const size_t termKeySuffixLengthV2 = 32U;
const size_t termKeyLengthV2 = termKeyPrefixLengthV2 + termKeySuffixLengthV2;

// TextIndexVersion 3.
// If the term is longer than 256 characters, it may
// result in the generated key being too large
// for the index. In that case, we generate a 256-character key
// from the concatenation of the first 224 characters
// and the hex string of the md5 hash value of the entire
// term value.
const size_t termKeyPrefixLengthV3 = 224U;
// 128-bit hash value expressed in hex = 32 characters
const size_t termKeySuffixLengthV3 = 32U;
const size_t termKeyLengthV3 = termKeyPrefixLengthV3 + termKeySuffixLengthV3;

/**
 * Returns size of buffer required to store term in index key.
 * In version 1, terms are stored verbatim in key.
 * In version 2 and above, terms longer than 32 characters are hashed and combined
 * with a prefix.
 */
int guessTermSize(const std::string& term, TextIndexVersion textIndexVersion) {
    if (TEXT_INDEX_VERSION_1 == textIndexVersion) {
        return term.size();
    } else if (TEXT_INDEX_VERSION_2 == textIndexVersion) {
        if (term.size() <= termKeyPrefixLengthV2) {
            return term.size();
        }

        return termKeyLengthV2;
    } else {
        invariant(TEXT_INDEX_VERSION_3 == textIndexVersion);
        if (term.size() <= termKeyPrefixLengthV3) {
            return term.size();
        }

        return termKeyLengthV3;
    }
}

/**
 * Given an object being indexed, 'obj', and a path through 'obj', returns the corresponding BSON
 * element, according to the indexing rules for the non-text fields of an FTS index key pattern.
 *
 * Specifically, throws a user assertion if an array is encountered while traversing the 'path'. It
 * is not legal for there to be an array along the path of the non-text prefix or suffix fields of a
 * text index, unless a particular array index is specified, as in "a.3".
 */
BSONElement extractNonFTSKeyElement(const BSONObj& obj, StringData path) {
    BSONElementSet indexedElements;
    const bool expandArrayOnTrailingField = true;
    std::set<size_t> arrayComponents;
    dps::extractAllElementsAlongPath(
        obj, path, indexedElements, expandArrayOnTrailingField, &arrayComponents);
    uassert(ErrorCodes::CannotBuildIndexKeys,
            str::stream() << "Field '" << path
                          << "' of text index contains an array in document: " << obj,
            arrayComponents.empty());

    // Since there aren't any arrays, there cannot be more than one extracted element on 'path'.
    invariant(indexedElements.size() <= 1U);
    return indexedElements.empty() ? nullElt : *indexedElements.begin();
}
}  // namespace

MONGO_INITIALIZER(FTSIndexFormat)(InitializerContext* context) {
    BSONObjBuilder b;
    b.appendNull("");
    nullObj = b.obj();
    nullElt = nullObj.firstElement();
    return Status::OK();
}

void FTSIndexFormat::getKeys(const FTSSpec& spec,
                             const BSONObj& obj,
                             KeyStringSet* keys,
                             KeyString::Version keyStringVersion,
                             Ordering ordering,
                             boost::optional<RecordId> id) {
    int extraSize = 0;
    vector<BSONElement> extrasBefore;
    vector<BSONElement> extrasAfter;

    // Compute the non FTS key elements for the prefix.
    for (unsigned i = 0; i < spec.numExtraBefore(); i++) {
        auto indexedElement = extractNonFTSKeyElement(obj, spec.extraBefore(i));
        extrasBefore.push_back(indexedElement);
        extraSize += indexedElement.size();
    }

    // Compute the non FTS key elements for the suffix.
    for (unsigned i = 0; i < spec.numExtraAfter(); i++) {
        auto indexedElement = extractNonFTSKeyElement(obj, spec.extraAfter(i));
        extrasAfter.push_back(indexedElement);
        extraSize += indexedElement.size();
    }

    TermFrequencyMap term_freqs;
    spec.scoreDocument(obj, &term_freqs);

    // create index keys from raw scores
    // only 1 per string
    long long keyBSONSize = 0;

    for (TermFrequencyMap::const_iterator i = term_freqs.begin(); i != term_freqs.end(); ++i) {
        const string& term = i->first;
        double weight = i->second;

        // guess the total size of the btree entry based on the size of the weight, term tuple
        int guess = 5 /* bson overhead */ + 10 /* weight */ + 8 /* term overhead */ +
            /* term size (could be truncated/hashed) */
            guessTermSize(term, spec.getTextIndexVersion()) + extraSize;

        BSONObjBuilder b(guess);  // builds a BSON object with guess length.
        for (unsigned k = 0; k < extrasBefore.size(); k++) {
            b.appendAs(extrasBefore[k], "");
        }
        _appendIndexKey(b, weight, term, spec.getTextIndexVersion());
        for (unsigned k = 0; k < extrasAfter.size(); k++) {
            b.appendAs(extrasAfter[k], "");
        }
        BSONObj res = b.obj();

        verify(guess >= res.objsize());

        KeyString::HeapBuilder keyString(keyStringVersion, res, ordering);
        if (id) {
            keyString.appendRecordId(*id);
        }
        keys->insert(keyString.release());
        keyBSONSize += res.objsize();
    }
}

BSONObj FTSIndexFormat::getIndexKey(double weight,
                                    const string& term,
                                    const BSONObj& indexPrefix,
                                    TextIndexVersion textIndexVersion) {
    BSONObjBuilder b;

    BSONObjIterator i(indexPrefix);
    while (i.more()) {
        b.appendAs(i.next(), "");
    }

    _appendIndexKey(b, weight, term, textIndexVersion);
    return b.obj();
}

void FTSIndexFormat::_appendIndexKey(BSONObjBuilder& b,
                                     double weight,
                                     const string& term,
                                     TextIndexVersion textIndexVersion) {
    verify(weight >= 0 && weight <= MAX_WEIGHT);  // FTSmaxweight =  defined in fts_header
    // Terms are added to index key verbatim.
    if (TEXT_INDEX_VERSION_1 == textIndexVersion) {
        b.append("", term);
        b.append("", weight);
    }
    // See comments at the top of file for termKeyPrefixLengthV2.
    // Apply hash for text index version 2 to long terms (longer than 32 characters).
    else if (TEXT_INDEX_VERSION_2 == textIndexVersion) {
        if (term.size() <= termKeyPrefixLengthV2) {
            b.append("", term);
        } else {
            union {
                uint64_t hash[2];
                char data[16];
            } t;
            uint32_t seed = 0;
            MurmurHash3_x64_128(term.data(), term.size(), seed, t.hash);
            string keySuffix = mongo::toHexLower(t.data, sizeof(t.data));
            invariant(termKeySuffixLengthV2 == keySuffix.size());
            b.append("", term.substr(0, termKeyPrefixLengthV2) + keySuffix);
        }
        b.append("", weight);
    } else {
        invariant(TEXT_INDEX_VERSION_3 == textIndexVersion);
        if (term.size() <= termKeyPrefixLengthV3) {
            b.append("", term);
        } else {
            string keySuffix = md5simpledigest(term);
            invariant(termKeySuffixLengthV3 == keySuffix.size());
            b.append("", term.substr(0, termKeyPrefixLengthV3) + keySuffix);
        }
        b.append("", weight);
    }
}
}  // namespace fts
}  // namespace mongo
