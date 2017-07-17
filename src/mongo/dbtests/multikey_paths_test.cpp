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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <iostream>
#include <string>

#include "mongo/db/catalog/index_create.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/service_context.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/mongoutils/str.h"

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
    MultikeyPathsTest() : _nss("unittests.multikey_paths") {}

    void setUp() final {
        AutoGetOrCreateDb autoDb(_opCtx.get(), _nss.db(), MODE_X);
        Database* database = autoDb.getDb();
        {
            WriteUnitOfWork wuow(_opCtx.get());
            ASSERT(database->createCollection(_opCtx.get(), _nss.ns()));
            wuow.commit();
        }
    }

    void tearDown() final {
        AutoGetDb autoDb(_opCtx.get(), _nss.db(), MODE_X);
        Database* database = autoDb.getDb();
        if (database) {
            WriteUnitOfWork wuow(_opCtx.get());
            ASSERT_OK(database->dropCollection(_opCtx.get(), _nss.ns()));
            wuow.commit();
        }
    }

    Status createIndex(Collection* collection, BSONObj indexSpec) {
        return dbtests::createIndexFromSpec(_opCtx.get(), collection->ns().ns(), indexSpec);
    }

    void assertMultikeyPaths(Collection* collection,
                             BSONObj keyPattern,
                             const MultikeyPaths& expectedMultikeyPaths) {
        IndexCatalog* indexCatalog = collection->getIndexCatalog();
        std::vector<IndexDescriptor*> indexes;
        indexCatalog->findIndexesByKeyPattern(_opCtx.get(), keyPattern, false, &indexes);
        ASSERT_EQ(indexes.size(), 1U);
        IndexDescriptor* desc = indexes[0];
        const IndexCatalogEntry* ice = indexCatalog->getEntry(desc);

        auto actualMultikeyPaths = ice->getMultikeyPaths(_opCtx.get());
        if (storageEngineSupportsPathLevelMultikeyTracking()) {
            ASSERT_FALSE(actualMultikeyPaths.empty());
            const bool match = (expectedMultikeyPaths == actualMultikeyPaths);
            if (!match) {
                FAIL(str::stream() << "Expected: " << dumpMultikeyPaths(expectedMultikeyPaths)
                                   << ", Actual: "
                                   << dumpMultikeyPaths(actualMultikeyPaths));
            }
            ASSERT_TRUE(match);
        } else {
            ASSERT_TRUE(actualMultikeyPaths.empty());
        }
    }

protected:
    const ServiceContext::UniqueOperationContext _opCtx = cc().makeOperationContext();
    const NamespaceString _nss;

private:
    bool storageEngineSupportsPathLevelMultikeyTracking() {
        // Path-level multikey tracking is supported for all storage engines that use the KVCatalog.
        // MMAPv1 is the only storage engine that does not.
        //
        // TODO SERVER-22727: Store path-level multikey information in MMAPv1 index catalog.
        return !getGlobalServiceContext()->getGlobalStorageEngine()->isMmapV1();
    }

    std::string dumpMultikeyPaths(const MultikeyPaths& multikeyPaths) {
        std::stringstream ss;

        ss << "[ ";
        for (const auto multikeyComponents : multikeyPaths) {
            ss << "[ ";
            for (const auto multikeyComponent : multikeyComponents) {
                ss << multikeyComponent << " ";
            }
            ss << "] ";
        }
        ss << "]";

        return ss.str();
    }
};

TEST_F(MultikeyPathsTest, PathsUpdatedOnIndexCreation) {
    AutoGetCollection autoColl(_opCtx.get(), _nss, MODE_X);
    Collection* collection = autoColl.getCollection();
    invariant(collection);

    {
        WriteUnitOfWork wuow(_opCtx.get());
        OpDebug* const nullOpDebug = nullptr;
        const bool enforceQuota = true;
        ASSERT_OK(collection->insertDocument(
            _opCtx.get(),
            InsertStatement(BSON("_id" << 0 << "a" << 5 << "b" << BSON_ARRAY(1 << 2 << 3))),
            nullOpDebug,
            enforceQuota));
        wuow.commit();
    }

    BSONObj keyPattern = BSON("a" << 1 << "b" << 1);
    createIndex(collection,
                BSON("name"
                     << "a_1_b_1"
                     << "ns"
                     << _nss.ns()
                     << "key"
                     << keyPattern
                     << "v"
                     << static_cast<int>(kIndexVersion)))
        .transitional_ignore();

    assertMultikeyPaths(collection, keyPattern, {std::set<size_t>{}, {0U}});
}

