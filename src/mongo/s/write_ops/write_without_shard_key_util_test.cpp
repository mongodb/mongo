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

#include "mongo/s/write_ops/write_without_shard_key_util.h"

#include "mongo/bson/json.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache_test_fixture.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace write_without_shard_key {
namespace {

class WriteWithoutShardKeyUtilTest : public RouterCatalogCacheTestFixture {
public:
    void setUp() override {
        RouterCatalogCacheTestFixture::setUp();

        _cm = _makeChunkManager();
    }

    NamespaceString ns() const {
        return NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
    }

    ChunkManager getChunkManager() const {
        return *_cm;
    }

    OperationContext* getOpCtx() {
        return operationContext();
    }

private:
    virtual ChunkManager _makeChunkManager() {
        return makeCollectionRoutingInfo(ns(),
                                         ShardKeyPattern{BSON("a" << 1 << "b" << 1)},
                                         nullptr,
                                         false,
                                         {BSON("a" << 50 << "b" << 50)},
                                         {})
            .getChunkManager();
    }

    boost::optional<ChunkManager> _cm;
};

class TimeseriesWriteWithoutShardKeyUtilTest : public WriteWithoutShardKeyUtilTest {
public:
    NamespaceString ns() const {
        return WriteWithoutShardKeyUtilTest::ns().makeTimeseriesBucketsNamespace();
    }

private:
    ChunkManager _makeChunkManager() override {
        TimeseriesOptions timeseriesOptions;
        timeseriesOptions.setTimeField("t"_sd);
        timeseriesOptions.setMetaField("m"_sd);

        TypeCollectionTimeseriesFields timeseriesFields;
        timeseriesFields.setTimeseriesOptions(timeseriesOptions);

        return makeCollectionRoutingInfo(ns(),
                                         ShardKeyPattern{BSON("meta.a" << 1 << "meta.b" << 1)},
                                         nullptr,
                                         false,
                                         {BSON("meta.a" << 50 << "meta.b" << 50)},
                                         boost::none,
                                         std::move(timeseriesFields))
            .getChunkManager();
    }
};

class UnshardedCollectionTest : public RouterCatalogCacheTestFixture {
protected:
    void setUp() override {
        RouterCatalogCacheTestFixture::setUp();
        setupNShards(2);
    }

    virtual NamespaceString ns() const {
        return NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
    }

    OperationContext* getOpCtx() {
        return operationContext();
    }
};

class TimeseriesUnshardedCollectionTest : public UnshardedCollectionTest {
protected:
    NamespaceString ns() const override {
        return UnshardedCollectionTest::ns().makeTimeseriesBucketsNamespace();
    }
};

class ProduceUpsertDocumentTest : public ServiceContextTest {
protected:
    ProduceUpsertDocumentTest() = default;

    NamespaceString ns() const {
        return NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
    }

    OperationContext* getOpCtx() const {
        return _opCtx.get();
    }

    ServiceContext::UniqueOperationContext _opCtx{makeOperationContext()};

private:
    NamespaceString _ns = NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
};

TEST_F(WriteWithoutShardKeyUtilTest, WriteQueryContainingFullShardKeyCanTargetSingleDocument) {
    auto useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     ns(),
                                                     true /* isUpdateOrDelete */,
                                                     false /* isUpsert */,
                                                     BSON("a" << 1 << "b" << 1),
                                                     {} /* collation */,
                                                     boost::none /* let */,
                                                     boost::none /* legacyRuntimeConstants */,
                                                     false /* isTimeseriesViewRequest */);
    ASSERT_EQ(useTwoPhaseProtocol, false);

    useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     ns(),
                                                     false /* isUpdateOrDelete */,
                                                     false /* isUpsert */,
                                                     BSON("a" << 1 << "b" << 1),
                                                     {} /* collation */,
                                                     boost::none /* let */,
                                                     boost::none /* legacyRuntimeConstants */,
                                                     false /* isTimeseriesViewRequest */);
    ASSERT_EQ(useTwoPhaseProtocol, false);
}

