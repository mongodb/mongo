/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/s/collection_range_deleter.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/query.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/s/catalog/dist_lock_catalog_impl.h"
#include "mongo/s/catalog/dist_lock_manager_mock.h"
#include "mongo/s/catalog/sharding_catalog_client_mock.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/sharding_mongod_test_fixture.h"

namespace mongo {

using unittest::assertGet;

const NamespaceString kNss = NamespaceString("foo", "bar");
const std::string kPattern = "_id";
const BSONObj kKeyPattern = BSON(kPattern << 1);
const std::string kShardName{"a"};
const HostAndPort dummyHost("dummy", 123);
const NamespaceString kAdminSystemVersion = NamespaceString("admin", "system.version");

class CollectionRangeDeleterTest : public ShardingMongodTestFixture {
public:
    using Deletion = CollectionRangeDeleter::Deletion;
    using Action = CollectionRangeDeleter::Action;

protected:
    auto next(CollectionRangeDeleter& rangeDeleter, Action action, int maxToDelete)
        -> CollectionRangeDeleter::Action {
        return CollectionRangeDeleter::cleanUpNextRange(
            operationContext(), kNss, action, maxToDelete, &rangeDeleter);
    }
    std::shared_ptr<RemoteCommandTargeterMock> configTargeter() {
        return RemoteCommandTargeterMock::get(shardRegistry()->getConfigShard()->getTargeter());
    }
    OID const& epoch() {
        return _epoch;
    }

private:
    void setUp() override;
    void tearDown() override;

    std::unique_ptr<DistLockCatalog> makeDistLockCatalog(ShardRegistry* shardRegistry) override {
        invariant(shardRegistry);
        return stdx::make_unique<DistLockCatalogImpl>(shardRegistry);
    }

    std::unique_ptr<DistLockManager> makeDistLockManager(
        std::unique_ptr<DistLockCatalog> distLockCatalog) override {
        return stdx::make_unique<DistLockManagerMock>(std::move(distLockCatalog));
    }

    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient(
        std::unique_ptr<DistLockManager> distLockManager) override {
        return stdx::make_unique<ShardingCatalogClientMock>(std::move(distLockManager));
    }

    OID _epoch;
};

std::ostream& operator<<(std::ostream& os, CollectionRangeDeleter::Action a) {
    return os << (int)a;
}

void CollectionRangeDeleterTest::setUp() {
    _epoch = OID::gen();
    serverGlobalParams.clusterRole = ClusterRole::ShardServer;
    ShardingMongodTestFixture::setUp();
    replicationCoordinator()->alwaysAllowWrites(true);
    initializeGlobalShardingStateForMongodForTest(ConnectionString(dummyHost))
        .transitional_ignore();

    // RemoteCommandTargeterMock::get(shardRegistry()->getConfigShard()->getTargeter())
    //     ->setConnectionStringReturnValue(kConfigConnStr);

    configTargeter()->setFindHostReturnValue(dummyHost);

    DBDirectClient(operationContext()).createCollection(kNss.ns());
    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IX);
        auto collectionShardingState = CollectionShardingState::get(operationContext(), kNss);
        collectionShardingState->refreshMetadata(
            operationContext(),
            stdx::make_unique<CollectionMetadata>(
                kKeyPattern,
                ChunkVersion(1, 0, epoch()),
                ChunkVersion(0, 0, epoch()),
                SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<CachedChunkInfo>()));
    }
}

void CollectionRangeDeleterTest::tearDown() {
    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IX);
        auto collectionShardingState = CollectionShardingState::get(operationContext(), kNss);
        collectionShardingState->refreshMetadata(operationContext(), nullptr);
    }
    ShardingMongodTestFixture::tearDown();
}

