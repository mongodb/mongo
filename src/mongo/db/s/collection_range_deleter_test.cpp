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
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/sharding_mongod_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

using unittest::assertGet;

const NamespaceString kNss = NamespaceString("foo", "bar");
const std::string kPattern = "_id";
const BSONObj kKeyPattern = BSON(kPattern << 1);
const std::string kShardName{"a"};
const HostAndPort dummyHost("dummy", 123);
const NamespaceString kAdminSysVer = NamespaceString("admin", "system.version");

class CollectionRangeDeleterTest : public ShardingMongodTestFixture {
public:
    using Deletion = CollectionRangeDeleter::Deletion;

protected:
    boost::optional<Date_t> next(CollectionRangeDeleter& rangeDeleter, int maxToDelete) {
        return CollectionRangeDeleter::cleanUpNextRange(
            operationContext(), kNss, epoch(), maxToDelete, &rangeDeleter);
    }

    std::shared_ptr<RemoteCommandTargeterMock> configTargeter() {
        return RemoteCommandTargeterMock::get(shardRegistry()->getConfigShard()->getTargeter());
    }

    OID const& epoch() {
        return _epoch;
    }

    virtual std::unique_ptr<BalancerConfiguration> makeBalancerConfiguration() override {
        return stdx::make_unique<BalancerConfiguration>();
    }

private:
    void setUp() override;
    void tearDown() override;

    OID _epoch;
};

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
        const KeyPattern skPattern(kKeyPattern);
        auto cm = ChunkManager::makeNew(
            kNss,
            kKeyPattern,
            nullptr,
            false,
            epoch(),
            {ChunkType(kNss,
                       ChunkRange{skPattern.globalMin(), skPattern.globalMax()},
                       ChunkVersion(1, 0, epoch()),
                       ShardId("otherShard"))});
        collectionShardingState->refreshMetadata(
            operationContext(), stdx::make_unique<CollectionMetadata>(cm, ShardId("thisShard")));
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
    ASSERT_FALSE(next(rangeDeleter, 1));
}

// Tests the case that there is data, but it is not in a range to clean.
TEST_F(CollectionRangeDeleterTest, NoDataInGivenRangeToClean) {
    CollectionRangeDeleter rangeDeleter;
    const BSONObj insertedDoc = BSON(kPattern << 25);
    DBDirectClient dbclient(operationContext());
    dbclient.insert(kNss.toString(), insertedDoc);
    ASSERT_BSONOBJ_EQ(insertedDoc, dbclient.findOne(kNss.toString(), QUERY(kPattern << 25)));
    std::list<Deletion> ranges;
    ranges.emplace_back(Deletion{ChunkRange{BSON(kPattern << 0), BSON(kPattern << 10)}, Date_t{}});
    auto when = rangeDeleter.add(std::move(ranges));
    ASSERT(when && *when == Date_t{});
    ASSERT_EQ(1u, rangeDeleter.size());
    ASSERT_TRUE(next(rangeDeleter, 1));

    ASSERT_EQ(0u, rangeDeleter.size());
    ASSERT_BSONOBJ_EQ(insertedDoc, dbclient.findOne(kNss.toString(), QUERY(kPattern << 25)));

    ASSERT_FALSE(next(rangeDeleter, 1));
}