TEST_F(TimeseriesWriteWithoutShardKeyUtilTest,
       WriteQueryContainingFullShardKeyCanTargetSingleDocument) {
    auto useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     ns(),
                                                     true /* isUpdateOrDelete */,
                                                     false /* isUpsert */,
                                                     BSON("m.a" << 1 << "m.b" << 1),
                                                     {} /* collation */,
                                                     boost::none /* let */,
                                                     boost::none /* legacyRuntimeConstants */,
                                                     true /* isTimeseriesViewRequest */);
    ASSERT_EQ(useTwoPhaseProtocol, false);

    useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     ns(),
                                                     false /* isUpdateOrDelete */,
                                                     false /* isUpsert */,
                                                     BSON("m.a" << 1 << "m.b" << 1),
                                                     {} /* collation */,
                                                     boost::none /* let */,
                                                     boost::none /* legacyRuntimeConstants */,
                                                     true /* isTimeseriesViewRequest */);
    ASSERT_EQ(useTwoPhaseProtocol, false);
}

TEST_F(WriteWithoutShardKeyUtilTest,
       WriteQueryContainingPartialShardKeyCannotTargetSingleDocument) {
    auto useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     ns(),
                                                     true /* isUpdateOrDelete */,
                                                     false /* isUpsert */,
                                                     BSON("a" << 1),
                                                     {} /* collation */,
                                                     boost::none /* let */,
                                                     boost::none /* legacyRuntimeConstants */,
                                                     false /* isTimeseriesViewRequest */);
    ASSERT_EQ(useTwoPhaseProtocol, true);

    useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     ns(),
                                                     false /* isUpdateOrDelete */,
                                                     false /* isUpsert */,
                                                     BSON("a" << 1),
                                                     {} /* collation */,
                                                     boost::none /* let */,
                                                     boost::none /* legacyRuntimeConstants */,
                                                     false /* isTimeseriesViewRequest */);
    ASSERT_EQ(useTwoPhaseProtocol, true);
}

TEST_F(TimeseriesWriteWithoutShardKeyUtilTest,
       WriteQueryContainingPartialShardKeyCannotTargetSingleDocument) {
    auto useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     ns(),
                                                     true /* isUpdateOrDelete */,
                                                     false /* isUpsert */,
                                                     BSON("m.a" << 1),
                                                     {} /* collation */,
                                                     boost::none /* let */,
                                                     boost::none /* legacyRuntimeConstants */,
                                                     true /* isTimeseriesViewRequest */);
    ASSERT_EQ(useTwoPhaseProtocol, true);

    useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     ns(),
                                                     false /* isUpdateOrDelete */,
                                                     false /* isUpsert */,
                                                     BSON("m.a" << 1),
                                                     {} /* collation */,
                                                     boost::none /* let */,
                                                     boost::none /* legacyRuntimeConstants */,
                                                     true /* isTimeseriesViewRequest */);
    ASSERT_EQ(useTwoPhaseProtocol, true);
}

TEST_F(WriteWithoutShardKeyUtilTest,
       UpdateAndDeleteQueryContainingUnderscoreIdCanTargetSingleDocument) {
    auto useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     ns(),
                                                     true /* isUpdateOrDelete */,
                                                     false /* isUpsert */,
                                                     BSON("_id" << 1),
                                                     {} /* collation */,
                                                     boost::none /* let */,
                                                     boost::none /* legacyRuntimeConstants */,
                                                     false /* isTimeseriesViewRequest */);
    ASSERT_EQ(useTwoPhaseProtocol, false);
}

TEST_F(TimeseriesWriteWithoutShardKeyUtilTest,
       UpdateAndDeleteQueryContainingUnderscoreIdCannotTargetSingleDocument) {
    auto useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     ns(),
                                                     true /* isUpdateOrDelete */,
                                                     false /* isUpsert */,
                                                     BSON("_id" << 1),
                                                     {} /* collation */,
                                                     boost::none /* let */,
                                                     boost::none /* legacyRuntimeConstants */,
                                                     true /* isTimeseriesViewRequest */);
    ASSERT_EQ(useTwoPhaseProtocol, true);
}

