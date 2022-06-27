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

#include "mongo/platform/basic.h"

#include <cstdint>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/multi_index_block.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine_init.h"
#include "mongo/dbtests/dbtests.h"

namespace IndexUpdateTests {

namespace {
const auto kIndexVersion = IndexDescriptor::IndexVersion::kV2;
}  // namespace

static const char* const _ns = "unittests.indexupdate";
static const NamespaceString _nss = NamespaceString(_ns);

/**
 * Test fixture for a write locked test using collection _ns.  Includes functionality to
 * partially construct a new IndexDetails in a manner that supports proper cleanup in
 * dropcollection().get().
 */
class IndexBuildBase {
public:
    IndexBuildBase() {
        regenOpCtx();

        AutoGetCollection autoColl(_opCtx, _nss, LockMode::MODE_IX);
        WriteUnitOfWork wuow(_opCtx);
        auto db = autoColl.ensureDbExists(_opCtx);
        ASSERT(db->createCollection(_opCtx, _nss)) << _nss;
        wuow.commit();
    }

    ~IndexBuildBase() {
        getGlobalServiceContext()->unsetKillAllOperations();

        AutoGetCollection autoColl(_opCtx, _nss, LockMode::MODE_X);
        WriteUnitOfWork wuow(_opCtx);
        auto db = autoColl.ensureDbExists(_opCtx);
        ASSERT_OK(db->dropCollection(_opCtx, _nss, {})) << _nss;
        wuow.commit();
    }

    CollectionWriter& collection() {
        _collection.emplace(_opCtx, _nss);
        return *_collection;
    }

protected:
    Status createIndex(const BSONObj& indexSpec);

    bool buildIndexInterrupted(const BSONObj& key) {
        try {
            MultiIndexBlock indexer;

            ScopeGuard abortOnExit([&] {
                indexer.abortIndexBuild(_opCtx, collection(), MultiIndexBlock::kNoopOnCleanUpFn);
            });

            uassertStatusOK(
                indexer.init(_opCtx, collection(), key, MultiIndexBlock::kNoopOnInitFn));
            uassertStatusOK(indexer.insertAllDocumentsInCollection(_opCtx, collection().get()));
            WriteUnitOfWork wunit(_opCtx);
            ASSERT_OK(indexer.commit(_opCtx,
                                     collection().getWritableCollection(_opCtx),
                                     MultiIndexBlock::kNoopOnCreateEachFn,
                                     MultiIndexBlock::kNoopOnCommitFn));
            wunit.commit();
            abortOnExit.dismiss();
        } catch (const DBException& e) {
            if (ErrorCodes::isInterruption(e.code()))
                return true;

            throw;
        }
        return false;
    }

    void regenOpCtx() {
        _txnPtr = nullptr;
        _txnPtr = cc().makeOperationContext();
        _opCtx = _txnPtr.get();
    }

