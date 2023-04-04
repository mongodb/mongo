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
#include "mongo/db/fle_crud.h"
#include "mongo/db/namespace_string.h"
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

void generateTags(uint64_t numInserts,
                  EDCDerivedFromDataTokenAndContentionFactorToken edcTok,
                  std::vector<PrfBlock>& binaryTags) {

    auto edcTag = TwiceDerived::generateEDCTwiceDerivedToken(edcTok);

    for (uint64_t i = 1; i <= numInserts; i++) {
        binaryTags.emplace_back(EDCServerCollection::generateTag(edcTag, i));
    }
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

// A positive contention factor (cm) means we must run the above algorithm (cm) times.
std::vector<PrfBlock> readTags(FLETagQueryInterface* queryImpl,
                               const NamespaceString& nssEsc,
                               ESCDerivedFromDataToken s,
                               EDCDerivedFromDataToken d,
                               boost::optional<int64_t> cm) {

    auto memoryLimit = static_cast<size_t>(internalQueryFLERewriteMemoryLimit.load());
    auto contentionMax = cm.value_or(0);
    std::vector<PrfBlock> binaryTags;

    std::vector<FLEEdgePrfBlock> blocks;
    blocks.reserve(contentionMax + 1);

    for (auto cf = 0; cf <= contentionMax; cf++) {
        auto escToken =
            DerivedToken::generateESCDerivedFromDataTokenAndContentionFactorToken(s, cf);
        auto edcToken =
            DerivedToken::generateEDCDerivedFromDataTokenAndContentionFactorToken(d, cf);

        FLEEdgePrfBlock edgeSet{escToken.data, edcToken.data};

        blocks.push_back(edgeSet);
    }

    std::vector<std::vector<FLEEdgePrfBlock>> blockSets;
    blockSets.push_back(blocks);

    auto countInfoSets =
        queryImpl->getTags(nssEsc, blockSets, FLETagQueryInterface::TagQueryType::kQuery);


    // Count how many tags we will need and check once if we they will fit
    //
    uint32_t totalTagCount = 0;

    for (const auto& countInfoSet : countInfoSets) {
        for (const auto& countInfo : countInfoSet) {
            totalTagCount += countInfo.count;
        }
    }

    verifyTagsWillFit(totalTagCount, memoryLimit);

    binaryTags.reserve(totalTagCount);

    for (const auto& countInfoSet : countInfoSets) {
        for (const auto& countInfo : countInfoSet) {

            uassert(7415001, "Missing EDC value", countInfo.edc.has_value());
            generateTags(countInfo.count, countInfo.edc.value(), binaryTags);
        }
    }

    return binaryTags;
}
}  // namespace mongo::fle
