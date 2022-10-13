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

#include "mongo/logv2/log.h"
#include "mongo/s/catalog_cache_test_fixture.h"
#include "mongo/s/write_ops/write_without_shard_key_util.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace write_without_shard_key {
namespace {

const NamespaceString kNss("TestDB", "TestColl");
const int splitPoint = 50;

class WriteWithoutShardKeyUtilTest : public CatalogCacheTestFixture {
public:
    void setUp() override {
        CatalogCacheTestFixture::setUp();

        // Shard key is a compound shard key: {a:1, b:1}.
        const ShardKeyPattern shardKeyPattern(BSON("a" << 1 << "b" << 1));
        _cm = makeChunkManager(
            kNss, shardKeyPattern, nullptr, false, {BSON("a" << splitPoint << "b" << splitPoint)});
    }

    ChunkManager getChunkManager() const {
        return *_cm;
    }

    OperationContext* getOpCtx() {
        return operationContext();
    }

private:
    boost::optional<ChunkManager> _cm;
};

TEST_F(WriteWithoutShardKeyUtilTest, WriteQueryContainingFullShardKeyCanTargetSingleDocument) {
    auto hasTargetingInfo = write_without_shard_key::canTargetQueryByShardKeyOrId(
        getOpCtx(), kNss, BSON("a" << 1 << "b" << 1));
    ASSERT_EQ(hasTargetingInfo, true);
}

TEST_F(WriteWithoutShardKeyUtilTest,
       WriteQueryContainingPartialShardKeyCannotTargetSingleDocument) {
    auto hasTargetingInfo =
        write_without_shard_key::canTargetQueryByShardKeyOrId(getOpCtx(), kNss, BSON("a" << 1));
    ASSERT_EQ(hasTargetingInfo, false);
}

TEST_F(WriteWithoutShardKeyUtilTest, WriteQueryContainingUnderscoreIdCanTargetSingleDocument) {
    auto hasTargetingInfo =
        write_without_shard_key::canTargetQueryByShardKeyOrId(getOpCtx(), kNss, BSON("_id" << 1));
    ASSERT_EQ(hasTargetingInfo, true);
}

TEST_F(WriteWithoutShardKeyUtilTest,
       WriteQueryWithoutShardKeyOrUnderscoreIdCannotTargetSingleDocument) {
    auto hasTargetingInfo =
        write_without_shard_key::canTargetQueryByShardKeyOrId(getOpCtx(), kNss, BSON("x" << 1));
    ASSERT_EQ(hasTargetingInfo, false);
}

}  // namespace
}  // namespace write_without_shard_key
}  // namespace mongo