// Tests the case that there is a single document within a range to clean.
TEST_F(CollectionRangeDeleterTest, OneDocumentInOneRangeToClean) {
    CollectionRangeDeleter rangeDeleter;
    const BSONObj insertedDoc = BSON(kPattern << 5);
    DBDirectClient dbclient(operationContext());
    dbclient.insert(kNss.toString(), BSON(kPattern << 5));
    ASSERT_BSONOBJ_EQ(insertedDoc, dbclient.findOne(kNss.toString(), QUERY(kPattern << 5)));

    std::list<Deletion> ranges;
    auto deletion = Deletion{ChunkRange(BSON(kPattern << 0), BSON(kPattern << 10)), Date_t{}};
    ranges.emplace_back(std::move(deletion));
    auto when = rangeDeleter.add(std::move(ranges));
    ASSERT(when && *when == Date_t{});
    ASSERT_TRUE(ranges.empty());  // spliced elements out of it

    auto optNotifn = rangeDeleter.overlaps(ChunkRange(BSON(kPattern << 0), BSON(kPattern << 10)));
    ASSERT(optNotifn);
    auto notifn = *optNotifn;
    ASSERT(!notifn.ready());
    // actually delete one
    ASSERT_TRUE(next(rangeDeleter, 1));
    ASSERT(!notifn.ready());

    ASSERT_EQ(rangeDeleter.size(), 1u);
    // range empty, pop range, notify
    ASSERT_TRUE(next(rangeDeleter, 1));
    ASSERT_TRUE(rangeDeleter.isEmpty());
    ASSERT(notifn.ready() && notifn.waitStatus(operationContext()).isOK());

    ASSERT_TRUE(dbclient.findOne(kNss.toString(), QUERY(kPattern << 5)).isEmpty());
    ASSERT_FALSE(next(rangeDeleter, 1));
    ASSERT_EQUALS(0ULL, dbclient.count(kAdminSysVer.ns(), BSON(kPattern << "startRangeDeletion")));
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
    auto deletion = Deletion{ChunkRange(BSON(kPattern << 0), BSON(kPattern << 10)), Date_t{}};
    ranges.emplace_back(std::move(deletion));
    auto when = rangeDeleter.add(std::move(ranges));
    ASSERT(when && *when == Date_t{});

    ASSERT_TRUE(next(rangeDeleter, 100));
    ASSERT_TRUE(next(rangeDeleter, 100));
    ASSERT_EQUALS(0ULL, dbclient.count(kNss.toString(), BSON(kPattern << LT << 5)));
    ASSERT_FALSE(next(rangeDeleter, 100));
    ASSERT_EQUALS(0ULL, dbclient.count(kAdminSysVer.ns(), BSON(kPattern << "startRangeDeletion")));
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
    auto deletion = Deletion{ChunkRange(BSON(kPattern << 0), BSON(kPattern << 10)), Date_t{}};
    ranges.emplace_back(std::move(deletion));
    auto when = rangeDeleter.add(std::move(ranges));
    ASSERT(when && *when == Date_t{});

    ASSERT_TRUE(next(rangeDeleter, 1));
    ASSERT_EQUALS(2ULL, dbclient.count(kNss.toString(), BSON(kPattern << LT << 5)));

    ASSERT_TRUE(next(rangeDeleter, 1));
    ASSERT_EQUALS(1ULL, dbclient.count(kNss.toString(), BSON(kPattern << LT << 5)));

    ASSERT_TRUE(next(rangeDeleter, 1));
    ASSERT_TRUE(next(rangeDeleter, 1));
    ASSERT_EQUALS(0ULL, dbclient.count(kNss.toString(), BSON(kPattern << LT << 5)));
    ASSERT_FALSE(next(rangeDeleter, 1));
    ASSERT_EQUALS(0ULL, dbclient.count(kAdminSysVer.ns(), BSON(kPattern << "startRangeDeletion")));
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
    auto later = Date_t::now();
    ranges.emplace_back(Deletion{ChunkRange{BSON(kPattern << 0), BSON(kPattern << 3)}, later});
    auto when = rangeDeleter.add(std::move(ranges));
    ASSERT(when && *when == later);
    ASSERT_TRUE(ranges.empty());  // not guaranteed by std, but failure would indicate a problem.

    std::list<Deletion> ranges2;
    ranges2.emplace_back(Deletion{ChunkRange{BSON(kPattern << 4), BSON(kPattern << 7)}, later});
    when = rangeDeleter.add(std::move(ranges2));
    ASSERT(!when);

    std::list<Deletion> ranges3;
    ranges3.emplace_back(Deletion{ChunkRange{BSON(kPattern << 3), BSON(kPattern << 4)}, Date_t{}});
    when = rangeDeleter.add(std::move(ranges3));
    ASSERT(when);

    auto optNotifn1 = rangeDeleter.overlaps(ChunkRange{BSON(kPattern << 0), BSON(kPattern << 3)});
    ASSERT_TRUE(optNotifn1);
    auto& notifn1 = *optNotifn1;
    ASSERT_FALSE(notifn1.ready());

    auto optNotifn2 = rangeDeleter.overlaps(ChunkRange{BSON(kPattern << 4), BSON(kPattern << 7)});
    ASSERT_TRUE(optNotifn2);
    auto& notifn2 = *optNotifn2;
    ASSERT_FALSE(notifn2.ready());

    auto optNotifn3 = rangeDeleter.overlaps(ChunkRange{BSON(kPattern << 3), BSON(kPattern << 4)});
    ASSERT_TRUE(optNotifn3);
    auto& notifn3 = *optNotifn3;
    ASSERT_FALSE(notifn3.ready());

    // test op== on notifications
    ASSERT_TRUE(notifn1 == *optNotifn1);
    ASSERT_FALSE(notifn1 == *optNotifn2);
    ASSERT_TRUE(notifn1 != *optNotifn2);
    ASSERT_FALSE(notifn1 != *optNotifn1);

    // no op log entry yet
    ASSERT_EQUALS(0ULL, dbclient.count(kAdminSysVer.ns(), BSON(kPattern << "startRangeDeletion")));

    ASSERT_EQUALS(6ULL, dbclient.count(kNss.ns(), BSON(kPattern << LT << 7)));

    // catch range3, [3..4) only
    auto next1 = next(rangeDeleter, 100);
    ASSERT_TRUE(next1);
    ASSERT_EQUALS(*next1, Date_t{});

    // no op log entry for immediate deletions
    ASSERT_EQUALS(0ULL, dbclient.count(kAdminSysVer.ns(), BSON(kPattern << "startRangeDeletion")));

    // 3 gone
    ASSERT_EQUALS(5ULL, dbclient.count(kNss.ns(), BSON(kPattern << LT << 7)));
    ASSERT_EQUALS(2ULL, dbclient.count(kNss.ns(), BSON(kPattern << LT << 4)));

    ASSERT_FALSE(notifn1.ready());  // no trigger yet
    ASSERT_FALSE(notifn2.ready());  // no trigger yet
    ASSERT_FALSE(notifn3.ready());  // no trigger yet

    // this will find the [3..4) range empty, so pop the range and notify
    auto next2 = next(rangeDeleter, 100);
    ASSERT_TRUE(next2);
    ASSERT_EQUALS(*next2, Date_t{});

    // still no op log entry, because not delayed
    ASSERT_EQUALS(0ULL, dbclient.count(kAdminSysVer.ns(), BSON(kPattern << "startRangeDeletion")));

    // deleted 1, 5 left
    ASSERT_EQUALS(2ULL, dbclient.count(kNss.ns(), BSON(kPattern << LT << 4)));
    ASSERT_EQUALS(5ULL, dbclient.count(kNss.ns(), BSON(kPattern << LT << 10)));

    ASSERT_FALSE(notifn1.ready());  // no trigger yet
    ASSERT_FALSE(notifn2.ready());  // no trigger yet
    ASSERT_TRUE(notifn3.ready());   // triggered.
    ASSERT_OK(notifn3.waitStatus(operationContext()));

    // This will find the regular queue empty, but the [0..3) range in the delayed queue.
    // However, the time to delete them is now, so the range is moved to the regular queue.
    auto next3 = next(rangeDeleter, 100);
    ASSERT_TRUE(next3);
    ASSERT_EQUALS(*next3, Date_t{});

    ASSERT_FALSE(notifn1.ready());  // no trigger yet
    ASSERT_FALSE(notifn2.ready());  // no trigger yet

    // deleted 3, 3 left
    ASSERT_EQUALS(3ULL, dbclient.count(kNss.ns(), BSON(kPattern << LT << 10)));

    ASSERT_EQUALS(1ULL, dbclient.count(kAdminSysVer.ns(), BSON(kPattern << "startRangeDeletion")));
    // clang-format off
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << "startRangeDeletion" << "ns" << kNss.ns()
          << "epoch" << epoch() << "min" << BSON("_id" << 0) << "max" << BSON("_id" << 3)),
        dbclient.findOne(kAdminSysVer.ns(), QUERY("_id" << "startRangeDeletion")));
    // clang-format on

    // this will find the [0..3) range empty, so pop the range and notify
    auto next4 = next(rangeDeleter, 100);
    ASSERT_TRUE(next4);
    ASSERT_EQUALS(*next4, Date_t{});

    ASSERT_TRUE(notifn1.ready());
    ASSERT_OK(notifn1.waitStatus(operationContext()));
    ASSERT_FALSE(notifn2.ready());

    // op log entry unchanged
    // clang-format off
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << "startRangeDeletion" << "ns" << kNss.ns()
          << "epoch" << epoch() << "min" << BSON("_id" << 0) << "max" << BSON("_id" << 3)),
        dbclient.findOne(kAdminSysVer.ns(), QUERY("_id" << "startRangeDeletion")));
    // clang-format on

    // still 3 left
    ASSERT_EQUALS(3ULL, dbclient.count(kNss.ns(), BSON(kPattern << LT << 10)));

    // delete the remaining documents
    auto next5 = next(rangeDeleter, 100);
    ASSERT_TRUE(next5);
    ASSERT_EQUALS(*next5, Date_t{});

    ASSERT_FALSE(notifn2.ready());

    // Another delayed range, so logged
    // clang-format off
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << "startRangeDeletion" << "ns" << kNss.ns()
          << "epoch" << epoch() << "min" << BSON("_id" << 4) << "max" << BSON("_id" << 7)),
        dbclient.findOne(kAdminSysVer.ns(), QUERY("_id" << "startRangeDeletion")));
    // clang-format on

    // all docs gone
    ASSERT_EQUALS(0ULL, dbclient.count(kNss.ns(), BSON(kPattern << LT << 10)));

    // discover there are no more, pop range 2
    auto next6 = next(rangeDeleter, 100);
    ASSERT_TRUE(next6);
    ASSERT_EQUALS(*next6, Date_t{});

    ASSERT_TRUE(notifn2.ready());
    ASSERT_OK(notifn2.waitStatus(operationContext()));

    // discover there are no more ranges
    ASSERT_FALSE(next(rangeDeleter, 1));
}

}  // unnamed namespace
}  // namespace mongo