TEST_F(WriteWithoutShardKeyUtilTest,
       WriteQueryWithoutShardKeyOrUnderscoreIdCannotTargetSingleDocument) {
    auto useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     ns(),
                                                     true /* isUpdateOrDelete */,
                                                     false /* isUpsert */,
                                                     BSON("x" << 1),
                                                     {} /* collation */,
                                                     boost::none /* let */,
                                                     boost::none /* legacyRuntimeConstants */,
                                                     false /* isTimeseriesViewRequest */);
    ASSERT_EQ(useTwoPhaseProtocol, true);

    useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     ns(),
                                                     false /* isUpdateOrDelete */,
                                                     false /* isUpsert */,
                                                     BSON("x" << 1),
                                                     {} /* collation */,
                                                     boost::none /* let */,
                                                     boost::none /* legacyRuntimeConstants */,
                                                     false /* isTimeseriesViewRequest */);
    ASSERT_EQ(useTwoPhaseProtocol, true);
}

TEST_F(TimeseriesWriteWithoutShardKeyUtilTest,
       WriteQueryWithoutShardKeyOrUnderscoreIdCannotTargetSingleDocument) {
    auto useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     ns(),
                                                     true /* isUpdateOrDelete */,
                                                     false /* isUpsert */,
                                                     BSON("x" << 1),
                                                     {} /* collation */,
                                                     boost::none /* let */,
                                                     boost::none /* legacyRuntimeConstants */,
                                                     true /* isTimeseriesViewRequest */);
    ASSERT_EQ(useTwoPhaseProtocol, true);

    useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     ns(),
                                                     false /* isUpdateOrDelete */,
                                                     false /* isUpsert */,
                                                     BSON("x" << 1),
                                                     {} /* collation */,
                                                     boost::none /* let */,
                                                     boost::none /* legacyRuntimeConstants */,
                                                     true /* isTimeseriesViewRequest */);
    ASSERT_EQ(useTwoPhaseProtocol, true);
}

TEST_F(WriteWithoutShardKeyUtilTest, FindAndModifyQueryWithOnlyIdMustUseTwoPhaseProtocol) {
    auto useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     ns(),
                                                     false /* isUpdateOrDelete */,
                                                     false /* isUpsert */,
                                                     BSON("_id" << 1),
                                                     {} /* collation */,
                                                     boost::none /* let */,
                                                     boost::none /* legacyRuntimeConstants */,
                                                     false /* isTimeseriesViewRequest */);
    ;
    ASSERT_EQ(useTwoPhaseProtocol, true);
}

TEST_F(TimeseriesWriteWithoutShardKeyUtilTest,
       FindAndModifyQueryWithOnlyIdMustUseTwoPhaseProtocol) {
    auto useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     ns(),
                                                     false /* isUpdateOrDelete */,
                                                     false /* isUpsert */,
                                                     BSON("_id" << 1),
                                                     {} /* collation */,
                                                     boost::none /* let */,
                                                     boost::none /* legacyRuntimeConstants */,
                                                     true /* isTimeseriesViewRequest */);
    ;
    ASSERT_EQ(useTwoPhaseProtocol, true);
}

TEST_F(WriteWithoutShardKeyUtilTest, FindAndModifyQueryWithoutShardKeyMustUseTwoPhaseProtocol) {
    auto useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     ns(),
                                                     false /* isUpdateOrDelete */,
                                                     false /* isUpsert */,
                                                     BSON("x" << 1),
                                                     {} /* collation */,
                                                     boost::none /* let */,
                                                     boost::none /* legacyRuntimeConstants */,
                                                     false /* isTimeseriesViewRequest */);
    ASSERT_EQ(useTwoPhaseProtocol, true);
}

TEST_F(TimeseriesWriteWithoutShardKeyUtilTest,
       FindAndModifyQueryWithoutShardKeyMustUseTwoPhaseProtocol) {
    auto useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     ns(),
                                                     false /* isUpdateOrDelete */,
                                                     false /* isUpsert */,
                                                     BSON("x" << 1),
                                                     {} /* collation */,
                                                     boost::none /* let */,
                                                     boost::none /* legacyRuntimeConstants */,
                                                     true /* isTimeseriesViewRequest */);
    ASSERT_EQ(useTwoPhaseProtocol, true);
}

