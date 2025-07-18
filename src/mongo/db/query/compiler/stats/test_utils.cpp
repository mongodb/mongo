/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
