//@file indexupdatetests.cpp : mongo/db/index_update.{h,cpp} tests

/**
 *    Copyright (C) 2012 10gen Inc.
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

#include <cstdint>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d.h"
#include "mongo/dbtests/dbtests.h"

namespace IndexUpdateTests {

using std::unique_ptr;

namespace {
const auto kIndexVersion = IndexDescriptor::IndexVersion::kV2;
}  // namespace

static const char* const _ns = "unittests.indexupdate";

/**
 * Test fixture for a write locked test using collection _ns.  Includes functionality to
 * partially construct a new IndexDetails in a manner that supports proper cleanup in
 * dropCollection().
 */
class IndexBuildBase {
public:
    IndexBuildBase() : _ctx(&_txn, _ns), _client(&_txn) {
        _client.createCollection(_ns);
    }
    ~IndexBuildBase() {
        _client.dropCollection(_ns);
        getGlobalServiceContext()->unsetKillAllOperations();
    }
    Collection* collection() {
        return _ctx.getCollection();
    }

protected:
    Status createIndex(const std::string& dbname, const BSONObj& indexSpec);

    bool buildIndexInterrupted(const BSONObj& key, bool allowInterruption) {
        try {
            MultiIndexBlock indexer(&_txn, collection());
            if (allowInterruption)
                indexer.allowInterruption();

            uassertStatusOK(indexer.init(key));
            uassertStatusOK(indexer.insertAllDocumentsInCollection());
            WriteUnitOfWork wunit(&_txn);
            indexer.commit();
            wunit.commit();
        } catch (const DBException& e) {
            if (ErrorCodes::isInterruption(ErrorCodes::Error(e.getCode())))
                return true;

            throw;
        }
        return false;
    }

    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _txn = *_txnPtr;
    OldClientWriteContext _ctx;
    DBDirectClient _client;
};

/** Index creation ignores unique constraints when told to. */
template <bool background>
class InsertBuildIgnoreUnique : public IndexBuildBase {
public:
    void run() {
        // Create a new collection.
        Database* db = _ctx.db();
        Collection* coll;
        {
            WriteUnitOfWork wunit(&_txn);
            db->dropCollection(&_txn, _ns);
            coll = db->createCollection(&_txn, _ns);

            OpDebug* const nullOpDebug = nullptr;
            coll->insertDocument(&_txn,
                                 BSON("_id" << 1 << "a"
                                            << "dup"),
                                 nullOpDebug,
                                 true);
            coll->insertDocument(&_txn,
                                 BSON("_id" << 2 << "a"
                                            << "dup"),
                                 nullOpDebug,
                                 true);
            wunit.commit();
        }

        MultiIndexBlock indexer(&_txn, coll);
        indexer.allowBackgroundBuilding();
        indexer.allowInterruption();
        indexer.ignoreUniqueConstraint();

        const BSONObj spec = BSON("name"
                                  << "a"
                                  << "ns"
                                  << coll->ns().ns()
                                  << "key"
                                  << BSON("a" << 1)
                                  << "v"
                                  << static_cast<int>(kIndexVersion)
                                  << "unique"
                                  << true
                                  << "background"
                                  << background);

        ASSERT_OK(indexer.init(spec).getStatus());
        ASSERT_OK(indexer.insertAllDocumentsInCollection());

        WriteUnitOfWork wunit(&_txn);
        indexer.commit();
        wunit.commit();
    }
};

