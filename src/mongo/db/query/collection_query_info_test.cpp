// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/collection_query_info.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/index_builds/index_build_test_helpers.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_test_utils.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
namespace mongo {
namespace {
class CollectionQueryInfoTest : public CatalogTestFixture {
public:
    void setUp() override {
        _kTestNss = NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
        CatalogTestFixture::setUp();
    }

    void createIndexOnEmptyCollection(const BSONObj& spec) {
        ASSERT_OK(storageInterface()->createCollection(
            operationContext(), _kTestNss, CollectionOptions()));
        WriteUnitOfWork wuow(operationContext());
        auto coll = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest(_kTestNss,
                                         PlacementConcern(boost::none, ShardVersion::UNTRACKED()),
                                         repl::ReadConcernArgs::get(operationContext()),
                                         AcquisitionPrerequisites::kWrite),
            MODE_X);
        CollectionWriter collectionWriter(operationContext(), &coll);
        IndexBuildsCoordinator::createIndexesOnEmptyCollection(
            operationContext(), collectionWriter, {spec}, /* fromMigrate */ false);
        wuow.commit();
    }

    void insertDocuments(const NamespaceString& nss, const std::vector<BSONObj> docs) {
        std::vector<InsertStatement> inserts{docs.begin(), docs.end()};
        const auto batchSize = 50000;

        const auto coll = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest(nss,
                                         PlacementConcern(boost::none, ShardVersion::UNTRACKED()),
                                         repl::ReadConcernArgs::get(operationContext()),
                                         AcquisitionPrerequisites::kWrite),
            MODE_IX);
        {
            size_t currentInsertion = 0;
            while (currentInsertion < inserts.size()) {
                WriteUnitOfWork wuow{operationContext()};

                int insertionsBeforeCommit = 0;
                while (true) {
                    ASSERT_OK(collection_internal::insertDocument(operationContext(),
                                                                  coll.getCollectionPtr(),
                                                                  inserts[currentInsertion],
                                                                  nullptr /* opDebug */));
                    insertionsBeforeCommit++;
                    currentInsertion++;

                    if (insertionsBeforeCommit > batchSize || currentInsertion == inserts.size()) {
                        insertionsBeforeCommit = 0;
                        break;
                    }
                }
                wuow.commit();
            }
        }
    }


    void dropIndex(OperationContext* opCtx,
                   const NamespaceString& nss,
                   const std::string& idxName) {
        AutoGetDb autoDb(opCtx, nss.dbName(), MODE_IX);
        Lock::CollectionLock collLock(opCtx, nss, MODE_X);
        WriteUnitOfWork wuow{opCtx};
        CollectionWriter coll{opCtx, nss};
        auto writableEntry =
            coll.getWritableCollection(opCtx)->getIndexCatalog()->getWritableEntryByName(opCtx,
                                                                                         idxName);
        ASSERT(writableEntry);
        ASSERT_OK(coll.getWritableCollection(opCtx)->getIndexCatalog()->dropIndexEntry(
            opCtx, coll.getWritableCollection(opCtx), writableEntry));
        wuow.commit();
    }


    NamespaceString _kTestNss;
};

CollectionOrViewAcquisition acquireCollectionForRead(OperationContext* opCtx,
                                                     const NamespaceString& nss) {
    return acquireCollectionOrView(
        opCtx,
        CollectionOrViewAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kWrite),
        LockMode::MODE_IX);
}

TEST_F(CollectionQueryInfoTest, PathArraynessUpdatesForCreateIndexOnEmptyCollection) {
    ExpressionContextForTest expCtx = ExpressionContextForTest();
    unittest::ServerParameterGuard featureFlag{"featureFlagPathArrayness", true};
    auto indexA = BSON("v" << 2 << "name" << "a_1" << "key" << BSON("a" << 1) << "unique" << false);
    createIndexOnEmptyCollection(indexA);

    // Re-acquire collection after DDL op.
    const auto coll = acquireCollectionForRead(operationContext(), _kTestNss);
    const auto pathArrayness =
        CollectionQueryInfo::get(coll.getCollection().getCollectionPtr()).getPathArrayness();
    ASSERT_FALSE(pathArrayness.get()->canPathBeArray("a", &expCtx));
}

