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

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/stats/ce_histogram.h"
#include "mongo/unittest/unittest.h"

#include <boost/move/utility_core.hpp>

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