/** Index creation enforces unique constraints unless told not to. */
template <bool background>
class InsertBuildEnforceUnique : public IndexBuildBase {
public:
    void run() {
        // Create a new collection.
        Database* db = _ctx.db();
        Collection* coll;
        {
            WriteUnitOfWork wunit(&_txn);
            db->dropCollection(&_txn, _ns);
            coll = db->createCollection(&_txn, _ns);

            OpDebug* const nullOpDebug = nullptr;
            coll->insertDocument(&_txn,
                                 BSON("_id" << 1 << "a"
                                            << "dup"),
                                 nullOpDebug,
                                 true);
            coll->insertDocument(&_txn,
                                 BSON("_id" << 2 << "a"
                                            << "dup"),
                                 nullOpDebug,
                                 true);
            wunit.commit();
        }

        MultiIndexBlock indexer(&_txn, coll);
        indexer.allowBackgroundBuilding();
        indexer.allowInterruption();
        // indexer.ignoreUniqueConstraint(); // not calling this

        const BSONObj spec = BSON("name"
                                  << "a"
                                  << "ns"
                                  << coll->ns().ns()
                                  << "key"
                                  << BSON("a" << 1)
                                  << "v"
                                  << static_cast<int>(kIndexVersion)
                                  << "unique"
                                  << true
                                  << "background"
                                  << background);

        ASSERT_OK(indexer.init(spec).getStatus());
        const Status status = indexer.insertAllDocumentsInCollection();
        ASSERT_EQUALS(status.code(), ErrorCodes::DuplicateKey);
    }
};

/** Index creation fills a passed-in set of dups rather than failing. */
template <bool background>
class InsertBuildFillDups : public IndexBuildBase {
public:
    void run() {
        // Create a new collection.
        Database* db = _ctx.db();
        Collection* coll;
        RecordId loc1;
        RecordId loc2;
        {
            WriteUnitOfWork wunit(&_txn);
            db->dropCollection(&_txn, _ns);
            coll = db->createCollection(&_txn, _ns);

            OpDebug* const nullOpDebug = nullptr;
            ASSERT_OK(coll->insertDocument(&_txn,
                                           BSON("_id" << 1 << "a"
                                                      << "dup"),
                                           nullOpDebug,
                                           true));
            ASSERT_OK(coll->insertDocument(&_txn,
                                           BSON("_id" << 2 << "a"
                                                      << "dup"),
                                           nullOpDebug,
                                           true));
            wunit.commit();
        }

        MultiIndexBlock indexer(&_txn, coll);
        indexer.allowBackgroundBuilding();
        indexer.allowInterruption();
        // indexer.ignoreUniqueConstraint(); // not calling this

        const BSONObj spec = BSON("name"
                                  << "a"
                                  << "ns"
                                  << coll->ns().ns()
                                  << "key"
                                  << BSON("a" << 1)
                                  << "v"
                                  << static_cast<int>(kIndexVersion)
                                  << "unique"
                                  << true
                                  << "background"
                                  << background);

        ASSERT_OK(indexer.init(spec).getStatus());

        std::set<RecordId> dups;
        ASSERT_OK(indexer.insertAllDocumentsInCollection(&dups));

        // either loc1 or loc2 should be in dups but not both.
        ASSERT_EQUALS(dups.size(), 1U);
        for (auto recordId : dups) {
            ASSERT_NOT_EQUALS(recordId, RecordId());
            BSONObj obj = coll->docFor(&_txn, recordId).value();
            int id = obj["_id"].Int();
            ASSERT(id == 1 || id == 2);
        }
    }
};

/** Index creation is killed if mayInterrupt is true. */
class InsertBuildIndexInterrupt : public IndexBuildBase {
public:
    void run() {
        // Create a new collection.
        Database* db = _ctx.db();
        Collection* coll;
        {
            WriteUnitOfWork wunit(&_txn);
            db->dropCollection(&_txn, _ns);
            coll = db->createCollection(&_txn, _ns);
            // Drop all indexes including id index.
            coll->getIndexCatalog()->dropAllIndexes(&_txn, true);
            // Insert some documents with enforceQuota=true.
            int32_t nDocs = 1000;
            OpDebug* const nullOpDebug = nullptr;
            for (int32_t i = 0; i < nDocs; ++i) {
                coll->insertDocument(&_txn, BSON("a" << i), nullOpDebug, true);
            }
            wunit.commit();
        }
        // Request an interrupt.
        getGlobalServiceContext()->setKillAllOperations();
        BSONObj indexInfo = BSON("key" << BSON("a" << 1) << "ns" << _ns << "name"
                                       << "a_1"
                                       << "v"
                                       << static_cast<int>(kIndexVersion));
        // The call is interrupted because mayInterrupt == true.
        ASSERT_TRUE(buildIndexInterrupted(indexInfo, true));
        // only want to interrupt the index build
        getGlobalServiceContext()->unsetKillAllOperations();
        // The new index is not listed in the index catalog because the index build failed.
        ASSERT(!coll->getIndexCatalog()->findIndexByName(&_txn, "a_1"));
    }
};

