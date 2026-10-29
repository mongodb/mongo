// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/stats/ce_histogram.h"
#include "mongo/unittest/unittest.h"


namespace mongo::stats {

using TypeTags = sbe::value::TypeTags;

TEST(TypeCountTest, HistogrammableCount) {
    const TypeCounts _empty = {};
    ASSERT_EQ(getTotalCount(_empty), 0);
    ASSERT_EQ(getTotalCount(_empty, true), 0);
    ASSERT_EQ(getTotalCount(_empty, false), 0);

    // Histogrammable types only.
    const TypeCounts _histogrammable = {
        {TypeTags::NumberDecimal, 10},
        {TypeTags::StringSmall, 20},
        {TypeTags::Timestamp, 30},
    };
    ASSERT_EQ(getTotalCount(_histogrammable), 60);
    ASSERT_EQ(getTotalCount(_histogrammable, true), 60);
    ASSERT_EQ(getTotalCount(_histogrammable, false), 0);

    // Non-histogrammable types only.
    const TypeCounts _nonHistogrammable = {
        {TypeTags::Boolean, 10},
        {TypeTags::Object, 20},
        {TypeTags::Array, 30},
    };
    ASSERT_EQ(getTotalCount(_nonHistogrammable), 60);
    ASSERT_EQ(getTotalCount(_nonHistogrammable, true), 0);
    ASSERT_EQ(getTotalCount(_nonHistogrammable, false), 60);

    // Mixed types.
    const TypeCounts _allTypes = {
        // Non-histogrammable types.
        {TypeTags::Boolean, 10},
        {TypeTags::Object, 20},
        {TypeTags::Array, 30},
        // Histogrammable types.
        {TypeTags::NumberInt32, 50},
        {TypeTags::StringBig, 20},
        {TypeTags::ObjectId, 20},
    };
    ASSERT_EQ(getTotalCount(_allTypes), 150);
    ASSERT_EQ(getTotalCount(_allTypes, true), 90);
    ASSERT_EQ(getTotalCount(_allTypes, false), 60);
}
}  // namespace mongo::stats
