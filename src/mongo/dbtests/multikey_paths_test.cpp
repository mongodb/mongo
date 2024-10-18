/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include <boost/container/flat_set.hpp>
#include <boost/container/small_vector.hpp>
#include <boost/container/vector.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/update/document_diff_applier.h"
#include "mongo/db/update/document_diff_calculator.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util_core.h"
#include "mongo/util/shared_buffer.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

const auto kIndexVersion = IndexDescriptor::IndexVersion::kV2;

/**
 * Fixture for testing correctness of multikey paths.
 *
 * Has helper functions for creating indexes and asserting that the multikey paths after performing
 * write operations are as expected.
 */
class MultikeyPathsTest : public unittest::Test {
public:
    MultikeyPathsTest()
        : _nss(NamespaceString::createNamespaceString_forTest("unittests.multikey_paths")) {}

    void setUp() final {
        AutoGetCollection autoColl(_opCtx.get(), _nss, MODE_IX);
        auto db = autoColl.ensureDbExists(_opCtx.get());

        WriteUnitOfWork wuow(_opCtx.get());
        ASSERT(db->createCollection(_opCtx.get(), _nss));
        wuow.commit();
    }

    void tearDown() final {
        AutoGetCollection autoColl(_opCtx.get(), _nss, MODE_X);
        if (!autoColl) {
            return;
        }

        auto db = autoColl.getDb();

        WriteUnitOfWork wuow(_opCtx.get());
        ASSERT_OK(db->dropCollection(_opCtx.get(), _nss));
        wuow.commit();
    }

    // Helper to refetch the Collection from the catalog in order to see any changes made to it
    CollectionPtr collection() const {
        return CollectionPtr(
            CollectionCatalog::get(_opCtx.get())->lookupCollectionByNamespace(_opCtx.get(), _nss));
    }


    Status createIndex(const CollectionPtr& collection, BSONObj indexSpec) {
        return dbtests::createIndexFromSpec(_opCtx.get(), collection->ns().ns_forTest(), indexSpec);
    }

    void assertMultikeyPaths(const CollectionPtr& collection,
                             BSONObj keyPattern,
                             const MultikeyPaths& expectedMultikeyPaths) {
        const IndexCatalog* indexCatalog = collection->getIndexCatalog();
        std::vector<const IndexDescriptor*> indexes;
        indexCatalog->findIndexesByKeyPattern(
            _opCtx.get(), keyPattern, IndexCatalog::InclusionPolicy::kReady, &indexes);
        ASSERT_EQ(indexes.size(), 1U);
        auto desc = indexes[0];
        const IndexCatalogEntry* ice = indexCatalog->getEntry(desc);

        auto actualMultikeyPaths = ice->getMultikeyPaths(_opCtx.get(), collection);
        ASSERT_FALSE(actualMultikeyPaths.empty());
        const bool match = (expectedMultikeyPaths == actualMultikeyPaths);
        if (!match) {
            FAIL(str::stream() << "Expected: " << dumpMultikeyPaths(expectedMultikeyPaths)
                               << ", Actual: " << dumpMultikeyPaths(actualMultikeyPaths));
        }
        ASSERT_TRUE(match);
    }

protected:
    const ServiceContext::UniqueOperationContext _opCtx = cc().makeOperationContext();
    const NamespaceString _nss;

private:
    std::string dumpMultikeyPaths(const MultikeyPaths& multikeyPaths) {
        std::stringstream ss;

        ss << "[ ";
        for (const auto& multikeyComponents : multikeyPaths) {
            ss << "[ ";
            for (const auto& multikeyComponent : multikeyComponents) {
                ss << multikeyComponent << " ";
            }
            ss << "] ";
        }
        ss << "]";

        return ss.str();
    }
};

TEST_F(MultikeyPathsTest, PathsUpdatedOnIndexCreation) {
    {
        AutoGetCollection collection(_opCtx.get(), _nss, MODE_X);
        invariant(collection);

        WriteUnitOfWork wuow(_opCtx.get());
        OpDebug* const nullOpDebug = nullptr;
        ASSERT_OK(collection_internal::insertDocument(
            _opCtx.get(),
            *collection,
            InsertStatement(BSON("_id" << 0 << "a" << 5 << "b" << BSON_ARRAY(1 << 2 << 3))),
            nullOpDebug));
        wuow.commit();
    }

    BSONObj keyPattern = BSON("a" << 1 << "b" << 1);
    createIndex(collection(),
                BSON("name"
                     << "a_1_b_1"
                     << "key" << keyPattern << "v" << static_cast<int>(kIndexVersion)))
        .transitional_ignore();

    assertMultikeyPaths(collection(), keyPattern, {MultikeyComponents{}, {0U}});
}

