// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/crypto/fle_tags.h"

#include "mongo/base/error_codes.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <cstddef>
#include <limits>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::fle {

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

    auto edcTag = EDCTwiceDerivedToken::deriveFrom(edcTok);

    HmacContext hmacCtx;
    hmacCtx.setReuseKey(true);
    for (uint64_t i = 1; i <= numInserts; i++) {
        binaryTags.emplace_back(EDCServerCollection::generateTag(&hmacCtx, edcTag, i));
    }
    hmacCtx.setReuseKey(false);
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

std::vector<std::vector<FLEEdgeCountInfo>> getCountInfoSets(FLETagQueryInterface* queryImpl,
                                                            const NamespaceString& nssEsc,
                                                            ESCDerivedFromDataToken s,
                                                            EDCDerivedFromDataToken d,
                                                            boost::optional<int64_t> cm) {
    auto contentionMax = cm.value_or(0);

    std::vector<FLEEdgePrfBlock> blocks;
    blocks.reserve(contentionMax + 1);

    for (auto cf = 0; cf <= contentionMax; cf++) {
        auto escToken = ESCDerivedFromDataTokenAndContentionFactorToken::deriveFrom(s, cf);
        auto edcToken = EDCDerivedFromDataTokenAndContentionFactorToken::deriveFrom(d, cf);

        FLEEdgePrfBlock edgeSet{escToken.asPrfBlock(), edcToken.asPrfBlock()};

        blocks.push_back(edgeSet);
    }

    std::vector<std::vector<FLEEdgePrfBlock>> blockSets;
    blockSets.push_back(blocks);

    return queryImpl->getTags(nssEsc, blockSets, FLETagQueryInterface::TagQueryType::kQuery);
}


// A positive contention factor (cm) means we must run the above algorithm (cm) times.
std::vector<PrfBlock> readTags(FLETagQueryInterface* queryImpl,
                               const NamespaceString& nssEsc,
                               ESCDerivedFromDataToken s,
                               EDCDerivedFromDataToken d,
                               boost::optional<int64_t> cm) {

    auto memoryLimit = static_cast<size_t>(internalQueryFLERewriteMemoryLimit.load());
    std::vector<PrfBlock> binaryTags;

    auto countInfoSets = getCountInfoSets(queryImpl, nssEsc, s, d, cm);

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