/** Index creation is not killed if mayInterrupt is false. */
class InsertBuildIndexInterruptDisallowed : public IndexBuildBase {
public:
    void run() {
        // Create a new collection.
        Database* db = _ctx.db();
        Collection* coll;
        {
            WriteUnitOfWork wunit(&_txn);
            db->dropCollection(&_txn, _ns);
            coll = db->createCollection(&_txn, _ns);
            coll->getIndexCatalog()->dropAllIndexes(&_txn, true);
            // Insert some documents.
            int32_t nDocs = 1000;
            OpDebug* const nullOpDebug = nullptr;
            for (int32_t i = 0; i < nDocs; ++i) {
                coll->insertDocument(&_txn, BSON("a" << i), nullOpDebug, true);
            }
            wunit.commit();
        }
        // Request an interrupt.
        getGlobalServiceContext()->setKillAllOperations();
        BSONObj indexInfo = BSON("key" << BSON("a" << 1) << "ns" << _ns << "name"
                                       << "a_1"
                                       << "v"
                                       << static_cast<int>(kIndexVersion));
        // The call is not interrupted because mayInterrupt == false.
        ASSERT_FALSE(buildIndexInterrupted(indexInfo, false));
        // only want to interrupt the index build
        getGlobalServiceContext()->unsetKillAllOperations();
        // The new index is listed in the index catalog because the index build completed.
        ASSERT(coll->getIndexCatalog()->findIndexByName(&_txn, "a_1"));
    }
};

/** Index creation is killed when building the _id index. */
class InsertBuildIdIndexInterrupt : public IndexBuildBase {
public:
    void run() {
        // Recreate the collection as capped, without an _id index.
        Database* db = _ctx.db();
        Collection* coll;
        {
            WriteUnitOfWork wunit(&_txn);
            db->dropCollection(&_txn, _ns);
            CollectionOptions options;
            options.capped = true;
            options.cappedSize = 10 * 1024;
            coll = db->createCollection(&_txn, _ns, options);
            coll->getIndexCatalog()->dropAllIndexes(&_txn, true);
            // Insert some documents.
            int32_t nDocs = 1000;
            OpDebug* const nullOpDebug = nullptr;
            for (int32_t i = 0; i < nDocs; ++i) {
                coll->insertDocument(&_txn, BSON("_id" << i), nullOpDebug, true);
            }
            wunit.commit();
        }
        // Request an interrupt.
        getGlobalServiceContext()->setKillAllOperations();
        BSONObj indexInfo = BSON("key" << BSON("_id" << 1) << "ns" << _ns << "name"
                                       << "_id_"
                                       << "v"
                                       << static_cast<int>(kIndexVersion));
        // The call is interrupted because mayInterrupt == true.
        ASSERT_TRUE(buildIndexInterrupted(indexInfo, true));
        // only want to interrupt the index build
        getGlobalServiceContext()->unsetKillAllOperations();
        // The new index is not listed in the index catalog because the index build failed.
        ASSERT(!coll->getIndexCatalog()->findIndexByName(&_txn, "_id_"));
    }
};

/** Index creation is not killed when building the _id index if mayInterrupt is false. */
class InsertBuildIdIndexInterruptDisallowed : public IndexBuildBase {
public:
    void run() {
        // Recreate the collection as capped, without an _id index.
        Database* db = _ctx.db();
        Collection* coll;
        {
            WriteUnitOfWork wunit(&_txn);
            db->dropCollection(&_txn, _ns);
            CollectionOptions options;
            options.capped = true;
            options.cappedSize = 10 * 1024;
            coll = db->createCollection(&_txn, _ns, options);
            coll->getIndexCatalog()->dropAllIndexes(&_txn, true);
            // Insert some documents.
            int32_t nDocs = 1000;
            OpDebug* const nullOpDebug = nullptr;
            for (int32_t i = 0; i < nDocs; ++i) {
                coll->insertDocument(&_txn, BSON("_id" << i), nullOpDebug, true);
            }
            wunit.commit();
        }
        // Request an interrupt.
        getGlobalServiceContext()->setKillAllOperations();
        BSONObj indexInfo = BSON("key" << BSON("_id" << 1) << "ns" << _ns << "name"
                                       << "_id_"
                                       << "v"
                                       << static_cast<int>(kIndexVersion));
        // The call is not interrupted because mayInterrupt == false.
        ASSERT_FALSE(buildIndexInterrupted(indexInfo, false));
        // only want to interrupt the index build
        getGlobalServiceContext()->unsetKillAllOperations();
        // The new index is listed in the index catalog because the index build succeeded.
        ASSERT(coll->getIndexCatalog()->findIndexByName(&_txn, "_id_"));
    }
};

