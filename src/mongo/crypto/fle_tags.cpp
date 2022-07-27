/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <boost/optional.hpp>

#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/fle_tags.h"
#include "mongo/db/query/query_knobs_gen.h"

namespace mongo::fle {

using DerivedToken = FLEDerivedFromDataTokenAndContentionFactorTokenGenerator;
using TwiceDerived = FLETwiceDerivedTokenGenerator;

size_t sizeArrayElementsMemory(size_t tagCount);

namespace {

inline constexpr size_t arrayElementSize(int digits) {
    constexpr size_t sizeOfType = 1;
    constexpr size_t sizeOfBinDataLength = 4;
    constexpr size_t sizeOfNullMarker = 1;
    constexpr size_t sizeOfSubType = 1;
    constexpr size_t sizeOfData = sizeof(PrfBlock);
    return sizeOfType + sizeOfBinDataLength + sizeOfNullMarker + digits + sizeOfSubType +
        sizeOfData;
}

void verifyTagsWillFit(size_t tagCount, size_t memoryLimit) {
    constexpr size_t largestElementSize = arrayElementSize(std::numeric_limits<size_t>::digits10);
    constexpr size_t ridiculousNumberOfTags =
        std::numeric_limits<size_t>::max() / largestElementSize;

    uassert(ErrorCodes::FLEMaxTagLimitExceeded,
            "Encrypted rewrite too many tags",
            tagCount < ridiculousNumberOfTags);
    uassert(ErrorCodes::FLEMaxTagLimitExceeded,
            "Encrypted rewrite memory limit exceeded",
            sizeArrayElementsMemory(tagCount) <= memoryLimit);
}

}  // namespace

size_t sizeArrayElementsMemory(size_t tagCount) {
    size_t size = 0;
    size_t power = 1;
    size_t digits = 1;
    size_t accountedTags = 0;
    while (tagCount >= power) {
        power *= 10;
        size_t count = std::min(tagCount, power) - accountedTags;
        size += arrayElementSize(digits) * count;
        accountedTags += count;
        digits++;
    }
    return size;
}


// The algorithm for constructing a list of tags matching an equality predicate on an encrypted
// field is as follows:
//
// (1) Query ESC to obtain the counter value (n) after the most recent insert.
// (2) Query ECC for a null document.
//      (a) A null document means there has been at least one compaction of ECC.
//      (b) No null document means there has not been a compaction. Therefore we have to check all
//          the tags from 1..n to see if they have been deleted.
// (3) Return the surviving set of tags from 1..n (encrypted).
//
// Given:
//      s: ESCDerivedFromDataToken
//      c: ECCDerivedFromDataToken
//      d: EDCDerivedFromDataToken
// Do:
//      n = ESC::emuBinary(s)
//      deletedTags = []
//      pos = ECC::nullDocument(c) ? ECC::nullDocument(c).position : 1
//      while true {
//          if (doc = ECC::getDocument(c, pos); doc != null) {
//              deletedTags = [doc | deletedTags]
//          } else {
//              break
//          }
//          pos++
//      }
//      return [EDC::encrypt(i) | i in [1..n] where i not in deletedTags]
std::vector<PrfBlock> readTagsWithContention(const FLEStateCollectionReader& esc,
                                             const FLEStateCollectionReader& ecc,
                                             ESCDerivedFromDataToken s,
                                             ECCDerivedFromDataToken c,
                                             EDCDerivedFromDataToken d,
                                             uint64_t cf,
                                             size_t memoryLimit,
                                             std::vector<PrfBlock>&& binaryTags) {

    auto escTok = DerivedToken::generateESCDerivedFromDataTokenAndContentionFactorToken(s, cf);
    auto escTag = TwiceDerived::generateESCTwiceDerivedTagToken(escTok);
    auto escVal = TwiceDerived::generateESCTwiceDerivedValueToken(escTok);

    auto eccTok = DerivedToken::generateECCDerivedFromDataTokenAndContentionFactorToken(c, cf);
    auto eccTag = TwiceDerived::generateECCTwiceDerivedTagToken(eccTok);
    auto eccVal = TwiceDerived::generateECCTwiceDerivedValueToken(eccTok);

    auto edcTok = DerivedToken::generateEDCDerivedFromDataTokenAndContentionFactorToken(d, cf);
    auto edcTag = TwiceDerived::generateEDCTwiceDerivedToken(edcTok);

    // (1) Query ESC for the counter value after the most recent insert.
    //     0 => 0 inserts for this field value pair.
    //     n => n inserts for this field value pair.
    //     none => compaction => query ESC for null document to find # of inserts.
    auto insertCounter = ESCCollection::emuBinary(esc, escTag, escVal);
    if (insertCounter && insertCounter.value() == 0) {
        return std::move(binaryTags);
    }

    auto numInserts = insertCounter
        ? uassertStatusOK(
              ESCCollection::decryptDocument(
                  escVal, esc.getById(ESCCollection::generateId(escTag, insertCounter))))
              .count
        : uassertStatusOK(ESCCollection::decryptNullDocument(
                              escVal, esc.getById(ESCCollection::generateId(escTag, boost::none))))
              .count;

    // (2) Query ECC for a null document.
    auto eccNullDoc = ecc.getById(ECCCollection::generateId(eccTag, boost::none));
    auto pos = eccNullDoc.isEmpty()
        ? 1
        : uassertStatusOK(ECCCollection::decryptNullDocument(eccVal, eccNullDoc)).position + 2;

    std::vector<ECCDocument> deletes;

    // (2) Search ECC for deleted tag(counter) values.
    while (true) {
        auto eccObj = ecc.getById(ECCCollection::generateId(eccTag, pos));
        if (eccObj.isEmpty()) {
            break;
        }
        auto eccDoc = uassertStatusOK(ECCCollection::decryptDocument(eccVal, eccObj));
        // Compaction placeholders only present for positive contention factors (cm).
        if (eccDoc.valueType == ECCValueType::kCompactionPlaceholder) {
            break;
        }
        // Note, in the worst case where no compactions have occurred, the deletes vector will grow
        // proportionally to the number of deletes. This is not likely to present a significant
        // problem but we should still track the memory we consume here.
        deletes.emplace_back(eccDoc);
        pos++;
    }

    std::sort(deletes.begin(), deletes.end());

    auto numDeletes = std::accumulate(deletes.begin(), deletes.end(), 0, [](auto acc, auto eccDoc) {
        return acc + eccDoc.end - eccDoc.start + 1;
    });
    auto cumTagCount = binaryTags.size() + numInserts - numDeletes;

    verifyTagsWillFit(cumTagCount, memoryLimit);

    for (uint64_t i = 1; i <= numInserts; i++) {
        if (auto it = std::lower_bound(
                deletes.begin(),
                deletes.end(),
                i,
                [](ECCDocument& eccDoc, uint64_t tag) { return eccDoc.end < tag; });
            it != deletes.end() && it->start <= i && i <= it->end) {
            continue;
        }
        // (3) Return the surviving set of tags from 1..n (encrypted).
        binaryTags.emplace_back(EDCServerCollection::generateTag(edcTag, i));
    }
    return std::move(binaryTags);
}

// A positive contention factor (cm) means we must run the above algorithm (cm) times.
std::vector<PrfBlock> readTags(const FLEStateCollectionReader& esc,
                               const FLEStateCollectionReader& ecc,
                               ESCDerivedFromDataToken s,
                               ECCDerivedFromDataToken c,
                               EDCDerivedFromDataToken d,
                               boost::optional<int64_t> cm) {
    // The output of readTags will be used as the argument to a $in expression, so make sure we
    // don't exceed the configured memory limit.
    auto limit = static_cast<size_t>(internalQueryFLERewriteMemoryLimit.load());
    if (!cm || cm.value() == 0) {
        auto binaryTags = readTagsWithContention(esc, ecc, s, c, d, 0, limit, {});
    }
    std::vector<PrfBlock> binaryTags;
    for (auto i = 0; i <= cm.value(); i++) {
        binaryTags = readTagsWithContention(esc, ecc, s, c, d, i, limit, std::move(binaryTags));
    }
    return binaryTags;
}
}  // namespace mongo::fle
