/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/sbe/sbe_unittest.h"
#include "mongo/db/exec/sbe/values/ts_block.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/sbe_stage_builder_test_fixture.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo::sbe {

class SbeValueTest : public SbeStageBuilderTestFixture {};

// This is just a made up example, and is not actually a valid bucket. There's no min/max and
// no time field.
const BSONObj kSampleBucket = fromjson(R"(
{
    "_id" : ObjectId("649f0704230f18da067519c4"),
    "control" : {"version" : 1},
    "meta" : "A",
    "data" : {
        "_id" : {"0" : 0}
    }
})");

TEST_F(SbeValueTest, CloneCreatesIndependentCopy) {
    // A TsCellBlock can be created in an "unowned" state.
    auto cellBlock = std::make_unique<value::TsCellBlock>(
        1,     /* count */
        false, /* owned */
        value::TypeTags::bsonObject,
        value::bitcastFrom<const char*>(kSampleBucket["data"]["_id"].embeddedObject().objdata()));

    auto& valBlock = cellBlock->getValueBlock();

    const auto expectedTagVal = bson::convertFrom<true>(kSampleBucket["data"]["_id"]["0"]);


    // And we can read its values.
    {
        auto extractedVals = valBlock.extract();
        ASSERT_EQ(extractedVals.count, 1) << "Expected only one value";
        ASSERT_THAT(std::make_pair(extractedVals.tags[0], extractedVals.vals[0]),
                    ValueEq(expectedTagVal))
            << "Expected value extracted from block to be same as original";
    }

    // And we can clone the CellBlock.
    auto cellBlockClone = cellBlock->clone();
    auto valBlockClone = valBlock.clone();

    // And if we destroy the originals, we should still be able to read the clones.
    cellBlock.reset();
    // 'valBlock' is now invalid.

    // If we get the values from the cloned CellBlock, we should see the same data.
    {
        auto& valsFromClonedCellBlock = cellBlockClone->getValueBlock();
        auto extractedVals = valsFromClonedCellBlock.extract();
        ASSERT_THAT(std::make_pair(extractedVals.tags[0], extractedVals.vals[0]),
                    ValueEq(expectedTagVal))
            << "Expected value extracted from cloned CellBlock to be same as original";
    }

    // If we extract the values from the cloned ValueBlock, we should see the same data.
    {
        auto extractedVals = valBlockClone->extract();
        ASSERT_THAT(std::make_pair(extractedVals.tags[0], extractedVals.vals[0]),
                    ValueEq(expectedTagVal))
            << "Expected value from cloned ValueBlock to be same as original";
    }
}
}  // namespace mongo::sbe
