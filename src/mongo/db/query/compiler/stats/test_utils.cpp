// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/stats/test_utils.h"

namespace mongo::stats {

bool sameTypeClassByComparingMin(sbe::value::TypeTags tag1, sbe::value::TypeTags tag2) {
    if (tag1 == tag2) {
        return true;
    }

    static constexpr const char* kTempFieldName = "temp";

    // As the type representations in SBEValues differ from those in BSONObj and they are not a
    // one-to-one match, the implementation ensures correctness by converting to BSONType with
    // tagToType() and then comparing with the values from BSONObjBuilder::appendMinForType.
    BSONObjBuilder minb1;
    minb1.appendMinForType(kTempFieldName, stdx::to_underlying(sbe::value::tagToType(tag1)));
    const BSONObj min1 = minb1.obj();

    BSONObjBuilder minb2;
    minb2.appendMinForType(kTempFieldName, stdx::to_underlying(sbe::value::tagToType(tag2)));
    const BSONObj min2 = minb2.obj();

    return min1.woCompare(min2) == 0;
};

TypeTagPairs generateTypeTagPairs(size_t start, size_t end) {
    invariant(end > start);

    std::vector<std::pair<sbe::value::TypeTags, sbe::value::TypeTags>> allTypeTagPairs(
        (end - start) * (end - start));

    for (size_t first = start; first < end; ++first) {
        for (size_t second = start; second < end; ++second) {
            auto firstTag = static_cast<sbe::value::TypeTags>(first);
            auto secondTag = static_cast<sbe::value::TypeTags>(second);
            if (tagToType(firstTag) == BSONType::eoo || tagToType(secondTag) == BSONType::eoo) {
                continue;
            }
            allTypeTagPairs.emplace_back(firstTag, secondTag);
        }
    }
    return allTypeTagPairs;
}

TypeTagPairs generateAllTypeTagPairs() {
    return generateTypeTagPairs(0, size_t(sbe::value::TypeTags::TypeTagsMax));
}

TypeTagPairs generateShallowTypeTagPairs() {
    return generateTypeTagPairs(0, size_t(sbe::value::TypeTags::EndOfShallowTypeTags) + 1);
}

TypeTagPairs generateHeapTypeTagPairs() {
    return generateTypeTagPairs(size_t(sbe::value::TypeTags::EndOfShallowTypeTags) + 1,
                                size_t(sbe::value::TypeTags::TypeTagsMax));
}

}  // namespace mongo::stats