/** Helpers::ensureIndex() is not interrupted. */
class HelpersEnsureIndexInterruptDisallowed : public IndexBuildBase {
public:
    void run() {
        // Insert some documents.
        int32_t nDocs = 1000;
        for (int32_t i = 0; i < nDocs; ++i) {
            _client.insert(_ns, BSON("a" << i));
        }
        // Start with just _id
        ASSERT_EQUALS(1U, _client.getIndexSpecs(_ns).size());
        // Request an interrupt.
        getGlobalServiceContext()->setKillAllOperations();
        // The call is not interrupted.
        Helpers::ensureIndex(&_txn, collection(), BSON("a" << 1), kIndexVersion, false, "a_1");
        // only want to interrupt the index build
        getGlobalServiceContext()->unsetKillAllOperations();
        // The new index is listed in getIndexSpecs because the index build completed.
        ASSERT_EQUALS(2U, _client.getIndexSpecs(_ns).size());
    }
};

Status IndexBuildBase::createIndex(const std::string& dbname, const BSONObj& indexSpec) {
    MultiIndexBlock indexer(&_txn, collection());
    Status status = indexer.init(indexSpec).getStatus();
    if (status == ErrorCodes::IndexAlreadyExists) {
        return Status::OK();
    }
    if (!status.isOK()) {
        return status;
    }
    status = indexer.insertAllDocumentsInCollection();
    if (!status.isOK()) {
        return status;
    }
    WriteUnitOfWork wunit(&_txn);
    indexer.commit();
    wunit.commit();
    return Status::OK();
}

/**
 * Fixture class that has a basic compound index.
 */
class SimpleCompoundIndex : public IndexBuildBase {
public:
    SimpleCompoundIndex() {
        ASSERT_OK(createIndex("unittest",
                              BSON("name"
                                   << "x"
                                   << "ns"
                                   << _ns
                                   << "key"
                                   << BSON("x" << 1 << "y" << 1)
                                   << "v"
                                   << static_cast<int>(kIndexVersion))));
    }
};

class SameSpecDifferentOption : public SimpleCompoundIndex {
public:
    void run() {
        // Cannot have same key spec with an option different from the existing one.
        ASSERT_EQUALS(ErrorCodes::IndexOptionsConflict,
                      createIndex("unittest",
                                  BSON("name"
                                       << "x"
                                       << "ns"
                                       << _ns
                                       << "unique"
                                       << true
                                       << "key"
                                       << BSON("x" << 1 << "y" << 1)
                                       << "v"
                                       << static_cast<int>(kIndexVersion))));
    }
};

class SameSpecSameOptions : public SimpleCompoundIndex {
public:
    void run() {
        ASSERT_OK(createIndex("unittest",
                              BSON("name"
                                   << "x"
                                   << "ns"
                                   << _ns
                                   << "key"
                                   << BSON("x" << 1 << "y" << 1)
                                   << "v"
                                   << static_cast<int>(kIndexVersion))));
    }
};

class DifferentSpecSameName : public SimpleCompoundIndex {
public:
    void run() {
        // Cannot create a different index with the same name as the existing one.
        ASSERT_EQUALS(ErrorCodes::IndexKeySpecsConflict,
                      createIndex("unittest",
                                  BSON("name"
                                       << "x"
                                       << "ns"
                                       << _ns
                                       << "key"
                                       << BSON("y" << 1 << "x" << 1)
                                       << "v"
                                       << static_cast<int>(kIndexVersion))));
    }
};