TEST_F(MultikeyPathsTest, PathsUpdatedOnIndexCreationWithMultipleDocuments) {
    AutoGetCollection autoColl(_opCtx.get(), _nss, MODE_X);
    Collection* collection = autoColl.getCollection();
    invariant(collection);

    {
        WriteUnitOfWork wuow(_opCtx.get());
        OpDebug* const nullOpDebug = nullptr;
        const bool enforceQuota = true;
        ASSERT_OK(collection->insertDocument(
            _opCtx.get(),
            InsertStatement(BSON("_id" << 0 << "a" << 5 << "b" << BSON_ARRAY(1 << 2 << 3))),
            nullOpDebug,
            enforceQuota));
        ASSERT_OK(collection->insertDocument(
            _opCtx.get(),
            InsertStatement(BSON("_id" << 1 << "a" << BSON_ARRAY(1 << 2 << 3) << "b" << 5)),
            nullOpDebug,
            enforceQuota));
        wuow.commit();
    }

    BSONObj keyPattern = BSON("a" << 1 << "b" << 1);
    createIndex(collection,
                BSON("name"
                     << "a_1_b_1"
                     << "ns"
                     << _nss.ns()
                     << "key"
                     << keyPattern
                     << "v"
                     << static_cast<int>(kIndexVersion)))
        .transitional_ignore();

    assertMultikeyPaths(collection, keyPattern, {{0U}, {0U}});
}

TEST_F(MultikeyPathsTest, PathsUpdatedOnDocumentInsert) {
    AutoGetCollection autoColl(_opCtx.get(), _nss, MODE_X);
    Collection* collection = autoColl.getCollection();
    invariant(collection);

    BSONObj keyPattern = BSON("a" << 1 << "b" << 1);
    createIndex(collection,
                BSON("name"
                     << "a_1_b_1"
                     << "ns"
                     << _nss.ns()
                     << "key"
                     << keyPattern
                     << "v"
                     << static_cast<int>(kIndexVersion)))
        .transitional_ignore();

    {
        WriteUnitOfWork wuow(_opCtx.get());
        OpDebug* const nullOpDebug = nullptr;
        const bool enforceQuota = true;
        ASSERT_OK(collection->insertDocument(
            _opCtx.get(),
            InsertStatement(BSON("_id" << 0 << "a" << 5 << "b" << BSON_ARRAY(1 << 2 << 3))),
            nullOpDebug,
            enforceQuota));
        wuow.commit();
    }

    assertMultikeyPaths(collection, keyPattern, {std::set<size_t>{}, {0U}});

    {
        WriteUnitOfWork wuow(_opCtx.get());
        OpDebug* const nullOpDebug = nullptr;
        const bool enforceQuota = true;
        ASSERT_OK(collection->insertDocument(
            _opCtx.get(),
            InsertStatement(BSON("_id" << 1 << "a" << BSON_ARRAY(1 << 2 << 3) << "b" << 5)),
            nullOpDebug,
            enforceQuota));
        wuow.commit();
    }

    assertMultikeyPaths(collection, keyPattern, {{0U}, {0U}});
}

TEST_F(MultikeyPathsTest, PathsUpdatedOnDocumentUpdate) {
    AutoGetCollection autoColl(_opCtx.get(), _nss, MODE_X);
    Collection* collection = autoColl.getCollection();
    invariant(collection);

    BSONObj keyPattern = BSON("a" << 1 << "b" << 1);
    createIndex(collection,
                BSON("name"
                     << "a_1_b_1"
                     << "ns"
                     << _nss.ns()
                     << "key"
                     << keyPattern
                     << "v"
                     << static_cast<int>(kIndexVersion)))
        .transitional_ignore();

    {
        WriteUnitOfWork wuow(_opCtx.get());
        OpDebug* const nullOpDebug = nullptr;
        const bool enforceQuota = true;
        ASSERT_OK(collection->insertDocument(_opCtx.get(),
                                             InsertStatement(BSON("_id" << 0 << "a" << 5)),
                                             nullOpDebug,
                                             enforceQuota));
        wuow.commit();
    }

    assertMultikeyPaths(collection, keyPattern, {std::set<size_t>{}, std::set<size_t>{}});

    {
        auto cursor = collection->getCursor(_opCtx.get());
        auto record = cursor->next();
        invariant(record);

        auto oldDoc = collection->docFor(_opCtx.get(), record->id);
        {
            WriteUnitOfWork wuow(_opCtx.get());
            const bool enforceQuota = true;
            const bool indexesAffected = true;
            OpDebug* opDebug = nullptr;
            OplogUpdateEntryArgs args;
            collection
                ->updateDocument(_opCtx.get(),
                                 record->id,
                                 oldDoc,
                                 BSON("_id" << 0 << "a" << 5 << "b" << BSON_ARRAY(1 << 2 << 3)),
                                 enforceQuota,
                                 indexesAffected,
                                 opDebug,
                                 &args)
                .status_with_transitional_ignore();
            wuow.commit();
        }
    }

    assertMultikeyPaths(collection, keyPattern, {std::set<size_t>{}, {0U}});
}