TEST_F(UnshardedCollectionTest, UnshardedCollectionDoesNotUseTwoPhaseProtocol) {
    auto future = scheduleRoutingInfoUnforcedRefresh(ns());
    expectGetDatabase(ns());

    // Return an empty collection
    expectFindSendBSONObjVector(kConfigHostAndPort, {});

    auto cri = *future.default_timed_get();
    ASSERT(!cri.isSharded());
    ASSERT(!cri.getChunkManager().isSharded());

    auto useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     ns(),
                                                     true /* isUpdateOrDelete */,
                                                     false /* isUpsert */,
                                                     BSON("x" << 1),
                                                     {} /* collation */,
                                                     boost::none /* let */,
                                                     boost::none /* legacyRuntimeConstants */,
                                                     false /* isTimeseriesViewRequest */);
    ASSERT_EQ(useTwoPhaseProtocol, false);
}

TEST_F(TimeseriesUnshardedCollectionTest, UnshardedCollectionDoesNotUseTwoPhaseProtocol) {
    auto future = scheduleRoutingInfoUnforcedRefresh(ns());
    expectGetDatabase(ns());

    // Return an empty collection
    expectFindSendBSONObjVector(kConfigHostAndPort, {});

    auto cri = *future.default_timed_get();
    ASSERT(!cri.isSharded());
    ASSERT(!cri.getChunkManager().isSharded());

    auto useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     ns(),
                                                     true /* isUpdateOrDelete */,
                                                     false /* isUpsert */,
                                                     BSON("x" << 1),
                                                     {} /* collation */,
                                                     boost::none /* let */,
                                                     boost::none /* legacyRuntimeConstants */,
                                                     true /* isTimeseriesViewRequest */);
    ASSERT_EQ(useTwoPhaseProtocol, false);
}

TEST_F(WriteWithoutShardKeyUtilTest,
       WriteQueryWithFullShardKeyAndCollationWithCollatableTypesUsesTwoPhaseProtocol) {
    auto useTwoPhaseProtocol = write_without_shard_key::useTwoPhaseProtocol(
        getOpCtx(),
        ns(),
        true /* isUpdateOrDelete */,
        false /* isUpsert */,
        BSON("a" << "a"
                 << "b"
                 << "b"),
        BSON("collation" << "lowercase") /* collation */,
        boost::none /* let */,
        boost::none /* legacyRuntimeConstants */,
        false /* isTimeseriesViewRequest */);
    ASSERT_EQ(useTwoPhaseProtocol, true);
}

TEST_F(TimeseriesWriteWithoutShardKeyUtilTest,
       WriteQueryWithFullShardKeyAndCollationWithCollatableTypesUsesTwoPhaseProtocol) {
    auto useTwoPhaseProtocol = write_without_shard_key::useTwoPhaseProtocol(
        getOpCtx(),
        ns(),
        true /* isUpdateOrDelete */,
        false /* isUpsert */,
        BSON("m.a" << "a"
                   << "m.b"
                   << "b"),
        BSON("collation" << "lowercase") /* collation */,
        boost::none /* let */,
        boost::none /* legacyRuntimeConstants */,
        true /* isTimeseriesViewRequest */);
    ASSERT_EQ(useTwoPhaseProtocol, true);
}

TEST_F(WriteWithoutShardKeyUtilTest,
       WriteQueryWithFullShardKeyAndCollationWithoutCollatableTypesDoesNotUseTwoPhaseProtocol) {
    auto useTwoPhaseProtocol = write_without_shard_key::useTwoPhaseProtocol(
        getOpCtx(),
        ns(),
        true /* isUpdateOrDelete */,
        false /* isUpsert */,
        BSON("a" << 1 << "b" << 1),
        BSON("collation" << "lowercase") /* collation */,
        boost::none /* let */,
        boost::none /* legacyRuntimeConstants */,
        false /* isTimeseriesViewRequest */);
    ASSERT_EQ(useTwoPhaseProtocol, false);
}