/**
 * Fixture class for indexes with complex options.
 */
class ComplexIndex : public IndexBuildBase {
public:
    ComplexIndex() {
        ASSERT_OK(createIndex("unittests",
                              BSON("name"
                                   << "super"
                                   << "ns"
                                   << _ns
                                   << "unique"
                                   << 1
                                   << "sparse"
                                   << true
                                   << "expireAfterSeconds"
                                   << 3600
                                   << "key"
                                   << BSON("superIdx"
                                           << "2d")
                                   << "v"
                                   << static_cast<int>(kIndexVersion))));
    }
};

class SameSpecSameOptionDifferentOrder : public ComplexIndex {
public:
    void run() {
        // Exactly the same specs with the existing one, only
        // specified in a different order than the original.
        ASSERT_OK(createIndex("unittests",
                              BSON("name"
                                   << "super2"
                                   << "ns"
                                   << _ns
                                   << "expireAfterSeconds"
                                   << 3600
                                   << "sparse"
                                   << true
                                   << "unique"
                                   << 1
                                   << "key"
                                   << BSON("superIdx"
                                           << "2d")
                                   << "v"
                                   << static_cast<int>(kIndexVersion))));
    }
};

// The following tests tries to create an index with almost the same
// specs as the original, except for one option.

class SameSpecDifferentUnique : public ComplexIndex {
public:
    void run() {
        ASSERT_EQUALS(ErrorCodes::IndexOptionsConflict,
                      createIndex("unittest",
                                  BSON("name"
                                       << "super2"
                                       << "ns"
                                       << _ns
                                       << "unique"
                                       << false
                                       << "sparse"
                                       << true
                                       << "expireAfterSeconds"
                                       << 3600
                                       << "key"
                                       << BSON("superIdx"
                                               << "2d")
                                       << "v"
                                       << static_cast<int>(kIndexVersion))));
    }
};

class SameSpecDifferentSparse : public ComplexIndex {
public:
    void run() {
        ASSERT_EQUALS(ErrorCodes::IndexOptionsConflict,
                      createIndex("unittest",
                                  BSON("name"
                                       << "super2"
                                       << "ns"
                                       << _ns
                                       << "unique"
                                       << 1
                                       << "sparse"
                                       << false
                                       << "background"
                                       << true
                                       << "expireAfterSeconds"
                                       << 3600
                                       << "key"
                                       << BSON("superIdx"
                                               << "2d")
                                       << "v"
                                       << static_cast<int>(kIndexVersion))));
    }
};

class SameSpecDifferentTTL : public ComplexIndex {
public:
    void run() {
        ASSERT_EQUALS(ErrorCodes::IndexOptionsConflict,
                      createIndex("unittest",
                                  BSON("name"
                                       << "super2"
                                       << "ns"
                                       << _ns
                                       << "unique"
                                       << 1
                                       << "sparse"
                                       << true
                                       << "expireAfterSeconds"
                                       << 2400
                                       << "key"
                                       << BSON("superIdx"
                                               << "2d")
                                       << "v"
                                       << static_cast<int>(kIndexVersion))));
    }
};