TEST_F(MultikeyPathsTest, PathsNotUpdatedOnDocumentDelete) {
    AutoGetCollection autoColl(_opCtx.get(), _nss, MODE_X);
    Collection* collection = autoColl.getCollection();
    invariant(collection);

    BSONObj keyPattern = BSON("a" << 1 << "b" << 1);
    createIndex(collection,
                BSON("name"
                     << "a_1_b_1"
                     << "ns"
                     << _nss.ns()
                     << "key"
                     << keyPattern
                     << "v"
                     << static_cast<int>(kIndexVersion)))
        .transitional_ignore();

    {
        WriteUnitOfWork wuow(_opCtx.get());
        OpDebug* const nullOpDebug = nullptr;
        const bool enforceQuota = true;
        ASSERT_OK(collection->insertDocument(
            _opCtx.get(),
            InsertStatement(BSON("_id" << 0 << "a" << 5 << "b" << BSON_ARRAY(1 << 2 << 3))),
            nullOpDebug,
            enforceQuota));
        wuow.commit();
    }

    assertMultikeyPaths(collection, keyPattern, {std::set<size_t>{}, {0U}});

    {
        auto cursor = collection->getCursor(_opCtx.get());
        auto record = cursor->next();
        invariant(record);

        {
            WriteUnitOfWork wuow(_opCtx.get());
            OpDebug* const nullOpDebug = nullptr;
            collection->deleteDocument(_opCtx.get(), kUninitializedStmtId, record->id, nullOpDebug);
            wuow.commit();
        }
    }

    assertMultikeyPaths(collection, keyPattern, {std::set<size_t>{}, {0U}});
}

TEST_F(MultikeyPathsTest, PathsUpdatedForMultipleIndexesOnDocumentInsert) {
    AutoGetCollection autoColl(_opCtx.get(), _nss, MODE_X);
    Collection* collection = autoColl.getCollection();
    invariant(collection);

    BSONObj keyPatternAB = BSON("a" << 1 << "b" << 1);
    createIndex(collection,
                BSON("name"
                     << "a_1_b_1"
                     << "ns"
                     << _nss.ns()
                     << "key"
                     << keyPatternAB
                     << "v"
                     << static_cast<int>(kIndexVersion)))
        .transitional_ignore();

    BSONObj keyPatternAC = BSON("a" << 1 << "c" << 1);
    createIndex(collection,
                BSON("name"
                     << "a_1_c_1"
                     << "ns"
                     << _nss.ns()
                     << "key"
                     << keyPatternAC
                     << "v"
                     << static_cast<int>(kIndexVersion)))
        .transitional_ignore();
    {
        WriteUnitOfWork wuow(_opCtx.get());
        OpDebug* const nullOpDebug = nullptr;
        const bool enforceQuota = true;
        ASSERT_OK(collection->insertDocument(
            _opCtx.get(),
            InsertStatement(
                BSON("_id" << 0 << "a" << BSON_ARRAY(1 << 2 << 3) << "b" << 5 << "c" << 8)),
            nullOpDebug,
            enforceQuota));
        wuow.commit();
    }

    assertMultikeyPaths(collection, keyPatternAB, {{0U}, std::set<size_t>{}});
    assertMultikeyPaths(collection, keyPatternAC, {{0U}, std::set<size_t>{}});
}

}  // namespace
}  // namespace mongo