TEST_F(MultikeyPathsTest, PathsUpdatedOnIndexCreationWithMultipleDocuments) {
    {
        AutoGetCollection collection(_opCtx.get(), _nss, MODE_X);
        invariant(collection);

        WriteUnitOfWork wuow(_opCtx.get());
        OpDebug* const nullOpDebug = nullptr;
        ASSERT_OK(collection_internal::insertDocument(
            _opCtx.get(),
            *collection,
            InsertStatement(BSON("_id" << 0 << "a" << 5 << "b" << BSON_ARRAY(1 << 2 << 3))),
            nullOpDebug));
        ASSERT_OK(collection_internal::insertDocument(
            _opCtx.get(),
            *collection,
            InsertStatement(BSON("_id" << 1 << "a" << BSON_ARRAY(1 << 2 << 3) << "b" << 5)),
            nullOpDebug));
        wuow.commit();
    }

    BSONObj keyPattern = BSON("a" << 1 << "b" << 1);
    createIndex(collection(),
                BSON("name"
                     << "a_1_b_1"
                     << "key" << keyPattern << "v" << static_cast<int>(kIndexVersion)))
        .transitional_ignore();

    assertMultikeyPaths(collection(), keyPattern, {{0U}, {0U}});
}

TEST_F(MultikeyPathsTest, PathsUpdatedOnDocumentInsert) {
    BSONObj keyPattern = BSON("a" << 1 << "b" << 1);
    createIndex(collection(),
                BSON("name"
                     << "a_1_b_1"
                     << "key" << keyPattern << "v" << static_cast<int>(kIndexVersion)))
        .transitional_ignore();

    AutoGetCollection collection(_opCtx.get(), _nss, MODE_IX);

    {
        WriteUnitOfWork wuow(_opCtx.get());
        OpDebug* const nullOpDebug = nullptr;
        ASSERT_OK(collection_internal::insertDocument(
            _opCtx.get(),
            *collection,
            InsertStatement(BSON("_id" << 0 << "a" << 5 << "b" << BSON_ARRAY(1 << 2 << 3))),
            nullOpDebug));
        wuow.commit();
    }

    assertMultikeyPaths(collection.getCollection(), keyPattern, {MultikeyComponents{}, {0U}});

    {
        WriteUnitOfWork wuow(_opCtx.get());
        OpDebug* const nullOpDebug = nullptr;
        ASSERT_OK(collection_internal::insertDocument(
            _opCtx.get(),
            *collection,
            InsertStatement(BSON("_id" << 1 << "a" << BSON_ARRAY(1 << 2 << 3) << "b" << 5)),
            nullOpDebug));
        wuow.commit();
    }

    assertMultikeyPaths(collection.getCollection(), keyPattern, {{0U}, {0U}});
}

TEST_F(MultikeyPathsTest, PathsUpdatedOnDocumentUpdate) {
    BSONObj keyPattern = BSON("a" << 1 << "b" << 1);
    createIndex(collection(),
                BSON("name"
                     << "a_1_b_1"
                     << "key" << keyPattern << "v" << static_cast<int>(kIndexVersion)))
        .transitional_ignore();
    AutoGetCollection collection(_opCtx.get(), _nss, MODE_IX);

    {
        WriteUnitOfWork wuow(_opCtx.get());
        OpDebug* const nullOpDebug = nullptr;
        ASSERT_OK(collection_internal::insertDocument(
            _opCtx.get(), *collection, InsertStatement(BSON("_id" << 0 << "a" << 5)), nullOpDebug));
        wuow.commit();
    }

    assertMultikeyPaths(
        collection.getCollection(), keyPattern, {MultikeyComponents{}, MultikeyComponents{}});

    {
        auto cursor = collection->getCursor(_opCtx.get());
        auto record = cursor->next();
        invariant(record);

        auto oldDoc = collection->docFor(_opCtx.get(), record->id);
        {
            WriteUnitOfWork wuow(_opCtx.get());
            OpDebug* opDebug = nullptr;
            CollectionUpdateArgs args{oldDoc.value()};
            collection_internal::updateDocument(
                _opCtx.get(),
                *collection,
                record->id,
                oldDoc,
                BSON("_id" << 0 << "a" << 5 << "b" << BSON_ARRAY(1 << 2 << 3)),
                collection_internal::kUpdateAllIndexes,
                nullptr /* indexesAffected */,
                opDebug,
                &args);
            wuow.commit();
        }
    }

    assertMultikeyPaths(collection.getCollection(), keyPattern, {MultikeyComponents{}, {0U}});
}