    ServiceContext::UniqueOperationContext _txnPtr;  // = cc().makeOperationContext();
    OperationContext* _opCtx;                        // = _txnPtr.get();
    boost::optional<CollectionWriter> _collection;
};

/** Index creation ignores unique constraints when told to. */
template <bool background>
class InsertBuildIgnoreUnique : public IndexBuildBase {
public:
    void run() {
        AutoGetCollection autoColl(_opCtx, _nss, LockMode::MODE_X);
        auto db = autoColl.ensureDbExists(_opCtx);
        ASSERT(db) << _nss;
        auto& coll = collection();
        {
            WriteUnitOfWork wunit(_opCtx);
            OpDebug* const nullOpDebug = nullptr;
            ASSERT_OK(coll->insertDocument(_opCtx,
                                           InsertStatement(BSON("_id" << 1 << "a"
                                                                      << "dup")),
                                           nullOpDebug,
                                           true));
            ASSERT_OK(coll->insertDocument(_opCtx,
                                           InsertStatement(BSON("_id" << 2 << "a"
                                                                      << "dup")),
                                           nullOpDebug,
                                           true));
            wunit.commit();
        }

        MultiIndexBlock indexer;
        indexer.ignoreUniqueConstraint();

        const BSONObj spec = BSON("name"
                                  << "a"
                                  << "key" << BSON("a" << 1) << "v"
                                  << static_cast<int>(kIndexVersion) << "unique" << true
                                  << "background" << background);

        ScopeGuard abortOnExit([&] {
            indexer.abortIndexBuild(_opCtx, collection(), MultiIndexBlock::kNoopOnCleanUpFn);
        });

        ASSERT_OK(indexer.init(_opCtx, coll, spec, MultiIndexBlock::kNoopOnInitFn).getStatus());
        ASSERT_OK(indexer.insertAllDocumentsInCollection(_opCtx, coll.get()));
        ASSERT_OK(indexer.checkConstraints(_opCtx, coll.get()));

        WriteUnitOfWork wunit(_opCtx);
        ASSERT_OK(indexer.commit(_opCtx,
                                 coll.getWritableCollection(_opCtx),
                                 MultiIndexBlock::kNoopOnCreateEachFn,
                                 MultiIndexBlock::kNoopOnCommitFn));
        wunit.commit();
        abortOnExit.dismiss();
    }
};

/** Index creation enforces unique constraints unless told not to. */
template <bool background>
class InsertBuildEnforceUnique : public IndexBuildBase {
public:
    void run() {
        // Create a new collection.
        {
            AutoGetCollection autoColl(_opCtx, _nss, LockMode::MODE_IX);
            auto db = autoColl.ensureDbExists(_opCtx);
            ASSERT(db) << _nss;

            auto& coll = collection();
            {
                WriteUnitOfWork wunit(_opCtx);
                OpDebug* const nullOpDebug = nullptr;
                ASSERT_OK(coll->insertDocument(_opCtx,
                                               InsertStatement(BSON("_id" << 1 << "a"
                                                                          << "dup")),
                                               nullOpDebug,
                                               true));
                ASSERT_OK(coll->insertDocument(_opCtx,
                                               InsertStatement(BSON("_id" << 2 << "a"
                                                                          << "dup")),
                                               nullOpDebug,
                                               true));
                wunit.commit();
            }
        }
        {
            AutoGetCollection autoColl(_opCtx, _nss, LockMode::MODE_X);
            MultiIndexBlock indexer;

            const BSONObj spec = BSON("name"
                                      << "a"
                                      << "key" << BSON("a" << 1) << "v"
                                      << static_cast<int>(kIndexVersion) << "unique" << true
                                      << "background" << background);
            ScopeGuard abortOnExit([&] {
                indexer.abortIndexBuild(_opCtx, collection(), MultiIndexBlock::kNoopOnCleanUpFn);
            });

            ASSERT_OK(indexer.init(_opCtx, collection(), spec, MultiIndexBlock::kNoopOnInitFn)
                          .getStatus());

            auto& coll = collection();
            auto desc = coll->getIndexCatalog()->findIndexByName(
                _opCtx,
                "a",
                IndexCatalog::InclusionPolicy::kReady | IndexCatalog::InclusionPolicy::kUnfinished);
            ASSERT(desc);

            // Hybrid index builds check duplicates explicitly.
            ASSERT_OK(indexer.insertAllDocumentsInCollection(_opCtx, coll.get()));

            auto status = indexer.checkConstraints(_opCtx, coll.get());
            ASSERT_EQUALS(status.code(), ErrorCodes::DuplicateKey);
        }
    }
};

/** Index creation is killed if mayInterrupt is true. */
class InsertBuildIndexInterrupt : public IndexBuildBase {
public:
    void run() {
        {
            AutoGetCollection autoColl(_opCtx, _nss, LockMode::MODE_X);
            auto db = autoColl.ensureDbExists(_opCtx);
            ASSERT(db) << _nss;

            auto& coll = collection();
            {
                WriteUnitOfWork wunit(_opCtx);
                // Drop all indexes including id index.
                coll.getWritableCollection(_opCtx)->getIndexCatalog()->dropAllIndexes(
                    _opCtx, coll.getWritableCollection(_opCtx), true, {});
                // Insert some documents.
                int32_t nDocs = 1000;
                OpDebug* const nullOpDebug = nullptr;
                for (int32_t i = 0; i < nDocs; ++i) {
                    ASSERT_OK(
                        coll->insertDocument(_opCtx, InsertStatement(BSON("a" << i)), nullOpDebug));
                }
                wunit.commit();
            }
            // Request an interrupt.
            getGlobalServiceContext()->setKillAllOperations();
            BSONObj indexInfo = BSON("key" << BSON("a" << 1) << "name"
                                           << "a_1"
                                           << "v" << static_cast<int>(kIndexVersion));
            // The call is interrupted because mayInterrupt == true.
            ASSERT_TRUE(buildIndexInterrupted(indexInfo));
            // only want to interrupt the index build
            getGlobalServiceContext()->unsetKillAllOperations();
        }

        regenOpCtx();
        AutoGetDb dbRaii(_opCtx, _nss.db(), LockMode::MODE_IX);
        boost::optional<Lock::CollectionLock> collLk;
        collLk.emplace(_opCtx, _nss, LockMode::MODE_IX);
        // The new index is not listed in the index catalog because the index build failed.
        ASSERT(!collection().get()->getIndexCatalog()->findIndexByName(_opCtx, "a_1"));
    }
};

/** Index creation is killed when building the _id index. */
class InsertBuildIdIndexInterrupt : public IndexBuildBase {
public:
    void run() {
        // Skip the test if the storage engine doesn't support capped collections.
        if (!getGlobalServiceContext()->getStorageEngine()->supportsCappedCollections()) {
            return;
        }

        {
            // Recreate the collection as capped, without an _id index.
            AutoGetCollection autoColl(_opCtx, _nss, LockMode::MODE_X);
            auto db = autoColl.ensureDbExists(_opCtx);

            WriteUnitOfWork wunit(_opCtx);
            ASSERT_OK(db->dropCollection(_opCtx, _nss));
            CollectionOptions options;
            options.capped = true;
            options.cappedSize = 10 * 1024;
            Collection* coll = db->createCollection(_opCtx, _nss, options);
            coll->getIndexCatalog()->dropAllIndexes(_opCtx, coll, true, {});
            // Insert some documents.
            int32_t nDocs = 1000;
            OpDebug* const nullOpDebug = nullptr;
            for (int32_t i = 0; i < nDocs; ++i) {
                ASSERT_OK(coll->insertDocument(
                    _opCtx, InsertStatement(BSON("_id" << i)), nullOpDebug, true));
            }
            wunit.commit();
            // Request an interrupt.
            getGlobalServiceContext()->setKillAllOperations();
            BSONObj indexInfo = BSON("key" << BSON("_id" << 1) << "name"
                                           << "_id_"
                                           << "v" << static_cast<int>(kIndexVersion));
            ASSERT_TRUE(buildIndexInterrupted(indexInfo));
            // only want to interrupt the index build
            getGlobalServiceContext()->unsetKillAllOperations();
        }
        regenOpCtx();
        AutoGetCollection autoColl(_opCtx, _nss, LockMode::MODE_IX);

        // The new index is not listed in the index catalog because the index build failed.
        ASSERT(!collection().get()->getIndexCatalog()->findIndexByName(_opCtx, "_id_"));
    }
};  // namespace IndexUpdateTests

Status IndexBuildBase::createIndex(const BSONObj& indexSpec) {
    Lock::DBLock dbLk(_opCtx, _nss.dbName(), MODE_IX);
    Lock::CollectionLock collLk(_opCtx, _nss, MODE_X);

    MultiIndexBlock indexer;
    ScopeGuard abortOnExit(
        [&] { indexer.abortIndexBuild(_opCtx, collection(), MultiIndexBlock::kNoopOnCleanUpFn); });
    Status status =
        indexer.init(_opCtx, collection(), indexSpec, MultiIndexBlock::kNoopOnInitFn).getStatus();
    if (status == ErrorCodes::IndexAlreadyExists) {
        return Status::OK();
    }
    if (!status.isOK()) {
        return status;
    }
    status = indexer.insertAllDocumentsInCollection(_opCtx, collection().get());
    if (!status.isOK()) {
        return status;
    }
    status = indexer.checkConstraints(_opCtx, collection().get());
    if (!status.isOK()) {
        return status;
    }
    WriteUnitOfWork wunit(_opCtx);
    ASSERT_OK(indexer.commit(_opCtx,
                             collection().getWritableCollection(_opCtx),
                             MultiIndexBlock::kNoopOnCreateEachFn,
                             MultiIndexBlock::kNoopOnCommitFn));
    wunit.commit();
    abortOnExit.dismiss();
    return Status::OK();
}

/**
 * Fixture class that has a basic compound index.
 */
class SimpleCompoundIndex : public IndexBuildBase {
public:
    SimpleCompoundIndex() {
        ASSERT_OK(createIndex(BSON("name"
                                   << "x"
                                   << "key" << BSON("x" << 1 << "y" << 1) << "v"
                                   << static_cast<int>(kIndexVersion))));
    }
};

class SameSpecDifferentOption : public SimpleCompoundIndex {
public:
    void run() {
        // Cannot have same key spec with an option different from the existing one.
        ASSERT_EQUALS(ErrorCodes::IndexOptionsConflict,
                      createIndex(BSON("name"
                                       << "x"
                                       << "key" << BSON("x" << 1 << "y" << 1) << "storageEngine"
                                       << BSON("wiredTiger" << BSONObj()) << "v"
                                       << static_cast<int>(kIndexVersion))));
    }
};

class SameSpecSameOptions : public SimpleCompoundIndex {
public:
    void run() {
        ASSERT_OK(createIndex(BSON("name"
                                   << "x"
                                   << "key" << BSON("x" << 1 << "y" << 1) << "v"
                                   << static_cast<int>(kIndexVersion))));
    }
};

class DifferentSpecSameName : public SimpleCompoundIndex {
public:
    void run() {
        // Cannot create a different index with the same name as the existing one.
        ASSERT_EQUALS(ErrorCodes::IndexKeySpecsConflict,
                      createIndex(BSON("name"
                                       << "x"
                                       << "key" << BSON("y" << 1 << "x" << 1) << "v"
                                       << static_cast<int>(kIndexVersion))));
    }
};

/**
 * Fixture class for indexes with complex options.
 */
class ComplexIndex : public IndexBuildBase {
public:
    ComplexIndex() {
        ASSERT_OK(createIndex(BSON("name"
                                   << "super"
                                   << "unique" << 1 << "sparse" << true << "expireAfterSeconds"
                                   << 3600 << "key"
                                   << BSON("superIdx"
                                           << "2d")
                                   << "v" << static_cast<int>(kIndexVersion))));
    }
};

class SameSpecDifferentNameDifferentOrder : public ComplexIndex {
public:
    void run() {
        // Exactly the same specs with the existing one, only specified in a different order than
        // the original. This will throw an IndexOptionsConflict as the index already exists under
        // another name.
        ASSERT_EQUALS(ErrorCodes::IndexOptionsConflict,
                      createIndex(BSON("name"
                                       << "super2"
                                       << "expireAfterSeconds" << 3600 << "sparse" << true
                                       << "unique" << 1 << "key"
                                       << BSON("superIdx"
                                               << "2d")
                                       << "v" << static_cast<int>(kIndexVersion))));
    }
};

class SameSpecSameNameDifferentOrder : public ComplexIndex {
public:
    void run() {
        // Exactly the same specs with the existing one, only specified in a different order than
        // the original, but with the same name.
        ASSERT_OK(createIndex(BSON("name"
                                   << "super"
                                   << "expireAfterSeconds" << 3600 << "sparse" << true << "unique"
                                   << 1 << "key"
                                   << BSON("superIdx"
                                           << "2d")
                                   << "v" << static_cast<int>(kIndexVersion))));
    }
};

// The following tests tries to create an index with almost the same
// specs as the original, except for one option.

class SameSpecDifferentUnique : public ComplexIndex {
public:
    void run() {
        ASSERT_OK(createIndex(BSON("name"
                                   << "super2"
                                   << "unique" << false << "sparse" << true << "expireAfterSeconds"
                                   << 3600 << "key"
                                   << BSON("superIdx"
                                           << "2d")
                                   << "v" << static_cast<int>(kIndexVersion))));
    }
};

class SameSpecDifferentSparse : public ComplexIndex {
public:
    void run() {
        ASSERT_OK(createIndex(BSON("name"
                                   << "super3"
                                   << "unique" << 1 << "sparse" << false << "background" << true
                                   << "expireAfterSeconds" << 3600 << "key"
                                   << BSON("superIdx"
                                           << "2d")
                                   << "v" << static_cast<int>(kIndexVersion))));
    }
};

class SameSpecDifferentTTL : public ComplexIndex {
public:
    void run() {
        ASSERT_EQUALS(ErrorCodes::IndexOptionsConflict,
                      createIndex(BSON("name"
                                       << "super4"
                                       << "unique" << 1 << "sparse" << true << "expireAfterSeconds"
                                       << 2400 << "key"
                                       << BSON("superIdx"
                                               << "2d")
                                       << "v" << static_cast<int>(kIndexVersion))));
    }
};

class StorageEngineOptions : public IndexBuildBase {
public:
    void run() {
        // "storageEngine" field has to be an object if present.
        ASSERT_NOT_OK(createIndex(_createSpec(12345)));

        // 'storageEngine' must not be empty.
        ASSERT_NOT_OK(createIndex(_createSpec(BSONObj())));

        // Every field under "storageEngine" must match a registered storage engine.
        ASSERT_NOT_OK(createIndex(_createSpec(BSON("unknownEngine" << BSONObj()))));

        // Testing with 'wiredTiger' because the registered storage engine factory
        // supports custom index options under 'storageEngine'.
        const std::string storageEngineName = "wiredTiger";

        // Run 'wiredTiger' tests if the storage engine is supported.
        if (isRegisteredStorageEngine(getGlobalServiceContext(), storageEngineName)) {
            // Every field under "storageEngine" has to be an object.
            ASSERT_NOT_OK(createIndex(_createSpec(BSON(storageEngineName << 1))));

            // Storage engine options must pass validation by the storage engine factory.
            // For 'wiredTiger', embedded document must contain 'configString'.
            ASSERT_NOT_OK(
                createIndex(_createSpec(BSON(storageEngineName << BSON("unknown" << 1)))));

            // Configuration string for 'wiredTiger' must be a string.
            ASSERT_NOT_OK(
                createIndex(_createSpec(BSON(storageEngineName << BSON("configString" << 1)))));

            // Valid 'wiredTiger' configuration.
            ASSERT_OK(createIndex(
                _createSpec(BSON(storageEngineName << BSON("configString"
                                                           << "block_compressor=zlib")))));
        }
    }

protected:
    template <typename T>
    BSONObj _createSpec(T storageEngineValue) {
        return BSON("name"
                    << "super2"
                    << "key" << BSON("a" << 1) << "v" << static_cast<int>(kIndexVersion)
                    << "storageEngine" << storageEngineValue);
    }
};

class IndexCatatalogFixIndexKey : public IndexBuildBase {
public:
    void run() {
        auto indexCatalog = collection().get()->getIndexCatalog();

        ASSERT_BSONOBJ_EQ(BSON("x" << 1), indexCatalog->fixIndexKey(BSON("x" << 1)));

        ASSERT_BSONOBJ_EQ(BSON("_id" << 1), indexCatalog->fixIndexKey(BSON("_id" << 1)));

        ASSERT_BSONOBJ_EQ(BSON("_id" << 1), indexCatalog->fixIndexKey(BSON("_id" << true)));
    }
};

class InsertSymbolIntoIndexWithCollationFails {
public:
    void run() {
        auto opCtx = cc().makeOperationContext();
        DBDirectClient client(opCtx.get());
        client.dropCollection(_ns);
        IndexSpec indexSpec;
        indexSpec.addKey("a").addOptions(BSON("collation" << BSON("locale"
                                                                  << "fr")));
        client.createIndex(_ns, indexSpec);

        auto response = client.insertAcknowledged(_ns, {BSON("a" << BSONSymbol("mySymbol"))});
        ASSERT_EQUALS(getStatusFromWriteCommandReply(response), ErrorCodes::CannotBuildIndexKeys);
        ASSERT_EQUALS(client.count(_nss), 0U);
    }
};

class InsertSymbolIntoIndexWithoutCollationSucceeds {
public:
    void run() {
        auto opCtx = cc().makeOperationContext();
        DBDirectClient client(opCtx.get());
        client.dropCollection(_ns);
        IndexSpec indexSpec;
        indexSpec.addKey("a");
        client.createIndex(_ns, indexSpec);

        auto response = client.insertAcknowledged(_ns, {BSON("a" << BSONSymbol("mySymbol"))});
        ASSERT_OK(getStatusFromWriteCommandReply(response));
        ASSERT_EQUALS(response["n"].Int(), 1);
        ASSERT_EQUALS(client.count(_nss), 1U);
    }
};

class InsertSymbolInsideNestedObjectIntoIndexWithCollationFails {
public:
    void run() {
        auto opCtx = cc().makeOperationContext();
        DBDirectClient client(opCtx.get());
        client.dropCollection(_ns);
        IndexSpec indexSpec;
        indexSpec.addKey("a").addOptions(BSON("collation" << BSON("locale"
                                                                  << "fr")));
        client.createIndex(_ns, indexSpec);

        auto response = client.insertAcknowledged(
            _ns, {BSON("a" << BSON("b" << 99 << "c" << BSONSymbol("mySymbol")))});
        ASSERT_EQUALS(getStatusFromWriteCommandReply(response), ErrorCodes::CannotBuildIndexKeys);
        ASSERT_EQUALS(client.count(_nss), 0U);
    }
};

class InsertSymbolInsideNestedArrayIntoIndexWithCollationFails {
public:
    void run() {
        auto opCtx = cc().makeOperationContext();
        DBDirectClient client(opCtx.get());
        client.dropCollection(_ns);
        IndexSpec indexSpec;
        indexSpec.addKey("a").addOptions(BSON("collation" << BSON("locale"
                                                                  << "fr")));
        client.createIndex(_ns, indexSpec);

        auto response =
            client.insertAcknowledged(_ns, {BSON("a" << BSON_ARRAY(99 << BSONSymbol("mySymbol")))});
        ASSERT_EQUALS(getStatusFromWriteCommandReply(response), ErrorCodes::CannotBuildIndexKeys);
        ASSERT_EQUALS(client.count(_nss), 0U);
    }
};

class BuildingIndexWithCollationWhenSymbolDataExistsShouldFail {
public:
    void run() {
        auto opCtx = cc().makeOperationContext();
        DBDirectClient client(opCtx.get());
        client.dropCollection(_ns);
        client.insert(_ns, BSON("a" << BSON_ARRAY(99 << BSONSymbol("mySymbol"))));
        ASSERT_EQUALS(client.count(_nss), 1U);
        IndexSpec indexSpec;
        indexSpec.addKey("a").addOptions(BSON("collation" << BSON("locale"
                                                                  << "fr")));
        ASSERT_THROWS_CODE(client.createIndex(_ns, indexSpec),
                           AssertionException,
                           ErrorCodes::CannotBuildIndexKeys);
    }
};

class IndexingSymbolWithInheritedCollationShouldFail {
public:
    void run() {
        auto opCtx = cc().makeOperationContext();
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

        auto response =
            client.insertAcknowledged(_ns, {BSON("a" << BSON_ARRAY(99 << BSONSymbol("mySymbol")))});
        ASSERT_EQUALS(getStatusFromWriteCommandReply(response), ErrorCodes::CannotBuildIndexKeys);
    }
};

class IndexUpdateTests : public OldStyleSuiteSpecification {
public:
    IndexUpdateTests() : OldStyleSuiteSpecification("indexupdate") {}

    template <typename T>
    void addIf() {
        addNameCallback(nameForTestClass<T>(), [] { T().run(); });
    }

    void setupTests() {
        // These tests check that index creation ignores the unique constraint when told to.
        // The mobile storage engine does not support duplicate keys in unique indexes so these
        // tests are disabled.
        addIf<InsertBuildIgnoreUnique<true>>();
        addIf<InsertBuildIgnoreUnique<false>>();
        addIf<InsertBuildEnforceUnique<true>>();
        addIf<InsertBuildEnforceUnique<false>>();

        add<InsertBuildIndexInterrupt>();
        add<InsertBuildIdIndexInterrupt>();
        add<SameSpecDifferentOption>();
        add<SameSpecSameOptions>();
        add<DifferentSpecSameName>();
        add<SameSpecDifferentNameDifferentOrder>();
        add<SameSpecSameNameDifferentOrder>();
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
};

OldStyleSuiteInitializer<IndexUpdateTests> indexUpdateTests;

}  // namespace IndexUpdateTests