class StorageEngineOptions : public IndexBuildBase {
public:
    void run() {
        // "storageEngine" field has to be an object if present.
        ASSERT_NOT_OK(createIndex("unittest", _createSpec(12345)));

        // 'storageEngine' must not be empty.
        ASSERT_NOT_OK(createIndex("unittest", _createSpec(BSONObj())));

        // Every field under "storageEngine" must match a registered storage engine.
        ASSERT_NOT_OK(createIndex("unittest", _createSpec(BSON("unknownEngine" << BSONObj()))));

        // Testing with 'wiredTiger' because the registered storage engine factory
        // supports custom index options under 'storageEngine'.
        const std::string storageEngineName = "wiredTiger";

        // Run 'wiredTiger' tests if the storage engine is supported.
        if (getGlobalServiceContext()->isRegisteredStorageEngine(storageEngineName)) {
            // Every field under "storageEngine" has to be an object.
            ASSERT_NOT_OK(createIndex("unittest", _createSpec(BSON(storageEngineName << 1))));

            // Storage engine options must pass validation by the storage engine factory.
            // For 'wiredTiger', embedded document must contain 'configString'.
            ASSERT_NOT_OK(createIndex(
                "unittest", _createSpec(BSON(storageEngineName << BSON("unknown" << 1)))));

            // Configuration string for 'wiredTiger' must be a string.
            ASSERT_NOT_OK(createIndex(
                "unittest", _createSpec(BSON(storageEngineName << BSON("configString" << 1)))));

            // Valid 'wiredTiger' configuration.
            ASSERT_OK(createIndex(
                "unittest",
                _createSpec(BSON(storageEngineName << BSON("configString"
                                                           << "block_compressor=zlib")))));
        }
    }

protected:
    template <typename T>
    BSONObj _createSpec(T storageEngineValue) {
        return BSON("name"
                    << "super2"
                    << "ns"
                    << _ns
                    << "key"
                    << BSON("a" << 1)
                    << "v"
                    << static_cast<int>(kIndexVersion)
                    << "storageEngine"
                    << storageEngineValue);
    }
};

class IndexCatatalogFixIndexKey {
public:
    void run() {
        ASSERT_BSONOBJ_EQ(BSON("x" << 1), IndexCatalog::fixIndexKey(BSON("x" << 1)));

        ASSERT_BSONOBJ_EQ(BSON("_id" << 1), IndexCatalog::fixIndexKey(BSON("_id" << 1)));

        ASSERT_BSONOBJ_EQ(BSON("_id" << 1), IndexCatalog::fixIndexKey(BSON("_id" << true)));
    }
};

class InsertSymbolIntoIndexWithCollationFails {
public:
    void run() {
        auto opCtx = cc().makeOperationContext();
        FeatureCompatibilityVersion::set(opCtx.get(), "3.4");
        DBDirectClient client(opCtx.get());
        client.dropCollection(_ns);
        IndexSpec indexSpec;
        indexSpec.addKey("a").addOptions(BSON("collation" << BSON("locale"
                                                                  << "fr")));
        client.createIndex(_ns, indexSpec);
        client.insert(_ns, BSON("a" << BSONSymbol("mySymbol")));
        ASSERT_EQUALS(client.getLastErrorDetailed()["code"].numberInt(),
                      ErrorCodes::CannotBuildIndexKeys);
        ASSERT_EQUALS(client.count(_ns), 0U);
    }
};

class InsertSymbolIntoIndexWithoutCollationSucceeds {
public:
    void run() {
        auto opCtx = cc().makeOperationContext();
        FeatureCompatibilityVersion::set(opCtx.get(), "3.4");
        DBDirectClient client(opCtx.get());
        client.dropCollection(_ns);
        IndexSpec indexSpec;
        indexSpec.addKey("a");
        client.createIndex(_ns, indexSpec);
        client.insert(_ns, BSON("a" << BSONSymbol("mySymbol")));
        ASSERT(client.getLastError().empty());
        ASSERT_EQUALS(client.count(_ns), 1U);
    }
};

class InsertSymbolInsideNestedObjectIntoIndexWithCollationFails {
public:
    void run() {
        auto opCtx = cc().makeOperationContext();
        FeatureCompatibilityVersion::set(opCtx.get(), "3.4");
        DBDirectClient client(opCtx.get());
        client.dropCollection(_ns);
        IndexSpec indexSpec;
        indexSpec.addKey("a").addOptions(BSON("collation" << BSON("locale"
                                                                  << "fr")));
        client.createIndex(_ns, indexSpec);
        client.insert(_ns, BSON("a" << BSON("b" << 99 << "c" << BSONSymbol("mySymbol"))));
        ASSERT_EQUALS(client.getLastErrorDetailed()["code"].numberInt(),
                      ErrorCodes::CannotBuildIndexKeys);
        ASSERT_EQUALS(client.count(_ns), 0U);
    }
};