TEST_F(TimeseriesWriteWithoutShardKeyUtilTest,
       WriteQueryWithFullShardKeyAndCollationWithoutCollatableTypesDoesNotUseTwoPhaseProtocol) {
    auto useTwoPhaseProtocol = write_without_shard_key::useTwoPhaseProtocol(
        getOpCtx(),
        ns(),
        true /* isUpdateOrDelete */,
        false /* isUpsert */,
        BSON("m.a" << 1 << "m.b" << 1),
        BSON("collation" << "lowercase") /* collation */,
        boost::none /* let */,
        boost::none /* legacyRuntimeConstants */,
        true /* isTimeseriesViewRequest */);
    ASSERT_EQ(useTwoPhaseProtocol, false);
}

TEST_F(WriteWithoutShardKeyUtilTest,
       WriteQueryWithOnlyIdAndCollationWithCollatableTypeUsesTwoPhaseProtocol) {
    auto useTwoPhaseProtocol = write_without_shard_key::useTwoPhaseProtocol(
        getOpCtx(),
        ns(),
        true /* isUpdateOrDelete */,
        false /* isUpsert */,
        BSON("_id" << "hello"),
        BSON("collation" << "lowercase") /* collation */,
        boost::none /* let */,
        boost::none /* legacyRuntimeConstants */,
        false /* isTimeseriesViewRequest */);
    ASSERT_EQ(useTwoPhaseProtocol, true);
}

TEST_F(TimeseriesWriteWithoutShardKeyUtilTest,
       WriteQueryWithOnlyIdAndCollationWithCollatableTypeUsesTwoPhaseProtocol) {
    auto useTwoPhaseProtocol = write_without_shard_key::useTwoPhaseProtocol(
        getOpCtx(),
        ns(),
        true /* isUpdateOrDelete */,
        false /* isUpsert */,
        BSON("_id" << "hello"),
        BSON("collation" << "lowercase") /* collation */,
        boost::none /* let */,
        boost::none /* legacyRuntimeConstants */,
        true /* isTimeseriesViewRequest */);
    ASSERT_EQ(useTwoPhaseProtocol, true);
}

TEST_F(WriteWithoutShardKeyUtilTest,
       WriteQueryWithOnlyIdAndCollationWithoutCollatableTypeDoesNotUseTwoPhaseProtocol) {
    auto useTwoPhaseProtocol = write_without_shard_key::useTwoPhaseProtocol(
        getOpCtx(),
        ns(),
        true /* isUpdateOrDelete */,
        false /* isUpsert */,
        BSON("_id" << 1),
        BSON("collation" << "lowercase") /* collation */,
        boost::none /* let */,
        boost::none /* legacyRuntimeConstants */,
        false /* isTimeseriesViewRequest */);
    ASSERT_EQ(useTwoPhaseProtocol, false);
}

TEST_F(TimeseriesWriteWithoutShardKeyUtilTest,
       WriteQueryWithOnlyIdAndCollationWithoutCollatableTypeDoesUsesTwoPhaseProtocol) {
    auto useTwoPhaseProtocol = write_without_shard_key::useTwoPhaseProtocol(
        getOpCtx(),
        ns(),
        true /* isUpdateOrDelete */,
        false /* isUpsert */,
        BSON("_id" << 1),
        BSON("collation" << "lowercase") /* collation */,
        boost::none /* let */,
        boost::none /* legacyRuntimeConstants */,
        true /* isTimeseriesViewRequest */);
    ASSERT_EQ(useTwoPhaseProtocol, true);
}

TEST_F(WriteWithoutShardKeyUtilTest, WriteQueryWithOnlyIdAndUpsertUsesTwoPhaseProtocol) {
    auto useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     ns(),
                                                     true /* isUpdateOrDelete */,
                                                     true /* isUpsert */,
                                                     BSON("_id" << BSON("$eq" << 1)),
                                                     {} /* collation */,
                                                     boost::none /* let */,
                                                     boost::none /* legacyRuntimeConstants */,
                                                     false /* isTimeseriesViewRequest */);
    ASSERT_EQ(useTwoPhaseProtocol, true);
}

