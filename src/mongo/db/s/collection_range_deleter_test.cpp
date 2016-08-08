/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/client/query.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/s/chunk_version.h"

namespace mongo {

using unittest::assertGet;

namespace {

const NamespaceString kNamespaceString = NamespaceString("foo", "bar");
const std::string kPattern = "_id";
const BSONObj kKeyPattern = BSON(kPattern << 1);
const ChunkRange kChunkRange = ChunkRange(BSON(kPattern << 0), BSON(kPattern << 10));

class CollectionRangeDeleterTest : public ServiceContextMongoDTest {
protected:
    ServiceContext::UniqueOperationContext _opCtx;
    MetadataManager* _migrationManager;
    std::unique_ptr<DBDirectClient> _dbDirectClient;
    std::unique_ptr<CollectionRangeDeleter> _rangeDeleter;

    OperationContext* operationContext();

private:
    void setUp() override;
    void tearDown() override;
};

void CollectionRangeDeleterTest::setUp() {
    ServiceContextMongoDTest::setUp();
    const repl::ReplSettings replSettings = {};
    repl::setGlobalReplicationCoordinator(new repl::ReplicationCoordinatorMock(replSettings));
    _opCtx = getServiceContext()->makeOperationContext(&cc());
    _dbDirectClient = stdx::make_unique<DBDirectClient>(operationContext());
    _rangeDeleter = stdx::make_unique<CollectionRangeDeleter>(kNamespaceString);
    AutoGetCollection autoColl(operationContext(), kNamespaceString, MODE_IX);
    auto collectionShardingState =
        CollectionShardingState::get(operationContext(), kNamespaceString);
    collectionShardingState->refreshMetadata(
        operationContext(),
        stdx::make_unique<CollectionMetadata>(kKeyPattern, ChunkVersion(1, 1, OID::gen())));
    _migrationManager = collectionShardingState->getMetadataManager();
}

void CollectionRangeDeleterTest::tearDown() {
    {
        AutoGetCollection autoColl(operationContext(), kNamespaceString, MODE_IX);
        ShardingState::get(operationContext())->resetMetadata(kNamespaceString.toString());
    }
    _rangeDeleter.reset();
    _dbDirectClient.reset();
    _opCtx.reset();
    ServiceContextMongoDTest::tearDown();
    repl::setGlobalReplicationCoordinator(nullptr);
}

OperationContext* CollectionRangeDeleterTest::operationContext() {
    invariant(_opCtx);
    return _opCtx.get();
}

// Tests the case that there is nothing in the database.
TEST_F(CollectionRangeDeleterTest, EmptyDatabase) {
    ASSERT_FALSE(_rangeDeleter->cleanupNextRange(operationContext(), 1));
}

// Tests the case that there is data, but it is not in a range to clean.
TEST_F(CollectionRangeDeleterTest, NoDataInGivenRangeToClean) {
    const BSONObj insertedDoc = BSON(kPattern << 25);

    _dbDirectClient->insert(kNamespaceString.toString(), insertedDoc);
    ASSERT_EQUALS(insertedDoc,
                  _dbDirectClient->findOne(kNamespaceString.toString(), QUERY(kPattern << 25)));

    _migrationManager->addRangeToClean(kChunkRange);
    ASSERT_FALSE(_rangeDeleter->cleanupNextRange(operationContext(), 1));

    ASSERT_EQUALS(insertedDoc,
                  _dbDirectClient->findOne(kNamespaceString.toString(), QUERY(kPattern << 25)));
}

// Tests the case that there is a single document within a range to clean.
TEST_F(CollectionRangeDeleterTest, OneDocumentInOneRangeToClean) {
    const BSONObj insertedDoc = BSON(kPattern << 5);

    _dbDirectClient->insert(kNamespaceString.toString(), BSON(kPattern << 5));
    ASSERT_EQUALS(insertedDoc,
                  _dbDirectClient->findOne(kNamespaceString.toString(), QUERY(kPattern << 5)));

    _migrationManager->addRangeToClean(kChunkRange);

    ASSERT_TRUE(_rangeDeleter->cleanupNextRange(operationContext(), 1));
    ASSERT_TRUE(
        _dbDirectClient->findOne(kNamespaceString.toString(), QUERY(kPattern << 5)).isEmpty());

    ASSERT_FALSE(_rangeDeleter->cleanupNextRange(operationContext(), 1));
}

// Tests the case that there are multiple documents within a range to clean.
TEST_F(CollectionRangeDeleterTest, MultipleDocumentsInOneRangeToClean) {
    _dbDirectClient->insert(kNamespaceString.toString(), BSON(kPattern << 1));
    _dbDirectClient->insert(kNamespaceString.toString(), BSON(kPattern << 2));
    _dbDirectClient->insert(kNamespaceString.toString(), BSON(kPattern << 3));
    ASSERT_EQUALS(3ULL,
                  _dbDirectClient->count(kNamespaceString.toString(), BSON(kPattern << LT << 5)));

    _migrationManager->addRangeToClean(kChunkRange);

    ASSERT_TRUE(_rangeDeleter->cleanupNextRange(operationContext(), 100));
    ASSERT_EQUALS(0ULL,
                  _dbDirectClient->count(kNamespaceString.toString(), BSON(kPattern << LT << 5)));

    ASSERT_FALSE(_rangeDeleter->cleanupNextRange(operationContext(), 100));
}

// Tests the case that there are multiple documents within a range to clean, and the range deleter
// has a max deletion rate of one document per run.
TEST_F(CollectionRangeDeleterTest, MultipleCleanupNextRangeCallsToCleanOneRange) {
    _dbDirectClient->insert(kNamespaceString.toString(), BSON(kPattern << 1));
    _dbDirectClient->insert(kNamespaceString.toString(), BSON(kPattern << 2));
    _dbDirectClient->insert(kNamespaceString.toString(), BSON(kPattern << 3));
    ASSERT_EQUALS(3ULL,
                  _dbDirectClient->count(kNamespaceString.toString(), BSON(kPattern << LT << 5)));

    _migrationManager->addRangeToClean(kChunkRange);

    ASSERT_TRUE(_rangeDeleter->cleanupNextRange(operationContext(), 1));
    ASSERT_EQUALS(2ULL,
                  _dbDirectClient->count(kNamespaceString.toString(), BSON(kPattern << LT << 5)));

    ASSERT_TRUE(_rangeDeleter->cleanupNextRange(operationContext(), 1));
    ASSERT_EQUALS(1ULL,
                  _dbDirectClient->count(kNamespaceString.toString(), BSON(kPattern << LT << 5)));

    ASSERT_TRUE(_rangeDeleter->cleanupNextRange(operationContext(), 1));
    ASSERT_EQUALS(0ULL,
                  _dbDirectClient->count(kNamespaceString.toString(), BSON(kPattern << LT << 5)));

    ASSERT_FALSE(_rangeDeleter->cleanupNextRange(operationContext(), 1));
}


// Tests the case that there are two ranges to clean, each containing multiple documents.
TEST_F(CollectionRangeDeleterTest, MultipleDocumentsInMultipleRangesToClean) {
    _dbDirectClient->insert(kNamespaceString.toString(), BSON(kPattern << 1));
    _dbDirectClient->insert(kNamespaceString.toString(), BSON(kPattern << 2));
    _dbDirectClient->insert(kNamespaceString.toString(), BSON(kPattern << 3));
    _dbDirectClient->insert(kNamespaceString.toString(), BSON(kPattern << 4));
    _dbDirectClient->insert(kNamespaceString.toString(), BSON(kPattern << 5));
    _dbDirectClient->insert(kNamespaceString.toString(), BSON(kPattern << 6));
    ASSERT_EQUALS(6ULL,
                  _dbDirectClient->count(kNamespaceString.toString(), BSON(kPattern << LT << 10)));

    const ChunkRange chunkRange1 = ChunkRange(BSON(kPattern << 0), BSON(kPattern << 4));
    const ChunkRange chunkRange2 = ChunkRange(BSON(kPattern << 4), BSON(kPattern << 7));
    _migrationManager->addRangeToClean(chunkRange1);
    _migrationManager->addRangeToClean(chunkRange2);

    ASSERT_TRUE(_rangeDeleter->cleanupNextRange(operationContext(), 100));
    ASSERT_EQUALS(0ULL,
                  _dbDirectClient->count(kNamespaceString.toString(), BSON(kPattern << LT << 4)));
    ASSERT_EQUALS(3ULL,
                  _dbDirectClient->count(kNamespaceString.toString(), BSON(kPattern << LT << 10)));

    ASSERT_TRUE(_rangeDeleter->cleanupNextRange(operationContext(), 100));
    ASSERT_TRUE(_rangeDeleter->cleanupNextRange(operationContext(), 100));
    ASSERT_EQUALS(0ULL,
                  _dbDirectClient->count(kNamespaceString.toString(), BSON(kPattern << LT << 10)));

    ASSERT_FALSE(_rangeDeleter->cleanupNextRange(operationContext(), 1));
}

// Tests the case that there is a range to clean, and halfway through a deletion a chunk
// within the range is received
TEST_F(CollectionRangeDeleterTest, MultipleCallstoCleanupNextRangeWithChunkReceive) {
    _dbDirectClient->insert(kNamespaceString.toString(), BSON(kPattern << 1));
    _dbDirectClient->insert(kNamespaceString.toString(), BSON(kPattern << 2));
    _dbDirectClient->insert(kNamespaceString.toString(), BSON(kPattern << 3));
    _dbDirectClient->insert(kNamespaceString.toString(), BSON(kPattern << 4));
    const BSONObj insertedDoc5 = BSON(kPattern << 5);  // not to be deleted
    _dbDirectClient->insert(kNamespaceString.toString(), insertedDoc5);
    ASSERT_EQUALS(5ULL,
                  _dbDirectClient->count(kNamespaceString.toString(), BSON(kPattern << LT << 10)));

    _migrationManager->addRangeToClean(kChunkRange);

    ASSERT_TRUE(_rangeDeleter->cleanupNextRange(operationContext(), 2));
    ASSERT_EQUALS(3ULL,
                  _dbDirectClient->count(kNamespaceString.toString(), BSON(kPattern << LT << 10)));

    _migrationManager->beginReceive(ChunkRange(BSON(kPattern << 5), BSON(kPattern << 6)));

    // insertedDoc5 is no longer eligible for deletion

    ASSERT_TRUE(_rangeDeleter->cleanupNextRange(operationContext(), 2));
    ASSERT_EQUALS(1ULL,
                  _dbDirectClient->count(kNamespaceString.toString(), BSON(kPattern << LT << 10)));

    ASSERT_TRUE(_rangeDeleter->cleanupNextRange(operationContext(), 2));
    ASSERT_EQUALS(1ULL,
                  _dbDirectClient->count(kNamespaceString.toString(), BSON(kPattern << LT << 10)));

    ASSERT_FALSE(_rangeDeleter->cleanupNextRange(operationContext(), 2));

    ASSERT_EQUALS(
        insertedDoc5,
        _dbDirectClient->findOne(kNamespaceString.toString(), QUERY(kPattern << GT << 0)));
}

TEST_F(CollectionRangeDeleterTest, CalltoCleanupNextRangeWithChunkReceive) {
    _dbDirectClient->insert(kNamespaceString.toString(), BSON(kPattern << 1));
    _dbDirectClient->insert(kNamespaceString.toString(), BSON(kPattern << 2));
    _dbDirectClient->insert(kNamespaceString.toString(), BSON(kPattern << 3));
    _dbDirectClient->insert(kNamespaceString.toString(), BSON(kPattern << 4));
    ASSERT_EQUALS(4ULL,
                  _dbDirectClient->count(kNamespaceString.toString(), BSON(kPattern << LT << 10)));

    _migrationManager->addRangeToClean(kChunkRange);

    ASSERT_TRUE(_rangeDeleter->cleanupNextRange(operationContext(), 2));
    ASSERT_EQUALS(2ULL,
                  _dbDirectClient->count(kNamespaceString.toString(), BSON(kPattern << LT << 10)));

    _migrationManager->beginReceive(kChunkRange);

    ASSERT_FALSE(_rangeDeleter->cleanupNextRange(operationContext(), 2));

    ASSERT_EQUALS(2ULL,
                  _dbDirectClient->count(kNamespaceString.toString(), BSON(kPattern << LT << 10)));
}

}  // unnamed namespace

}  // namespace mongo