class InsertSymbolInsideNestedArrayIntoIndexWithCollationFails {
public:
    void run() {
        auto opCtx = cc().makeOperationContext();
        FeatureCompatibilityVersion::set(opCtx.get(), "3.4");
        DBDirectClient client(opCtx.get());
        client.dropCollection(_ns);
        IndexSpec indexSpec;
        indexSpec.addKey("a").addOptions(BSON("collation" << BSON("locale"
                                                                  << "fr")));
        client.createIndex(_ns, indexSpec);
        client.insert(_ns, BSON("a" << BSON_ARRAY(99 << BSONSymbol("mySymbol"))));
        ASSERT_EQUALS(client.getLastErrorDetailed()["code"].numberInt(),
                      ErrorCodes::CannotBuildIndexKeys);
        ASSERT_EQUALS(client.count(_ns), 0U);
    }
};

class BuildingIndexWithCollationWhenSymbolDataExistsShouldFail {
public:
    void run() {
        auto opCtx = cc().makeOperationContext();
        FeatureCompatibilityVersion::set(opCtx.get(), "3.4");
        DBDirectClient client(opCtx.get());
        client.dropCollection(_ns);
        client.insert(_ns, BSON("a" << BSON_ARRAY(99 << BSONSymbol("mySymbol"))));
        ASSERT_EQUALS(client.count(_ns), 1U);
        IndexSpec indexSpec;
        indexSpec.addKey("a").addOptions(BSON("collation" << BSON("locale"
                                                                  << "fr")));
        ASSERT_THROWS_CODE(
            client.createIndex(_ns, indexSpec), UserException, ErrorCodes::CannotBuildIndexKeys);
    }
};

class IndexingSymbolWithInheritedCollationShouldFail {
public:
    void run() {
        auto opCtx = cc().makeOperationContext();
        FeatureCompatibilityVersion::set(opCtx.get(), "3.4");
        DBDirectClient client(opCtx.get());
        client.dropCollection(_ns);
        BSONObj cmdResult;
        ASSERT_TRUE(client.runCommand("unittests",
                                      BSON("create"
                                           << "indexupdate"
                                           << "collation"
                                           << BSON("locale"
                                                   << "fr")),
                                      cmdResult));
        IndexSpec indexSpec;
        indexSpec.addKey("a");
        client.createIndex(_ns, indexSpec);
        client.insert(_ns, BSON("a" << BSON_ARRAY(99 << BSONSymbol("mySymbol"))));
        ASSERT_EQUALS(client.getLastErrorDetailed()["code"].numberInt(),
                      ErrorCodes::CannotBuildIndexKeys);
    }
};

class IndexUpdateTests : public Suite {
public:
    IndexUpdateTests() : Suite("indexupdate") {}

    void setupTests() {
        add<InsertBuildIgnoreUnique<true>>();
        add<InsertBuildIgnoreUnique<false>>();
        add<InsertBuildEnforceUnique<true>>();
        add<InsertBuildEnforceUnique<false>>();
        add<InsertBuildFillDups<true>>();
        add<InsertBuildFillDups<false>>();
        add<InsertBuildIndexInterrupt>();
        add<InsertBuildIndexInterruptDisallowed>();
        add<InsertBuildIdIndexInterrupt>();
        add<InsertBuildIdIndexInterruptDisallowed>();
        add<HelpersEnsureIndexInterruptDisallowed>();
        add<SameSpecDifferentOption>();
        add<SameSpecSameOptions>();
        add<DifferentSpecSameName>();
        add<SameSpecSameOptionDifferentOrder>();
        add<SameSpecDifferentUnique>();
        add<SameSpecDifferentSparse>();
        add<SameSpecDifferentTTL>();
        add<StorageEngineOptions>();

        add<IndexCatatalogFixIndexKey>();

        add<InsertSymbolInsideNestedObjectIntoIndexWithCollationFails>();
        add<InsertSymbolIntoIndexWithoutCollationSucceeds>();
        add<InsertSymbolInsideNestedObjectIntoIndexWithCollationFails>();
        add<InsertSymbolInsideNestedArrayIntoIndexWithCollationFails>();
        add<BuildingIndexWithCollationWhenSymbolDataExistsShouldFail>();
        add<IndexingSymbolWithInheritedCollationShouldFail>();
    }
} indexUpdateTests;

}  // namespace IndexUpdateTests