TEST_F(TimeseriesWriteWithoutShardKeyUtilTest, WriteQueryWithOnlyIdAndUpsertUsesTwoPhaseProtocol) {
    auto useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     ns(),
                                                     true /* isUpdateOrDelete */,
                                                     true /* isUpsert */,
                                                     BSON("_id" << BSON("$eq" << 1)),
                                                     {} /* collation */,
                                                     boost::none /* let */,
                                                     boost::none /* legacyRuntimeConstants */,
                                                     true /* isTimeseriesViewRequest */);
    ASSERT_EQ(useTwoPhaseProtocol, true);
}

TEST_F(WriteWithoutShardKeyUtilTest,
       WriteQueryContainingPartialShardKeyAndIdPerformingAnUpsertUsesTwoPhaseProtocol) {
    auto useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     ns(),
                                                     true /* isUpdateOrDelete */,
                                                     true /* isUpsert */,
                                                     BSON("a" << 1 << "_id" << BSON("$eq" << 1)),
                                                     {} /* collation */,
                                                     boost::none /* let */,
                                                     boost::none /* legacyRuntimeConstants */,
                                                     false /* isTimeseriesViewRequest */);
    ASSERT_EQ(useTwoPhaseProtocol, true);
}

TEST_F(TimeseriesWriteWithoutShardKeyUtilTest,
       WriteQueryContainingPartialShardKeyAndIdPerformingAnUpsertUsesTwoPhaseProtocol) {
    auto useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     ns(),
                                                     true /* isUpdateOrDelete */,
                                                     true /* isUpsert */,
                                                     BSON("m.a" << 1 << "_id" << BSON("$eq" << 1)),
                                                     {} /* collation */,
                                                     boost::none /* let */,
                                                     boost::none /* legacyRuntimeConstants */,
                                                     true /* isTimeseriesViewRequest */);
    ASSERT_EQ(useTwoPhaseProtocol, true);
}

TEST_F(WriteWithoutShardKeyUtilTest,
       WriteQueryWithOnlyIdThatIsNotADirectEqualityUsesTwoPhaseProtocol) {
    auto useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     ns(),
                                                     true /* isUpdateOrDelete */,
                                                     false /* isUpsert */,
                                                     BSON("_id" << BSON("$gt" << 1)),
                                                     {} /* collation */,
                                                     boost::none /* let */,
                                                     boost::none /* legacyRuntimeConstants */,
                                                     false /* isTimeseriesViewRequest */);
    ASSERT_EQ(useTwoPhaseProtocol, true);
}

TEST_F(TimeseriesWriteWithoutShardKeyUtilTest,
       WriteQueryWithOnlyIdThatIsNotADirectEqualityUsesTwoPhaseProtocol) {
    auto useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     ns(),
                                                     true /* isUpdateOrDelete */,
                                                     false /* isUpsert */,
                                                     BSON("_id" << BSON("$gt" << 1)),
                                                     {} /* collation */,
                                                     boost::none /* let */,
                                                     boost::none /* legacyRuntimeConstants */,
                                                     true /* isTimeseriesViewRequest */);
    ASSERT_EQ(useTwoPhaseProtocol, true);
}

TEST_F(ProduceUpsertDocumentTest, produceUpsertDocumentUsingReplacementUpdate) {
    write_ops::UpdateOpEntry entry;
    entry.setQ(BSON("_id" << 3));
    entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(BSON("x" << 2)));

    write_ops::UpdateCommandRequest updateCommandRequest(ns());
    updateCommandRequest.setUpdates({entry});
    UpdateRequest updateRequest(updateCommandRequest.getUpdates().front());

    auto [doc, _] =
        write_without_shard_key::generateUpsertDocument(getOpCtx(),
                                                        updateRequest,
                                                        UUID::gen(),
                                                        /*timeseriesOptions=*/boost::none,
                                                        /*comparator=*/nullptr);
    ASSERT_BSONOBJ_EQ(doc, fromjson("{ _id: 3, x: 2 }"));
}