namespace {

// Tests the case that there is nothing in the database.
TEST_F(CollectionRangeDeleterTest, EmptyDatabase) {
    CollectionRangeDeleter rangeDeleter;
    ASSERT_EQ(Action::kFinished, next(rangeDeleter, Action::kWriteOpLog, 1));
}

// Tests the case that there is data, but it is not in a range to clean.
TEST_F(CollectionRangeDeleterTest, NoDataInGivenRangeToClean) {
    CollectionRangeDeleter rangeDeleter;
    const BSONObj insertedDoc = BSON(kPattern << 25);
    DBDirectClient dbclient(operationContext());
    dbclient.insert(kNss.toString(), insertedDoc);
    ASSERT_BSONOBJ_EQ(insertedDoc, dbclient.findOne(kNss.toString(), QUERY(kPattern << 25)));
    std::list<Deletion> ranges;
    ranges.emplace_back(Deletion(ChunkRange(BSON(kPattern << 0), BSON(kPattern << 10))));
    ASSERT_TRUE(rangeDeleter.add(std::move(ranges)));
    ASSERT_EQ(1u, rangeDeleter.size());
    ASSERT_EQ(Action::kWriteOpLog, next(rangeDeleter, Action::kWriteOpLog, 1));

    ASSERT_EQ(0u, rangeDeleter.size());
    ASSERT_BSONOBJ_EQ(insertedDoc, dbclient.findOne(kNss.toString(), QUERY(kPattern << 25)));

    ASSERT_EQ(Action::kFinished, next(rangeDeleter, Action::kWriteOpLog, 1));
}

// Tests the case that there is a single document within a range to clean.
TEST_F(CollectionRangeDeleterTest, OneDocumentInOneRangeToClean) {
    CollectionRangeDeleter rangeDeleter;
    const BSONObj insertedDoc = BSON(kPattern << 5);
    DBDirectClient dbclient(operationContext());
    dbclient.insert(kNss.toString(), BSON(kPattern << 5));
    ASSERT_BSONOBJ_EQ(insertedDoc, dbclient.findOne(kNss.toString(), QUERY(kPattern << 5)));

    std::list<Deletion> ranges;
    Deletion deletion{ChunkRange(BSON(kPattern << 0), BSON(kPattern << 10))};
    ranges.emplace_back(std::move(deletion));
    ASSERT_TRUE(rangeDeleter.add(std::move(ranges)));
    ASSERT_TRUE(ranges.empty());  // spliced elements out of it

    auto optNotifn = rangeDeleter.overlaps(ChunkRange(BSON(kPattern << 0), BSON(kPattern << 10)));
    ASSERT(optNotifn);
    auto notifn = *optNotifn;
    ASSERT(!notifn.ready());
    ASSERT_EQ(Action::kMore, next(rangeDeleter, Action::kWriteOpLog, 1));  // actually delete one
    ASSERT(!notifn.ready());

    ASSERT_EQ(rangeDeleter.size(), 1u);
    // range empty, pop range, notify
    ASSERT_EQ(Action::kWriteOpLog, next(rangeDeleter, Action::kMore, 1));
    ASSERT_TRUE(rangeDeleter.isEmpty());
    ASSERT(notifn.ready() && notifn.waitStatus(operationContext()).isOK());

    ASSERT_TRUE(dbclient.findOne(kNss.toString(), QUERY(kPattern << 5)).isEmpty());
    ASSERT_EQ(Action::kFinished, next(rangeDeleter, Action::kWriteOpLog, 1));
}

// Tests the case that there are multiple documents within a range to clean.
TEST_F(CollectionRangeDeleterTest, MultipleDocumentsInOneRangeToClean) {
    CollectionRangeDeleter rangeDeleter;
    DBDirectClient dbclient(operationContext());
    dbclient.insert(kNss.toString(), BSON(kPattern << 1));
    dbclient.insert(kNss.toString(), BSON(kPattern << 2));
    dbclient.insert(kNss.toString(), BSON(kPattern << 3));
    ASSERT_EQUALS(3ULL, dbclient.count(kNss.toString(), BSON(kPattern << LT << 5)));

    std::list<Deletion> ranges;
    Deletion deletion{ChunkRange(BSON(kPattern << 0), BSON(kPattern << 10))};
    ranges.emplace_back(std::move(deletion));
    ASSERT_TRUE(rangeDeleter.add(std::move(ranges)));

    ASSERT_EQ(Action::kMore, next(rangeDeleter, Action::kWriteOpLog, 100));
    ASSERT_EQ(Action::kWriteOpLog, next(rangeDeleter, Action::kMore, 100));
    ASSERT_EQUALS(0ULL, dbclient.count(kNss.toString(), BSON(kPattern << LT << 5)));
    ASSERT_EQ(Action::kFinished, next(rangeDeleter, Action::kWriteOpLog, 100));
}

// Tests the case that there are multiple documents within a range to clean, and the range deleter
// has a max deletion rate of one document per run.
TEST_F(CollectionRangeDeleterTest, MultipleCleanupNextRangeCalls) {
    CollectionRangeDeleter rangeDeleter;
    DBDirectClient dbclient(operationContext());
    dbclient.insert(kNss.toString(), BSON(kPattern << 1));
    dbclient.insert(kNss.toString(), BSON(kPattern << 2));
    dbclient.insert(kNss.toString(), BSON(kPattern << 3));
    ASSERT_EQUALS(3ULL, dbclient.count(kNss.toString(), BSON(kPattern << LT << 5)));

    std::list<Deletion> ranges;
    Deletion deletion{ChunkRange(BSON(kPattern << 0), BSON(kPattern << 10))};
    ranges.emplace_back(std::move(deletion));
    ASSERT_TRUE(rangeDeleter.add(std::move(ranges)));

    ASSERT_EQ(Action::kMore, next(rangeDeleter, Action::kWriteOpLog, 1));
    ASSERT_EQUALS(2ULL, dbclient.count(kNss.toString(), BSON(kPattern << LT << 5)));

    ASSERT_EQ(Action::kMore, next(rangeDeleter, Action::kMore, 1));
    ASSERT_EQUALS(1ULL, dbclient.count(kNss.toString(), BSON(kPattern << LT << 5)));

    ASSERT_EQ(Action::kMore, next(rangeDeleter, Action::kMore, 1));
    ASSERT_EQ(Action::kWriteOpLog, next(rangeDeleter, Action::kMore, 1));
    ASSERT_EQUALS(0ULL, dbclient.count(kNss.toString(), BSON(kPattern << LT << 5)));
    ASSERT_EQ(Action::kFinished, next(rangeDeleter, Action::kWriteOpLog, 1));
}

// Tests the case that there are two ranges to clean, each containing multiple documents.
TEST_F(CollectionRangeDeleterTest, MultipleDocumentsInMultipleRangesToClean) {
    CollectionRangeDeleter rangeDeleter;
    DBDirectClient dbclient(operationContext());
    dbclient.insert(kNss.toString(), BSON(kPattern << 1));
    dbclient.insert(kNss.toString(), BSON(kPattern << 2));
    dbclient.insert(kNss.toString(), BSON(kPattern << 3));
    dbclient.insert(kNss.toString(), BSON(kPattern << 4));
    dbclient.insert(kNss.toString(), BSON(kPattern << 5));
    dbclient.insert(kNss.toString(), BSON(kPattern << 6));
    ASSERT_EQUALS(6ULL, dbclient.count(kNss.toString(), BSON(kPattern << LT << 10)));

    std::list<Deletion> ranges;
    ranges.emplace_back(Deletion{ChunkRange{BSON(kPattern << 0), BSON(kPattern << 4)}});
    ASSERT_TRUE(rangeDeleter.add(std::move(ranges)));
    ASSERT_TRUE(ranges.empty());
    ranges.emplace_back(Deletion{ChunkRange{BSON(kPattern << 4), BSON(kPattern << 7)}});
    ASSERT_FALSE(rangeDeleter.add(std::move(ranges)));

    auto optNotifn1 = rangeDeleter.overlaps(ChunkRange{BSON(kPattern << 0), BSON(kPattern << 4)});
    ASSERT_TRUE(optNotifn1);
    auto& notifn1 = *optNotifn1;
    ASSERT_FALSE(notifn1.ready());
    auto optNotifn2 = rangeDeleter.overlaps(ChunkRange{BSON(kPattern << 4), BSON(kPattern << 7)});
    ASSERT_TRUE(optNotifn2);
    auto& notifn2 = *optNotifn2;
    ASSERT_FALSE(notifn2.ready());

    // test op== on notifications
    ASSERT_TRUE(notifn1 == *optNotifn1);
    ASSERT_FALSE(notifn1 == *optNotifn2);
    ASSERT_TRUE(notifn1 != *optNotifn2);
    ASSERT_FALSE(notifn1 != *optNotifn1);

    ASSERT_EQUALS(0ULL,
                  dbclient.count(kAdminSystemVersion.ns(), BSON(kPattern << "startRangeDeletion")));

    ASSERT_EQ(Action::kMore, next(rangeDeleter, Action::kWriteOpLog, 100));
    ASSERT_FALSE(notifn1.ready());  // no trigger yet
    ASSERT_FALSE(notifn2.ready());  // no trigger yet

    ASSERT_EQUALS(1ULL,
                  dbclient.count(kAdminSystemVersion.ns(), BSON(kPattern << "startRangeDeletion")));
    // clang-format off
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << "startRangeDeletion" << "ns" << kNss.ns()
          << "epoch" << epoch() << "min" << BSON("_id" << 0) << "max" << BSON("_id" << 4)),
        dbclient.findOne(kAdminSystemVersion.ns(), QUERY("_id" << "startRangeDeletion")));
    // clang-format on

    ASSERT_EQUALS(0ULL, dbclient.count(kNss.ns(), BSON(kPattern << LT << 4)));
    ASSERT_EQUALS(3ULL, dbclient.count(kNss.ns(), BSON(kPattern << LT << 10)));

    // discover there are no more < 4, pop range 1
    ASSERT_EQ(Action::kWriteOpLog, next(rangeDeleter, Action::kMore, 100));

    ASSERT_EQUALS(1ULL,
                  dbclient.count(kAdminSystemVersion.ns(), BSON(kPattern << "startRangeDeletion")));
    // clang-format off
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << "startRangeDeletion" << "ns" << kNss.ns()
          << "epoch" << epoch() << "min" << BSON("_id" << 0) << "max" << BSON("_id" << 4)),
        dbclient.findOne(kAdminSystemVersion.ns(), QUERY("_id" << "startRangeDeletion")));
    // clang-format on

    ASSERT_TRUE(notifn1.ready() && notifn1.waitStatus(operationContext()).isOK());
    ASSERT_FALSE(notifn2.ready());

    ASSERT_EQUALS(3ULL, dbclient.count(kNss.ns(), BSON(kPattern << LT << 10)));

    // delete the remaining documents
    ASSERT_EQ(Action::kMore, next(rangeDeleter, Action::kWriteOpLog, 100));
    ASSERT_FALSE(notifn2.ready());

    // clang-format off
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << "startRangeDeletion" << "ns" << kNss.ns()
          << "epoch" << epoch() << "min" << BSON("_id" << 4) << "max" << BSON("_id" << 7)),
        dbclient.findOne(kAdminSystemVersion.ns(), QUERY("_id" << "startRangeDeletion")));
    // clang-format on

    ASSERT_EQUALS(0ULL, dbclient.count(kNss.ns(), BSON(kPattern << LT << 10)));

    // discover there are no more, pop range 2
    ASSERT_EQ(Action::kWriteOpLog, next(rangeDeleter, Action::kMore, 1));

    ASSERT_TRUE(notifn2.ready() && notifn2.waitStatus(operationContext()).isOK());

    // discover there are no more ranges
    ASSERT_EQ(Action::kFinished, next(rangeDeleter, Action::kWriteOpLog, 1));
}

}  // unnamed namespace
}  // namespace mongo