TEST_F(MultikeyPathsTest, PathsUpdatedOnDocumentUpdateWithDamages) {
    BSONObj keyPattern = BSON("a" << 1 << "b" << 1);
    createIndex(collection(),
                BSON("name"
                     << "a_1_b_1"
                     << "key" << keyPattern << "v" << static_cast<int>(kIndexVersion)))
        .transitional_ignore();
    AutoGetCollection collection(_opCtx.get(), _nss, MODE_IX);

    auto oldDoc = BSON("_id" << 0 << "a" << 5);
    {
        WriteUnitOfWork wuow(_opCtx.get());
        OpDebug* const nullOpDebug = nullptr;
        ASSERT_OK(collection_internal::insertDocument(
            _opCtx.get(), *collection, InsertStatement(oldDoc), nullOpDebug));
        wuow.commit();
    }

    assertMultikeyPaths(
        collection.getCollection(), keyPattern, {MultikeyComponents{}, MultikeyComponents{}});

    {
        auto cursor = collection->getCursor(_opCtx.get());
        auto record = cursor->next();
        invariant(record);

        auto oldDoc = collection->docFor(_opCtx.get(), record->id);
        auto newDoc = BSON("_id" << 0 << "a" << 5 << "b" << BSON_ARRAY(1 << 2 << 3));
        auto diffResult = doc_diff::computeOplogDiff(oldDoc.value(), newDoc, 0);
        auto damagesOutput = doc_diff::computeDamages(oldDoc.value(), *diffResult, false);
        {
            WriteUnitOfWork wuow(_opCtx.get());
            OpDebug* opDebug = nullptr;
            CollectionUpdateArgs args{oldDoc.value()};
            auto newDocResult = collection_internal::updateDocumentWithDamages(
                _opCtx.get(),
                *collection,
                record->id,
                oldDoc,
                damagesOutput.damageSource.get(),
                damagesOutput.damages,
                collection_internal::kUpdateAllIndexes,
                nullptr /* indexesAffected */,
                opDebug,
                &args);
            ASSERT_TRUE(newDocResult.getValue().woCompare(newDoc) == 0);
            ASSERT_TRUE(newDocResult.isOK());
            wuow.commit();
        }
    }

    assertMultikeyPaths(collection.getCollection(), keyPattern, {MultikeyComponents{}, {0U}});
}

TEST_F(MultikeyPathsTest, PathsNotUpdatedOnDocumentDelete) {
    BSONObj keyPattern = BSON("a" << 1 << "b" << 1);
    createIndex(collection(),
                BSON("name"
                     << "a_1_b_1"
                     << "key" << keyPattern << "v" << static_cast<int>(kIndexVersion)))
        .transitional_ignore();
    AutoGetCollection collection(_opCtx.get(), _nss, MODE_IX);

    {
        WriteUnitOfWork wuow(_opCtx.get());
        OpDebug* const nullOpDebug = nullptr;
        ASSERT_OK(collection_internal::insertDocument(
            _opCtx.get(),
            *collection,
            InsertStatement(BSON("_id" << 0 << "a" << 5 << "b" << BSON_ARRAY(1 << 2 << 3))),
            nullOpDebug));
        wuow.commit();
    }

    assertMultikeyPaths(collection.getCollection(), keyPattern, {MultikeyComponents{}, {0U}});

    {
        auto cursor = collection->getCursor(_opCtx.get());
        auto record = cursor->next();
        invariant(record);

        {
            WriteUnitOfWork wuow(_opCtx.get());
            OpDebug* const nullOpDebug = nullptr;
            collection_internal::deleteDocument(
                _opCtx.get(), *collection, kUninitializedStmtId, record->id, nullOpDebug);
            wuow.commit();
        }
    }

    assertMultikeyPaths(collection.getCollection(), keyPattern, {MultikeyComponents{}, {0U}});
}

TEST_F(MultikeyPathsTest, PathsUpdatedForMultipleIndexesOnDocumentInsert) {
    BSONObj keyPatternAB = BSON("a" << 1 << "b" << 1);
    createIndex(collection(),
                BSON("name"
                     << "a_1_b_1"
                     << "key" << keyPatternAB << "v" << static_cast<int>(kIndexVersion)))
        .transitional_ignore();

    BSONObj keyPatternAC = BSON("a" << 1 << "c" << 1);
    createIndex(collection(),
                BSON("name"
                     << "a_1_c_1"
                     << "key" << keyPatternAC << "v" << static_cast<int>(kIndexVersion)))
        .transitional_ignore();

    AutoGetCollection collection(_opCtx.get(), _nss, MODE_IX);
    {
        WriteUnitOfWork wuow(_opCtx.get());
        OpDebug* const nullOpDebug = nullptr;
        ASSERT_OK(collection_internal::insertDocument(
            _opCtx.get(),
            *collection,
            InsertStatement(
                BSON("_id" << 0 << "a" << BSON_ARRAY(1 << 2 << 3) << "b" << 5 << "c" << 8)),
            nullOpDebug));
        wuow.commit();
    }

    assertMultikeyPaths(collection.getCollection(), keyPatternAB, {{0U}, MultikeyComponents{}});
    assertMultikeyPaths(collection.getCollection(), keyPatternAC, {{0U}, MultikeyComponents{}});
}

}  // namespace
}  // namespace mongo
