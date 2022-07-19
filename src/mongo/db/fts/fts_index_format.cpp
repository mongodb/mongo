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
    MultikeyComponents arrayComponents;
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
}

void FTSIndexFormat::getKeys(SharedBufferFragmentBuilder& pooledBufferBuilder,
                             const FTSSpec& spec,
                             const BSONObj& obj,
                             KeyStringSet* keys,
                             KeyString::Version keyStringVersion,
                             Ordering ordering,
                             const boost::optional<RecordId>& id) {
    vector<BSONElement> extrasBefore;
    vector<BSONElement> extrasAfter;

    // Compute the non FTS key elements for the prefix.
    for (unsigned i = 0; i < spec.numExtraBefore(); i++) {
        auto indexedElement = extractNonFTSKeyElement(obj, spec.extraBefore(i));
        extrasBefore.push_back(indexedElement);
    }

    // Compute the non FTS key elements for the suffix.
    for (unsigned i = 0; i < spec.numExtraAfter(); i++) {
        auto indexedElement = extractNonFTSKeyElement(obj, spec.extraAfter(i));
        extrasAfter.push_back(indexedElement);
    }

    TermFrequencyMap term_freqs;
    spec.scoreDocument(obj, &term_freqs);

    auto sequence = keys->extract_sequence();
    for (TermFrequencyMap::const_iterator i = term_freqs.begin(); i != term_freqs.end(); ++i) {
        const string& term = i->first;
        double weight = i->second;

        KeyString::PooledBuilder keyString(pooledBufferBuilder, keyStringVersion, ordering);
        for (const auto& elem : extrasBefore) {
            keyString.appendBSONElement(elem);
        }
        _appendIndexKey(keyString, weight, term, spec.getTextIndexVersion());
        for (const auto& elem : extrasAfter) {
            keyString.appendBSONElement(elem);
        }

        if (id) {
            keyString.appendRecordId(*id);
        }

        sequence.push_back(keyString.release());
    }
    keys->adopt_sequence(std::move(sequence));
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

    KeyString::Builder keyString(KeyString::Version::kLatestVersion, KeyString::ALL_ASCENDING);
    _appendIndexKey(keyString, weight, term, textIndexVersion);
    auto key = KeyString::toBson(keyString, KeyString::ALL_ASCENDING);

    return b.appendElements(key).obj();
}

template <typename KeyStringBuilder>
void FTSIndexFormat::_appendIndexKey(KeyStringBuilder& keyString,
                                     double weight,
                                     const string& term,
                                     TextIndexVersion textIndexVersion) {
    invariant(weight >= 0 && weight <= MAX_WEIGHT);  // FTSmaxweight =  defined in fts_header
    // Terms are added to index key verbatim.
    if (TEXT_INDEX_VERSION_1 == textIndexVersion) {
        keyString.appendString(term);
    }
    // See comments at the top of file for termKeyPrefixLengthV2.
    // Apply hash for text index version 2 to long terms (longer than 32 characters).
    else if (TEXT_INDEX_VERSION_2 == textIndexVersion) {
        if (term.size() <= termKeyPrefixLengthV2) {
            keyString.appendString(term);
        } else {
            union {
                uint64_t hash[2];
                char data[16];
            } t;
            uint32_t seed = 0;
            MurmurHash3_x64_128(term.data(), term.size(), seed, t.hash);
            string keySuffix = hexblob::encodeLower(t.data, sizeof(t.data));
            invariant(termKeySuffixLengthV2 == keySuffix.size());
            keyString.appendString(term.substr(0, termKeyPrefixLengthV2) + keySuffix);
        }
    } else {
        invariant(TEXT_INDEX_VERSION_3 == textIndexVersion);
        if (term.size() <= termKeyPrefixLengthV3) {
            keyString.appendString(term);
        } else {
            string keySuffix = md5simpledigest(term);
            invariant(termKeySuffixLengthV3 == keySuffix.size());
            keyString.appendString(term.substr(0, termKeyPrefixLengthV3) + keySuffix);
        }
    }
    keyString.appendNumberDouble(weight);
}
}  // namespace fts
}  // namespace mongo
