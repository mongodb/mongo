/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/index_builds/resumable_index_builds_common.h"

#include "mongo/db/index_builds/resumable_index_builds_gen.h"
#include "mongo/db/record_id.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

IndexStateInfo makeIndexStateInfo(boost::optional<RecordId> lastSpilled) {
    IndexStateInfo info;
    if (lastSpilled) {
        info.setLastSpilledRecordId(*lastSpilled);
    }
    return info;
}

TEST(minLastSpilledRecordIdTest, EmptyVectorReturnsNone) {
    std::vector<IndexStateInfo> indexes;
    EXPECT_EQ(index_builds::minLastSpilledRecordId(indexes), boost::none);
}

TEST(minLastSpilledRecordIdTest, SingleIndexMissingReturnsNone) {
    std::vector<IndexStateInfo> indexes{makeIndexStateInfo(boost::none)};
    EXPECT_EQ(index_builds::minLastSpilledRecordId(indexes), boost::none);
}

TEST(minLastSpilledRecordIdTest, SingleIndexPresentReturnsItsValue) {
    std::vector<IndexStateInfo> indexes{makeIndexStateInfo(RecordId{42})};
    EXPECT_EQ(index_builds::minLastSpilledRecordId(indexes), RecordId{42});
}

TEST(minLastSpilledRecordIdTest, MultipleIndexesAllPresentReturnsMinimum) {
    std::vector<IndexStateInfo> indexes{makeIndexStateInfo(RecordId{20}),
                                        makeIndexStateInfo(RecordId{5}),
                                        makeIndexStateInfo(RecordId{12})};
    EXPECT_EQ(index_builds::minLastSpilledRecordId(indexes), RecordId{5});
}

TEST(minLastSpilledRecordIdTest, DuplicateMinimaReturnsSharedMin) {
    std::vector<IndexStateInfo> indexes{makeIndexStateInfo(RecordId{7}),
                                        makeIndexStateInfo(RecordId{20}),
                                        makeIndexStateInfo(RecordId{7})};
    EXPECT_EQ(index_builds::minLastSpilledRecordId(indexes), RecordId{7});
}

TEST(minLastSpilledRecordIdTest, FirstIndexMissingReturnsNone) {
    std::vector<IndexStateInfo> indexes{makeIndexStateInfo(boost::none),
                                        makeIndexStateInfo(RecordId{5}),
                                        makeIndexStateInfo(RecordId{12})};
    EXPECT_EQ(index_builds::minLastSpilledRecordId(indexes), boost::none);
}

TEST(minLastSpilledRecordIdTest, MiddleIndexMissingReturnsNone) {
    std::vector<IndexStateInfo> indexes{makeIndexStateInfo(RecordId{20}),
                                        makeIndexStateInfo(boost::none),
                                        makeIndexStateInfo(RecordId{12})};
    EXPECT_EQ(index_builds::minLastSpilledRecordId(indexes), boost::none);
}

TEST(minLastSpilledRecordIdTest, LastIndexMissingReturnsNone) {
    std::vector<IndexStateInfo> indexes{makeIndexStateInfo(RecordId{20}),
                                        makeIndexStateInfo(RecordId{5}),
                                        makeIndexStateInfo(boost::none)};
    EXPECT_EQ(index_builds::minLastSpilledRecordId(indexes), boost::none);
}

}  // namespace
}  // namespace mongo
