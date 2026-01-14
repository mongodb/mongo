/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/idl/server_parameter_test_controller.h"
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
    RAIIServerParameterControllerForTest featureFlag{"featureFlagPathArrayness", true};
    auto indexA = BSON("v" << 2 << "name" << "a_1" << "key" << BSON("a" << 1) << "unique" << false);
    createIndexOnEmptyCollection(indexA);

    // Re-acquire collection after DDL op.
    const auto coll = acquireCollectionForRead(operationContext(), _kTestNss);
    const auto pathArrayness =
        CollectionQueryInfo::get(coll.getCollection().getCollectionPtr()).getPathArrayness();
    ASSERT_FALSE(pathArrayness.get()->isPathArray("a", &expCtx));
}

TEST_F(CollectionQueryInfoTest, PathArraynessUpdatesForCreateIndex) {
    ExpressionContextForTest expCtx = ExpressionContextForTest();
    RAIIServerParameterControllerForTest featureFlag{"featureFlagPathArrayness", true};
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
        ASSERT_FALSE(pathArrayness.get()->isPathArray("a", &expCtx));
    }
}

TEST_F(CollectionQueryInfoTest, PathArraynessUpdatesForMultikeyChange) {
    ExpressionContextForTest expCtx = ExpressionContextForTest();
    RAIIServerParameterControllerForTest featureFlag{"featureFlagPathArrayness", true};
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
        ASSERT_FALSE(pathArrayness.get()->isPathArray("a", &expCtx));
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
        ASSERT_TRUE(pathArrayness.get()->isPathArray("a", &expCtx));
        ASSERT_FALSE(pathArrayness.get()->isPathArray("b", &expCtx));
    }

    // Now make "b" multikey as well.
    const auto multikeyBDoc = BSON("_id" << 101 << "a" << 1 << "b" << BSON_ARRAY(1));
    insertDocuments(_kTestNss, {multikeyBDoc});

    {
        const auto coll = acquireCollectionForRead(operationContext(), _kTestNss);
        const auto pathArrayness =
            CollectionQueryInfo::get(coll.getCollection().getCollectionPtr()).getPathArrayness();
        // Both "a" and "b" are now mulitkey.
        ASSERT_TRUE(pathArrayness.get()->isPathArray("a", &expCtx));
        ASSERT_TRUE(pathArrayness.get()->isPathArray("b", &expCtx));
    }
}

TEST_F(CollectionQueryInfoTest, PathArraynessUpdatesForDropIndex) {
    ExpressionContextForTest expCtx = ExpressionContextForTest();
    RAIIServerParameterControllerForTest featureFlag{"featureFlagPathArrayness", true};
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
        ASSERT_TRUE(pathArrayness.get()->isPathArray("b", &expCtx));
    }


    auto indexB = BSON("b" << 1);
    ASSERT_OK(mongo::createIndex(operationContext(), _kTestNss.ns_forTest(), indexB));
    {
        const auto coll = acquireCollectionForRead(operationContext(), _kTestNss);
        const auto pathArrayness =
            CollectionQueryInfo::get(coll.getCollection().getCollectionPtr()).getPathArrayness();
        // "b" index does exist here.
        ASSERT_FALSE(pathArrayness.get()->isPathArray("b", &expCtx));
    }

    dropIndex(operationContext(), _kTestNss, "b_1");
    {
        const auto coll = acquireCollectionForRead(operationContext(), _kTestNss);
        const auto pathArrayness =
            CollectionQueryInfo::get(coll.getCollection().getCollectionPtr()).getPathArrayness();
        // "b" is dropped so we assume "b" is an array to be conservative.
        ASSERT_TRUE(pathArrayness.get()->isPathArray("b", &expCtx));
    }
}

TEST_F(CollectionQueryInfoTest, PathArraynessUpdatesForMultipleIndexes) {
    ExpressionContextForTest expCtx = ExpressionContextForTest();
    RAIIServerParameterControllerForTest featureFlag{"featureFlagPathArrayness", true};
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
        ASSERT_FALSE(pathArrayness.get()->isPathArray("a", &expCtx));
        ASSERT_TRUE(pathArrayness.get()->isPathArray("b", &expCtx));
    }
    // Create index on "b".
    auto indexB = BSON("b" << 1);
    ASSERT_OK(mongo::createIndex(operationContext(), _kTestNss.ns_forTest(), indexB));
    {
        const auto coll = acquireCollectionForRead(operationContext(), _kTestNss);
        const auto pathArrayness =
            CollectionQueryInfo::get(coll.getCollection().getCollectionPtr()).getPathArrayness();
        // "a" is not multi-key at this point.
        ASSERT_FALSE(pathArrayness.get()->isPathArray("a", &expCtx));
        // We created index on "b" and can now see that "b" is not multi-key.
        ASSERT_FALSE(pathArrayness.get()->isPathArray("b", &expCtx));
    }
}
}  // namespace
}  // namespace mongo

