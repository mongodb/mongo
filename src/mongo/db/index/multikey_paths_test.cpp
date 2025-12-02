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

#include "mongo/db/index/multikey_paths.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/db/client.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/curop.h"
#include "mongo/db/index_builds/index_build_test_helpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/shard_catalog/database.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/update/document_diff_applier.h"
#include "mongo/db/update/document_diff_calculator.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/shared_buffer.h"
#include "mongo/util/str.h"

#include <memory>
#include <string>
#include <vector>

namespace mongo {
namespace multikey_paths {
namespace {


const auto kIndexVersion = IndexDescriptor::IndexVersion::kV2;

/**
 * Fixture for testing correctness of multikey paths.
 *
 * Has helper functions for creating indexes and asserting that the multikey paths after performing
 * write operations are as expected.
 */
class MultikeyPathsTest : public ServiceContextMongoDTest {
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
        // TODO(SERVER-103400): Investigate usage validity of CollectionPtr::CollectionPtr_UNSAFE
        return CollectionPtr::CollectionPtr_UNSAFE(
            CollectionCatalog::get(_opCtx.get())->lookupCollectionByNamespace(_opCtx.get(), _nss));
    }


    Status createIndex(const CollectionPtr& collection, BSONObj indexSpec) {
        return createIndexFromSpec(_opCtx.get(), collection->ns().ns_forTest(), indexSpec);
    }