TEST_F(CollectionQueryInfoTest, PathArraynessUpdatesForCreateIndex) {
    ExpressionContextForTest expCtx = ExpressionContextForTest();
    unittest::ServerParameterGuard featureFlag{"featureFlagPathArrayness", true};
    std::vector<BSONObj> docs;
    for (int i = 0; i < 100; ++i) {
        docs.push_back(BSON("_id" << i << "a" << i));
    }
    ce::createCollAndInsertDocuments(operationContext(), _kTestNss, docs);

    auto indexA = BSON("a" << 1);
    ASSERT_OK(mongo::createIndex(operationContext(), _kTestNss.ns_forTest(), indexA));

    {
        const auto coll = acquireCollectionForRead(operationContext(), _kTestNss);
        const auto pathArrayness =
            CollectionQueryInfo::get(coll.getCollection().getCollectionPtr()).getPathArrayness();
        // "a" is not multi-key at this point.
        ASSERT_FALSE(pathArrayness.get()->canPathBeArray("a", &expCtx));
    }
}

TEST_F(CollectionQueryInfoTest, PathArraynessUpdatesForMultikeyChange) {
    ExpressionContextForTest expCtx = ExpressionContextForTest();
    unittest::ServerParameterGuard featureFlag{"featureFlagPathArrayness", true};
    std::vector<BSONObj> docs;
    for (int i = 0; i < 100; ++i) {
        docs.push_back(BSON("_id" << i << "a" << i << "b" << 0));
    }
    ce::createCollAndInsertDocuments(operationContext(), _kTestNss, docs);

    // Create index on "a" and "b" so we have multikey information.
    auto indexAB = BSON("a" << 1 << "b" << 1);
    ASSERT_OK(mongo::createIndex(operationContext(), _kTestNss.ns_forTest(), indexAB));

    {
        const auto coll = acquireCollectionForRead(operationContext(), _kTestNss);
        const auto pathArrayness =
            CollectionQueryInfo::get(coll.getCollection().getCollectionPtr()).getPathArrayness();
        // "a" is not multi-key at this point.
        ASSERT_FALSE(pathArrayness.get()->canPathBeArray("a", &expCtx));
    }

    // Make "a" multikey but inserting a doc where "a" is multikey.
    const auto multikeyADoc = BSON("_id" << 100 << "a" << BSON_ARRAY(1));
    insertDocuments(_kTestNss, {multikeyADoc});
    {
        const auto coll = acquireCollectionForRead(operationContext(), _kTestNss);
        const auto pathArrayness =
            CollectionQueryInfo::get(coll.getCollection().getCollectionPtr()).getPathArrayness();
        pathArrayness->visualizeTrie_forTest();
        // "a" is now mulitkey.
        ASSERT_TRUE(pathArrayness.get()->canPathBeArray("a", &expCtx));
        ASSERT_FALSE(pathArrayness.get()->canPathBeArray("b", &expCtx));
    }

    // Now make "b" multikey as well.
    const auto multikeyBDoc = BSON("_id" << 101 << "a" << 1 << "b" << BSON_ARRAY(1));
    insertDocuments(_kTestNss, {multikeyBDoc});

    {
        const auto coll = acquireCollectionForRead(operationContext(), _kTestNss);
        const auto pathArrayness =
            CollectionQueryInfo::get(coll.getCollection().getCollectionPtr()).getPathArrayness();
        // Both "a" and "b" are now mulitkey.
        ASSERT_TRUE(pathArrayness.get()->canPathBeArray("a", &expCtx));
        ASSERT_TRUE(pathArrayness.get()->canPathBeArray("b", &expCtx));
    }
}