TEST_F(ProduceUpsertDocumentTest, produceUpsertDocumentUsingLetConstantAndPipelineUpdate) {
    write_ops::UpdateOpEntry entry;
    entry.setQ(BSON("_id" << 4 << "x" << 3));

    std::vector<BSONObj> pipelineUpdate;
    pipelineUpdate.push_back(fromjson("{$set: {'x': '$$constOne'}}"));
    pipelineUpdate.push_back(fromjson("{$set: {'y': 3}}"));
    entry.setU(pipelineUpdate);

    BSONObj constants = fromjson("{constOne: 'foo'}");
    entry.setC(constants);

    write_ops::UpdateCommandRequest updateCommandRequest(ns());
    updateCommandRequest.setUpdates({entry});
    UpdateRequest updateRequest(updateCommandRequest.getUpdates().front());

    auto [doc, _] =
        write_without_shard_key::generateUpsertDocument(getOpCtx(),
                                                        updateRequest,
                                                        UUID::gen(),
                                                        /*timeseriesOptions=*/boost::none,
                                                        /*comparator=*/nullptr);
    ASSERT_BSONOBJ_EQ(doc, fromjson("{ _id: 4, x: 'foo', y: 3 }"));
}

TEST_F(ProduceUpsertDocumentTest, produceUpsertDocumentUsingArrayFilterAndModificationUpdate) {
    write_ops::UpdateOpEntry entry;
    BSONArrayBuilder arrayBuilder;
    arrayBuilder.append(BSON("a" << 90));
    entry.setQ(BSON("_id" << 4 << "x" << arrayBuilder.arr()));
    entry.setU(
        write_ops::UpdateModification::parseFromClassicUpdate(fromjson("{$inc: {'x.$[b].a': 3}}")));

    auto arrayFilter = std::vector<BSONObj>{fromjson("{'b.a': {$gt: 85}}")};
    entry.setArrayFilters(arrayFilter);

    write_ops::UpdateCommandRequest updateCommandRequest(ns());
    updateCommandRequest.setUpdates({entry});
    UpdateRequest updateRequest(updateCommandRequest.getUpdates().front());

    auto [doc, _] =
        write_without_shard_key::generateUpsertDocument(getOpCtx(),
                                                        updateRequest,
                                                        UUID::gen(),
                                                        /*timeseriesOptions=*/boost::none,
                                                        /*comparator=*/nullptr);
    ASSERT_BSONOBJ_EQ(doc, fromjson("{ _id: 4, x: [ { a: 93 } ] }"));
}

TEST_F(ProduceUpsertDocumentTest, produceUpsertDocumentUsingCollation) {
    write_ops::UpdateOpEntry entry;
    BSONArrayBuilder arrayBuilder;
    arrayBuilder.append(BSON("a" << "BAR"));
    arrayBuilder.append(BSON("a" << "bar"));
    arrayBuilder.append(BSON("a" << "foo"));
    entry.setQ(BSON("_id" << 4 << "x" << arrayBuilder.arr()));
    entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(
        fromjson("{$set: {'x.$[b].a': 'FOO'}}")));

    auto arrayFilter = std::vector<BSONObj>{fromjson("{'b.a': {$eq: 'bar'}}")};
    entry.setArrayFilters(arrayFilter);
    entry.setCollation(fromjson("{locale: 'en_US', strength: 2}"));

    write_ops::UpdateCommandRequest updateCommandRequest(ns());
    updateCommandRequest.setUpdates({entry});
    UpdateRequest updateRequest(updateCommandRequest.getUpdates().front());

    auto [doc, _] =
        write_without_shard_key::generateUpsertDocument(getOpCtx(),
                                                        updateRequest,
                                                        UUID::gen(),
                                                        /*timeseriesOptions=*/boost::none,
                                                        /*comparator=*/nullptr);
    ASSERT_BSONOBJ_EQ(doc, fromjson("{ _id: 4, x: [ { a: 'FOO' }, { a: 'FOO' }, { a: 'foo' } ] }"));
}

}  // namespace
}  // namespace write_without_shard_key
}  // namespace mongo