    void assertMultikeyPaths(const CollectionPtr& collection,
                             BSONObj keyPattern,
                             const MultikeyPaths& expectedMultikeyPaths) {
        const IndexCatalog* indexCatalog = collection->getIndexCatalog();
        std::vector<const IndexCatalogEntry*> indexes;
        indexCatalog->findIndexesByKeyPattern(
            _opCtx.get(), keyPattern, IndexCatalog::InclusionPolicy::kReady, &indexes);
        ASSERT_EQ(indexes.size(), 1U);
        auto ice = indexes[0];

        auto actualMultikeyPaths = ice->getMultikeyPaths(_opCtx.get(), collection);
        ASSERT_FALSE(actualMultikeyPaths.empty());
        const bool match = (expectedMultikeyPaths == actualMultikeyPaths);
        if (!match) {
            FAIL(std::string(str::stream()
                             << "Expected: " << dumpMultikeyPaths(expectedMultikeyPaths)
                             << ", Actual: " << dumpMultikeyPaths(actualMultikeyPaths)));
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
                BSON("name" << "a_1_b_1"
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
                BSON("name" << "a_1_b_1"
                            << "key" << keyPattern << "v" << static_cast<int>(kIndexVersion)))
        .transitional_ignore();

    assertMultikeyPaths(collection(), keyPattern, {{0U}, {0U}});
}

TEST_F(MultikeyPathsTest, PathsUpdatedOnDocumentInsert) {
    BSONObj keyPattern = BSON("a" << 1 << "b" << 1);
    createIndex(collection(),
                BSON("name" << "a_1_b_1"
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

    assertMultikeyPaths(*collection, keyPattern, {MultikeyComponents{}, {0U}});

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

    assertMultikeyPaths(*collection, keyPattern, {{0U}, {0U}});
}

TEST_F(MultikeyPathsTest, PathsUpdatedOnDocumentUpdate) {
    BSONObj keyPattern = BSON("a" << 1 << "b" << 1);
    createIndex(collection(),
                BSON("name" << "a_1_b_1"
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

    assertMultikeyPaths(*collection, keyPattern, {MultikeyComponents{}, MultikeyComponents{}});

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

    assertMultikeyPaths(*collection, keyPattern, {MultikeyComponents{}, {0U}});
}

TEST_F(MultikeyPathsTest, PathsUpdatedOnDocumentUpdateWithDamages) {
    BSONObj keyPattern = BSON("a" << 1 << "b" << 1);
    createIndex(collection(),
                BSON("name" << "a_1_b_1"
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

    assertMultikeyPaths(*collection, keyPattern, {MultikeyComponents{}, MultikeyComponents{}});

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

    assertMultikeyPaths(*collection, keyPattern, {MultikeyComponents{}, {0U}});
}

TEST_F(MultikeyPathsTest, PathsNotUpdatedOnDocumentDelete) {
    BSONObj keyPattern = BSON("a" << 1 << "b" << 1);
    createIndex(collection(),
                BSON("name" << "a_1_b_1"
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

    assertMultikeyPaths(*collection, keyPattern, {MultikeyComponents{}, {0U}});

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

    assertMultikeyPaths(*collection, keyPattern, {MultikeyComponents{}, {0U}});
}

TEST_F(MultikeyPathsTest, PathsUpdatedForMultipleIndexesOnDocumentInsert) {
    BSONObj keyPatternAB = BSON("a" << 1 << "b" << 1);
    createIndex(collection(),
                BSON("name" << "a_1_b_1"
                            << "key" << keyPatternAB << "v" << static_cast<int>(kIndexVersion)))
        .transitional_ignore();

    BSONObj keyPatternAC = BSON("a" << 1 << "c" << 1);
    createIndex(collection(),
                BSON("name" << "a_1_c_1"
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

    assertMultikeyPaths(*collection, keyPatternAB, {{0U}, MultikeyComponents{}});
    assertMultikeyPaths(*collection, keyPatternAC, {{0U}, MultikeyComponents{}});
}

TEST_F(MultikeyPathsTest, PrintEmptyPaths) {
    MultikeyPaths paths;
    ASSERT_EQ(toString(paths), "[]");
}

TEST_F(MultikeyPathsTest, PrintEmptySetPaths) {
    MultikeyPaths paths;
    paths.resize(1);
    ASSERT_EQ(toString(paths), "[{}]");
}

TEST_F(MultikeyPathsTest, PrintEmptySetsPaths) {
    MultikeyPaths paths;
    paths.resize(2);
    ASSERT_EQ(toString(paths), "[{},{}]");
}

TEST_F(MultikeyPathsTest, PrintNonEmptySetPaths) {
    MultikeyPaths paths;
    paths.resize(2);
    paths[1].insert(2);
    ASSERT_EQ(toString(paths), "[{},{2}]");
}

TEST_F(MultikeyPathsTest, PrintNonEmptySetsPaths) {
    MultikeyPaths paths;
    paths.resize(4);
    paths[1].insert(2);
    paths[3].insert(0);
    paths[3].insert(1);
    paths[3].insert(2);
    ASSERT_EQ(toString(paths), "[{},{2},{},{0,1,2}]");
}

TEST_F(MultikeyPathsTest, SerializeParseRoundTrip) {
    MultikeyPaths paths;
    ASSERT_EQ(parse(serialize({}, paths)), paths);

    paths.resize(1);
    ASSERT_EQ(parse(serialize(BSON("a" << 1), paths)), paths);

    paths.resize(2);
    ASSERT_EQ(parse(serialize(BSON("a" << 1 << "a.b.c" << 1), paths)), paths);

    paths[1].insert(2);
    ASSERT_EQ(parse(serialize(BSON("a" << 1 << "a.b.c" << 1), paths)), paths);

    paths.resize(4);
    paths[3].insert(0);
    paths[3].insert(1);
    paths[3].insert(2);
    ASSERT_EQ(parse(serialize(BSON("a" << 1 << "a.b.c" << 1 << "a.d.e.f" << 1 << "a.g.h.i.j" << 1),
                              paths)),
              paths);
}

TEST_F(MultikeyPathsTest, ParseInvalid) {
    ASSERT_EQ(parse(BSON("a" << 1)), ErrorCodes::BadValue);
    ASSERT_EQ(parse(BSON("a" << "str")), ErrorCodes::BadValue);

    std::string value(2049, 'a');
    BSONBinData binData{
        value.data(), static_cast<int>(value.length()), BinDataType::BinDataGeneral};
    ASSERT_EQ(parse(BSON("a" << binData)), ErrorCodes::BadValue);
}

}  // namespace
}  // namespace multikey_paths
}  // namespace mongo