TEST_F(CollectionQueryInfoTest, PathArraynessUpdatesForDropIndex) {
    ExpressionContextForTest expCtx = ExpressionContextForTest();
    unittest::ServerParameterGuard featureFlag{"featureFlagPathArrayness", true};
    std::vector<BSONObj> docs;
    for (int i = 0; i < 100; ++i) {
        docs.push_back(BSON("_id" << i << "b" << i));
    }
    ce::createCollAndInsertDocuments(operationContext(), _kTestNss, docs);

    {
        const auto coll = acquireCollectionForRead(operationContext(), _kTestNss);
        const auto pathArrayness =
            CollectionQueryInfo::get(coll.getCollection().getCollectionPtr()).getPathArrayness();
        // "b" index does not exist yet.
        ASSERT_TRUE(pathArrayness.get()->canPathBeArray("b", &expCtx));
    }


    auto indexB = BSON("b" << 1);
    ASSERT_OK(mongo::createIndex(operationContext(), _kTestNss.ns_forTest(), indexB));
    {
        const auto coll = acquireCollectionForRead(operationContext(), _kTestNss);
        const auto pathArrayness =
            CollectionQueryInfo::get(coll.getCollection().getCollectionPtr()).getPathArrayness();
        // "b" index does exist here.
        ASSERT_FALSE(pathArrayness.get()->canPathBeArray("b", &expCtx));
    }

    dropIndex(operationContext(), _kTestNss, "b_1");
    {
        const auto coll = acquireCollectionForRead(operationContext(), _kTestNss);
        const auto pathArrayness =
            CollectionQueryInfo::get(coll.getCollection().getCollectionPtr()).getPathArrayness();
        // "b" is dropped so we assume "b" is an array to be conservative.
        ASSERT_TRUE(pathArrayness.get()->canPathBeArray("b", &expCtx));
    }
}

TEST_F(CollectionQueryInfoTest, PathArraynessUpdatesForMultipleIndexes) {
    ExpressionContextForTest expCtx = ExpressionContextForTest();
    unittest::ServerParameterGuard featureFlag{"featureFlagPathArrayness", true};
    std::vector<BSONObj> docs;
    for (int i = 0; i < 100; ++i) {
        docs.push_back(BSON("_id" << i << "a" << i << "b" << i));
    }
    ce::createCollAndInsertDocuments(operationContext(), _kTestNss, docs);

    auto indexA = BSON("a" << 1);
    ASSERT_OK(mongo::createIndex(operationContext(), _kTestNss.ns_forTest(), indexA));

    {
        const auto coll = acquireCollectionForRead(operationContext(), _kTestNss);
        const auto pathArrayness =
            CollectionQueryInfo::get(coll.getCollection().getCollectionPtr()).getPathArrayness();
        // "a" is not multi-key at this point.
        ASSERT_FALSE(pathArrayness.get()->canPathBeArray("a", &expCtx));
        ASSERT_TRUE(pathArrayness.get()->canPathBeArray("b", &expCtx));
    }
    // Create index on "b".
    auto indexB = BSON("b" << 1);
    ASSERT_OK(mongo::createIndex(operationContext(), _kTestNss.ns_forTest(), indexB));
    {
        const auto coll = acquireCollectionForRead(operationContext(), _kTestNss);
        const auto pathArrayness =
            CollectionQueryInfo::get(coll.getCollection().getCollectionPtr()).getPathArrayness();
        // "a" is not multi-key at this point.
        ASSERT_FALSE(pathArrayness.get()->canPathBeArray("a", &expCtx));
        // We created index on "b" and can now see that "b" is not multi-key.
        ASSERT_FALSE(pathArrayness.get()->canPathBeArray("b", &expCtx));
    }
}
TEST_F(CollectionQueryInfoTest, PathArraynessUpdateForSetMultikeyIncrementsEpoch) {
    unittest::ServerParameterGuard featureFlag{"featureFlagPathArrayness", true};
    std::vector<BSONObj> docs;
    for (int i = 0; i < 10; ++i) {
        docs.push_back(BSON("_id" << i << "a" << i));
    }
    ce::createCollAndInsertDocuments(operationContext(), _kTestNss, docs);
    ASSERT_OK(mongo::createIndex(operationContext(), _kTestNss.ns_forTest(), BSON("a" << 1)));

    uint64_t epochBefore;
    {
        const auto coll = acquireCollectionForRead(operationContext(), _kTestNss);
        epochBefore = CollectionQueryInfo::get(coll.getCollection().getCollectionPtr())
                          .getPathArrayness()
                          ->epoch();
    }

    insertDocuments(_kTestNss, {BSON("_id" << 100 << "a" << BSON_ARRAY(1 << 2))});

    {
        const auto coll = acquireCollectionForRead(operationContext(), _kTestNss);
        ASSERT_EQ(CollectionQueryInfo::get(coll.getCollection().getCollectionPtr())
                      .getPathArrayness()
                      ->epoch(),
                  epochBefore + 1);
    }
}
}  // namespace
}  // namespace mongo

