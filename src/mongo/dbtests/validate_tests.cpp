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

#include "mongo/db/catalog/clustered_collection_util.h"
#include "mongo/db/catalog/collection_validation.h"
#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_build_interceptor.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/execution_context.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/dbtests/storage_debug_util.h"

namespace mongo {
namespace ValidateTests {
namespace {

const auto kIndexVersion = IndexDescriptor::IndexVersion::kV2;
const bool kTurnOnExtraLoggingForTest = true;

std::size_t omitTransientWarningsFromCount(const ValidateResults& results) {
    return std::count_if(
        results.warnings.begin(), results.warnings.end(), [](const std::string& elem) {
            std::string endMsg =
                "This is a transient issue as the collection was actively in use by other "
                "operations.";
            std::string beginMsg = "Could not complete validation of ";
            if (elem.size() >= std::max(endMsg.size(), beginMsg.size())) {
                bool startsWith = std::equal(beginMsg.begin(), beginMsg.end(), elem.begin());
                bool endsWith = std::equal(endMsg.rbegin(), endMsg.rend(), elem.rbegin());
                return !(startsWith && endsWith);
            } else {
                return true;
            }
        });
}

}  // namespace

static const char* const _ns = "unittests.validate_tests";

/**
 * Test fixture for a write locked test using collection _ns.  Includes functionality to
 * partially construct a new IndexDetails in a manner that supports proper cleanup in
 * dropCollection().
 */
class ValidateBase {
public:
    explicit ValidateBase(bool full, bool background, bool clustered)
        : _full(full), _background(background), _nss(_ns), _autoDb(nullptr), _db(nullptr) {

        CollectionOptions options;
        if (clustered) {
            options.clusteredIndex = clustered_util::makeCanonicalClusteredInfoForLegacyFormat();
        }

        const bool createIdIndex = !clustered;

        AutoGetCollection autoColl(&_opCtx, _nss, MODE_IX);
        auto db = autoColl.ensureDbExists(&_opCtx);
        ASSERT_TRUE(db) << _nss;

        WriteUnitOfWork wuow(&_opCtx);
        auto coll = db->createCollection(&_opCtx, _nss, options, createIdIndex);
        ASSERT_TRUE(coll) << _nss;
        wuow.commit();
    }

    explicit ValidateBase(bool full, bool background)
        : ValidateBase(full, background, /*clustered=*/false) {}

    ~ValidateBase() {
        AutoGetDb autoDb(&_opCtx, _nss.dbName(), MODE_X);
        auto db = autoDb.getDb();
        ASSERT_TRUE(db);

        WriteUnitOfWork wuow(&_opCtx);
        ASSERT_OK(db->dropCollection(&_opCtx, _nss));
        wuow.commit();

        getGlobalServiceContext()->unsetKillAllOperations();
    }

    // Helper to refetch the Collection from the catalog in order to see any changes made to it
    CollectionPtr coll() const {
        return CollectionCatalog::get(&_opCtx)->lookupCollectionByNamespace(&_opCtx, _nss);
    }

protected:
    ValidateResults runValidate() {
        // validate() will set a kCheckpoint read source. Callers continue to do operations after
        // running validate, so we must reset the read source back to normal before returning.
        auto originalReadSource = _opCtx.recoveryUnit()->getTimestampReadSource();
        ON_BLOCK_EXIT([&] {
            _opCtx.recoveryUnit()->abandonSnapshot();
            _opCtx.recoveryUnit()->setTimestampReadSource(originalReadSource);
        });

        auto mode = [&] {
            if (_background)
                return CollectionValidation::ValidateMode::kBackground;
            return _full ? CollectionValidation::ValidateMode::kForegroundFull
                         : CollectionValidation::ValidateMode::kForeground;
        }();
        auto repairMode = CollectionValidation::RepairMode::kNone;
        ValidateResults results;
        BSONObjBuilder output;

        ASSERT_OK(CollectionValidation::validate(
            &_opCtx, _nss, mode, repairMode, &results, &output, kTurnOnExtraLoggingForTest));

        //  Check if errors are reported if and only if valid is set to false.
        ASSERT_EQ(results.valid, results.errors.empty());

        if (_full) {
            BSONObj outputObj = output.done();
            bool allIndexesValid = true;
            for (auto elem : outputObj["indexDetails"].Obj()) {
                BSONObj indexDetail(elem.value());
                allIndexesValid = indexDetail["valid"].boolean() ? allIndexesValid : false;
            }
            ASSERT_EQ(results.valid, allIndexesValid);
        }

        return results;
    }

    void ensureValidateWorked() {
        ValidateResults results = runValidate();

        ScopeGuard dumpOnErrorGuard([&] {
            StorageDebugUtil::printValidateResults(results);
            StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, _nss);
        });

        ASSERT_TRUE(results.valid) << "Validation failed when it should've worked.";

        dumpOnErrorGuard.dismiss();
    }

    void ensureValidateFailed() {
        ValidateResults results = runValidate();

        ScopeGuard dumpOnErrorGuard([&] {
            StorageDebugUtil::printValidateResults(results);
            StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, _nss);
        });

        ASSERT_FALSE(results.valid) << "Validation worked when it should've failed.";

        dumpOnErrorGuard.dismiss();
    }

    void lockDb(LockMode mode) {
        _autoDb.reset();
        invariant(_opCtx.lockState()->isDbLockedForMode(_nss.dbName(), MODE_NONE));
        _autoDb.reset(new AutoGetDb(&_opCtx, _nss.dbName(), mode));
        invariant(_opCtx.lockState()->isDbLockedForMode(_nss.dbName(), mode));
        _db = _autoDb.get()->getDb();
    }

    void releaseDb() {
        _autoDb.reset();
        _db = nullptr;
        invariant(_opCtx.lockState()->isDbLockedForMode(_nss.dbName(), MODE_NONE));
    }

    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_txnPtr;
    bool _full;
    bool _background;
    const NamespaceString _nss;
    std::unique_ptr<AutoGetDb> _autoDb;
    Database* _db;
};

template <bool full, bool background>
class ValidateIdIndexCount : public ValidateBase {
public:
    ValidateIdIndexCount() : ValidateBase(full, background) {}

    void run() {
        if (_background) {
            return;
        }

        // Create a new collection, insert records {_id: 1} and {_id: 2} and check it's valid.
        lockDb(MODE_X);

        RecordId id1;
        {
            OpDebug* const nullOpDebug = nullptr;
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);

            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 1)), nullOpDebug, true));
            id1 = coll()->getCursor(&_opCtx)->next()->id;
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 2)), nullOpDebug, true));
            wunit.commit();
        }
        releaseDb();
        ensureValidateWorked();

        lockDb(MODE_X);
        RecordStore* rs = coll()->getRecordStore();

        // Remove {_id: 1} from the record store, so we get more _id entries than records.
        {
            WriteUnitOfWork wunit(&_opCtx);
            rs->deleteRecord(&_opCtx, id1);
            wunit.commit();
        }
        releaseDb();
        ensureValidateFailed();

        lockDb(MODE_X);

        // Insert records {_id: 0} and {_id: 1} , so we get too few _id entries, and verify
        // validate fails.
        {
            WriteUnitOfWork wunit(&_opCtx);
            for (int j = 0; j < 2; j++) {
                auto doc = BSON("_id" << j);
                ASSERT_OK(rs->insertRecord(&_opCtx, doc.objdata(), doc.objsize(), Timestamp()));
            }
            wunit.commit();
        }
        releaseDb();
        ensureValidateFailed();
    }
};

template <bool full, bool background>
class ValidateSecondaryIndexCount : public ValidateBase {
public:
    ValidateSecondaryIndexCount() : ValidateBase(full, background) {}
    void run() {
        if (_background) {
            return;
        }

        // Create a new collection, insert two documents.
        lockDb(MODE_X);
        RecordId id1;
        {
            OpDebug* const nullOpDebug = nullptr;
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 1 << "a" << 1)), nullOpDebug, true));
            id1 = coll()->getCursor(&_opCtx)->next()->id;
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 2 << "a" << 2)), nullOpDebug, true));
            wunit.commit();
        }

        auto status = dbtests::createIndexFromSpec(&_opCtx,
                                                   coll()->ns().ns(),
                                                   BSON("name"
                                                        << "a"
                                                        << "key" << BSON("a" << 1) << "v"
                                                        << static_cast<int>(kIndexVersion)
                                                        << "background" << false));

        ASSERT_OK(status);
        releaseDb();
        ensureValidateWorked();

        lockDb(MODE_X);
        RecordStore* rs = coll()->getRecordStore();

        // Remove a record, so we get more _id entries than records, and verify validate fails.
        {
            WriteUnitOfWork wunit(&_opCtx);
            rs->deleteRecord(&_opCtx, id1);
            wunit.commit();
        }
        releaseDb();
        ensureValidateFailed();

        lockDb(MODE_X);

        // Insert two more records, so we get too few entries for a non-sparse index, and
        // verify validate fails.
        {
            WriteUnitOfWork wunit(&_opCtx);
            for (int j = 0; j < 2; j++) {
                auto doc = BSON("_id" << j);
                ASSERT_OK(rs->insertRecord(&_opCtx, doc.objdata(), doc.objsize(), Timestamp()));
            }
            wunit.commit();
        }
        releaseDb();
        ensureValidateFailed();
    }
};

template <bool full, bool background>
class ValidateSecondaryIndex : public ValidateBase {
public:
    ValidateSecondaryIndex() : ValidateBase(full, background) {}
    void run() {
        if (_background) {
            return;
        }

        // Create a new collection, insert three records.
        lockDb(MODE_X);
        OpDebug* const nullOpDebug = nullptr;
        RecordId id1;
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 1 << "a" << 1)), nullOpDebug, true));
            id1 = coll()->getCursor(&_opCtx)->next()->id;
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 2 << "a" << 2)), nullOpDebug, true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 3 << "b" << 3)), nullOpDebug, true));
            wunit.commit();
        }

        auto status = dbtests::createIndexFromSpec(&_opCtx,
                                                   coll()->ns().ns(),
                                                   BSON("name"
                                                        << "a"
                                                        << "key" << BSON("a" << 1) << "v"
                                                        << static_cast<int>(kIndexVersion)
                                                        << "background" << false));

        ASSERT_OK(status);
        releaseDb();
        ensureValidateWorked();

        lockDb(MODE_X);
        RecordStore* rs = coll()->getRecordStore();

        // Update {a: 1} to {a: 9} without updating the index, so we get inconsistent values
        // between the index and the document. Verify validate fails.
        {
            WriteUnitOfWork wunit(&_opCtx);
            auto doc = BSON("_id" << 1 << "a" << 9);
            auto updateStatus = rs->updateRecord(&_opCtx, id1, doc.objdata(), doc.objsize());

            ASSERT_OK(updateStatus);
            wunit.commit();
        }
        releaseDb();
        ensureValidateFailed();
    }
};

template <bool full, bool background>
class ValidateIdIndex : public ValidateBase {
public:
    ValidateIdIndex() : ValidateBase(full, background) {}

    void run() {
        if (_background) {
            return;
        }

        // Create a new collection, insert records {_id: 1} and {_id: 2} and check it's valid.
        lockDb(MODE_X);
        OpDebug* const nullOpDebug = nullptr;
        RecordId id1;
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);

            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 1)), nullOpDebug, true));
            id1 = coll()->getCursor(&_opCtx)->next()->id;
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 2)), nullOpDebug, true));
            wunit.commit();
        }
        releaseDb();
        ensureValidateWorked();

        lockDb(MODE_X);
        RecordStore* rs = coll()->getRecordStore();

        // Update {_id: 1} to {_id: 9} without updating the index, so we get inconsistent values
        // between the index and the document. Verify validate fails.
        {
            WriteUnitOfWork wunit(&_opCtx);
            auto doc = BSON("_id" << 9);
            auto updateStatus = rs->updateRecord(&_opCtx, id1, doc.objdata(), doc.objsize());
            ASSERT_OK(updateStatus);
            wunit.commit();
        }
        releaseDb();
        ensureValidateFailed();

        lockDb(MODE_X);

        // Revert {_id: 9} to {_id: 1} and verify that validate succeeds.
        {
            WriteUnitOfWork wunit(&_opCtx);
            auto doc = BSON("_id" << 1);
            auto updateStatus = rs->updateRecord(&_opCtx, id1, doc.objdata(), doc.objsize());
            ASSERT_OK(updateStatus);
            wunit.commit();
        }
        releaseDb();
        ensureValidateWorked();

        lockDb(MODE_X);

        // Remove the {_id: 1} document and insert a new document without an index entry, so there
        // will still be the same number of index entries and documents, but one document will not
        // have an index entry.
        {
            WriteUnitOfWork wunit(&_opCtx);
            rs->deleteRecord(&_opCtx, id1);
            auto doc = BSON("_id" << 3);
            ASSERT_OK(
                rs->insertRecord(&_opCtx, doc.objdata(), doc.objsize(), Timestamp()).getStatus());
            wunit.commit();
        }
        releaseDb();
        ensureValidateFailed();
    }
};

template <bool full, bool background>
class ValidateMultiKeyIndex : public ValidateBase {
public:
    ValidateMultiKeyIndex() : ValidateBase(full, background) {}

    void run() {
        if (_background) {
            return;
        }

        // Create a new collection, insert three records and check it's valid.
        lockDb(MODE_X);
        OpDebug* const nullOpDebug = nullptr;
        RecordId id1;
        // {a: [b: 1, c: 2]}, {a: [b: 2, c: 2]}, {a: [b: 1, c: 1]}
        auto doc1 = BSON("_id" << 1 << "a" << BSON_ARRAY(BSON("b" << 1) << BSON("c" << 2)));
        auto doc1_b = BSON("_id" << 1 << "a" << BSON_ARRAY(BSON("b" << 2) << BSON("c" << 2)));
        auto doc1_c = BSON("_id" << 1 << "a" << BSON_ARRAY(BSON("b" << 1) << BSON("c" << 1)));

        // {a: [b: 2]}
        auto doc2 = BSON("_id" << 2 << "a" << BSON_ARRAY(BSON("b" << 2)));
        // {a: [c: 1]}
        auto doc3 = BSON("_id" << 3 << "a" << BSON_ARRAY(BSON("c" << 1)));
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);


            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(doc1), nullOpDebug, true));
            id1 = coll()->getCursor(&_opCtx)->next()->id;
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(doc2), nullOpDebug, true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(doc3), nullOpDebug, true));
            wunit.commit();
        }
        releaseDb();
        ensureValidateWorked();

        lockDb(MODE_X);

        // Create multi-key index.
        auto status = dbtests::createIndexFromSpec(&_opCtx,
                                                   coll()->ns().ns(),
                                                   BSON("name"
                                                        << "multikey_index"
                                                        << "key" << BSON("a.b" << 1) << "v"
                                                        << static_cast<int>(kIndexVersion)
                                                        << "background" << false));

        ASSERT_OK(status);
        releaseDb();
        ensureValidateWorked();

        lockDb(MODE_X);
        RecordStore* rs = coll()->getRecordStore();

        // Update a document's indexed field without updating the index.
        {
            WriteUnitOfWork wunit(&_opCtx);
            auto updateStatus = rs->updateRecord(&_opCtx, id1, doc1_b.objdata(), doc1_b.objsize());
            ASSERT_OK(updateStatus);
            wunit.commit();
        }
        releaseDb();
        ensureValidateFailed();

        lockDb(MODE_X);

        // Update a document's non-indexed field without updating the index.
        // Index validation should still be valid.
        {
            WriteUnitOfWork wunit(&_opCtx);
            auto updateStatus = rs->updateRecord(&_opCtx, id1, doc1_c.objdata(), doc1_c.objsize());
            ASSERT_OK(updateStatus);
            wunit.commit();
        }
        releaseDb();
        ensureValidateWorked();
    }
};

template <bool full, bool background>
class ValidateSparseIndex : public ValidateBase {
public:
    ValidateSparseIndex() : ValidateBase(full, background) {}

    void run() {
        if (_background) {
            return;
        }

        // Create a new collection, insert three records and check it's valid.
        lockDb(MODE_X);
        OpDebug* const nullOpDebug = nullptr;
        RecordId id1;
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);

            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 1 << "a" << 1)), nullOpDebug, true));
            id1 = coll()->getCursor(&_opCtx)->next()->id;
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 2 << "a" << 2)), nullOpDebug, true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 3 << "b" << 1)), nullOpDebug, true));
            wunit.commit();
        }

        // Create a sparse index.
        auto status =
            dbtests::createIndexFromSpec(&_opCtx,
                                         coll()->ns().ns(),
                                         BSON("name"
                                              << "sparse_index"
                                              << "key" << BSON("a" << 1) << "v"
                                              << static_cast<int>(kIndexVersion) << "background"
                                              << false << "sparse" << true));

        ASSERT_OK(status);
        releaseDb();
        ensureValidateWorked();

        lockDb(MODE_X);
        RecordStore* rs = coll()->getRecordStore();

        // Update a document's indexed field without updating the index.
        {
            WriteUnitOfWork wunit(&_opCtx);
            auto doc = BSON("_id" << 2 << "a" << 3);
            auto updateStatus = rs->updateRecord(&_opCtx, id1, doc.objdata(), doc.objsize());
            ASSERT_OK(updateStatus);
            wunit.commit();
        }
        releaseDb();
        ensureValidateFailed();
    }
};

template <bool full, bool background>
class ValidatePartialIndex : public ValidateBase {
public:
    ValidatePartialIndex() : ValidateBase(full, background) {}

    void run() {
        if (_background) {
            return;
        }

        // Create a new collection, insert three records and check it's valid.
        lockDb(MODE_X);
        OpDebug* const nullOpDebug = nullptr;
        RecordId id1;
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);

            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 1 << "a" << 1)), nullOpDebug, true));
            id1 = coll()->getCursor(&_opCtx)->next()->id;
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 2 << "a" << 2)), nullOpDebug, true));
            // Explicitly test that multi-key partial indexes containing documents that
            // don't match the filter expression are handled correctly.
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx,
                coll(),
                InsertStatement(BSON("_id" << 3 << "a" << BSON_ARRAY(-1 << -2 << -3))),
                nullOpDebug,
                true));
            wunit.commit();
        }

        // Create a partial index.
        auto status =
            dbtests::createIndexFromSpec(&_opCtx,
                                         coll()->ns().ns(),
                                         BSON("name"
                                              << "partial_index"
                                              << "key" << BSON("a" << 1) << "v"
                                              << static_cast<int>(kIndexVersion) << "background"
                                              << false << "partialFilterExpression"
                                              << BSON("a" << BSON("$gt" << 1))));

        ASSERT_OK(status);
        releaseDb();
        ensureValidateWorked();

        lockDb(MODE_X);
        RecordStore* rs = coll()->getRecordStore();

        // Update an unindexed document without updating the index.
        {
            WriteUnitOfWork wunit(&_opCtx);
            auto doc = BSON("_id" << 1);
            auto updateStatus = rs->updateRecord(&_opCtx, id1, doc.objdata(), doc.objsize());
            ASSERT_OK(updateStatus);
            wunit.commit();
        }
        releaseDb();
        ensureValidateWorked();
    }
};

template <bool full, bool background>
class ValidatePartialIndexOnCollectionWithNonIndexableFields : public ValidateBase {
public:
    ValidatePartialIndexOnCollectionWithNonIndexableFields() : ValidateBase(full, background) {}

    void run() {
        if (_background) {
            return;
        }

        // Create a new collection and insert a record that has a non-indexable value on the indexed
        // field.
        lockDb(MODE_X);
        OpDebug* const nullOpDebug = nullptr;

        RecordId id1;
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx,
                coll(),
                InsertStatement(BSON("_id" << 1 << "x" << 1 << "a" << 2)),
                nullOpDebug,
                true));
            wunit.commit();
        }

        // Create a partial geo index that indexes the document. This should return an error.
        ASSERT_NOT_OK(
            dbtests::createIndexFromSpec(&_opCtx,
                                         coll()->ns().ns(),
                                         BSON("name"
                                              << "partial_index"
                                              << "key"
                                              << BSON("x"
                                                      << "2dsphere")
                                              << "v" << static_cast<int>(kIndexVersion)
                                              << "background" << false << "partialFilterExpression"
                                              << BSON("a" << BSON("$eq" << 2)))));

        // Create a partial geo index that does not index the document.
        auto status =
            dbtests::createIndexFromSpec(&_opCtx,
                                         coll()->ns().ns(),
                                         BSON("name"
                                              << "partial_index"
                                              << "key"
                                              << BSON("x"
                                                      << "2dsphere")
                                              << "v" << static_cast<int>(kIndexVersion)
                                              << "background" << false << "partialFilterExpression"
                                              << BSON("a" << BSON("$eq" << 1))));
        ASSERT_OK(status);
        releaseDb();
        ensureValidateWorked();
    }
};

template <bool full, bool background>
class ValidateCompoundIndex : public ValidateBase {
public:
    ValidateCompoundIndex() : ValidateBase(full, background) {}

    void run() {
        if (_background) {
            return;
        }

        // Create a new collection, insert five records and check it's valid.
        lockDb(MODE_X);
        OpDebug* const nullOpDebug = nullptr;

        RecordId id1;
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);

            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx,
                coll(),
                InsertStatement(BSON("_id" << 1 << "a" << 1 << "b" << 4)),
                nullOpDebug,
                true));
            id1 = coll()->getCursor(&_opCtx)->next()->id;
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx,
                coll(),
                InsertStatement(BSON("_id" << 2 << "a" << 2 << "b" << 5)),
                nullOpDebug,
                true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 3 << "a" << 3)), nullOpDebug, true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 4 << "b" << 6)), nullOpDebug, true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 5 << "c" << 7)), nullOpDebug, true));
            wunit.commit();
        }

        // Create two compound indexes, one forward and one reverse, to test
        // validate()'s index direction parsing.
        auto status = dbtests::createIndexFromSpec(&_opCtx,
                                                   coll()->ns().ns(),
                                                   BSON("name"
                                                        << "compound_index_1"
                                                        << "key" << BSON("a" << 1 << "b" << -1)
                                                        << "v" << static_cast<int>(kIndexVersion)
                                                        << "background" << false));
        ASSERT_OK(status);

        status = dbtests::createIndexFromSpec(&_opCtx,
                                              coll()->ns().ns(),
                                              BSON("name"
                                                   << "compound_index_2"
                                                   << "key" << BSON("a" << -1 << "b" << 1) << "v"
                                                   << static_cast<int>(kIndexVersion)
                                                   << "background" << false));

        ASSERT_OK(status);
        releaseDb();
        ensureValidateWorked();

        lockDb(MODE_X);
        RecordStore* rs = coll()->getRecordStore();

        // Update a document's indexed field without updating the index.
        {
            WriteUnitOfWork wunit(&_opCtx);
            auto doc = BSON("_id" << 1 << "a" << 1 << "b" << 3);
            auto updateStatus = rs->updateRecord(&_opCtx, id1, doc.objdata(), doc.objsize());
            ASSERT_OK(updateStatus);
            wunit.commit();
        }
        releaseDb();
        ensureValidateFailed();
    }
};

template <bool full, bool background>
class ValidateIndexEntry : public ValidateBase {
public:
    ValidateIndexEntry() : ValidateBase(full, background) {}

    void run() {
        if (_background) {
            return;
        }

        SharedBufferFragmentBuilder pooledBuilder(
            KeyString::HeapBuilder::kHeapAllocatorDefaultBytes);

        // Create a new collection, insert three records and check it's valid.
        lockDb(MODE_X);
        OpDebug* const nullOpDebug = nullptr;

        RecordId id1;
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);

            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 1 << "a" << 1)), nullOpDebug, true));
            id1 = coll()->getCursor(&_opCtx)->next()->id;
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 2 << "a" << 2)), nullOpDebug, true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 3 << "b" << 1)), nullOpDebug, true));
            wunit.commit();
        }

        const std::string indexName = "bad_index";
        auto status = dbtests::createIndexFromSpec(
            &_opCtx,
            coll()->ns().ns(),
            BSON("name" << indexName << "key" << BSON("a" << 1) << "v"
                        << static_cast<int>(kIndexVersion) << "background" << false));

        ASSERT_OK(status);
        releaseDb();
        ensureValidateWorked();

        lockDb(MODE_X);

        // Replace a correct index entry with a bad one and check it's invalid.
        const IndexCatalog* indexCatalog = coll()->getIndexCatalog();
        auto descriptor = indexCatalog->findIndexByName(&_opCtx, indexName);
        auto iam = indexCatalog->getEntry(descriptor)->accessMethod()->asSortedData();

        {
            WriteUnitOfWork wunit(&_opCtx);
            int64_t numDeleted = 0;
            int64_t numInserted = 0;
            const BSONObj actualKey = BSON("a" << 1);
            const BSONObj badKey = BSON("a" << -1);
            InsertDeleteOptions options;
            options.dupsAllowed = true;
            options.logIfError = true;

            KeyStringSet keys;
            iam->getKeys(
                &_opCtx,
                coll(),
                pooledBuilder,
                actualKey,
                InsertDeleteOptions::ConstraintEnforcementMode::kRelaxConstraintsUnfiltered,
                SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                &keys,
                nullptr,
                nullptr,
                id1);

            auto removeStatus =
                iam->removeKeys(&_opCtx, {keys.begin(), keys.end()}, options, &numDeleted);
            auto insertStatus = iam->insert(&_opCtx,
                                            pooledBuilder,
                                            coll(),
                                            {{id1, Timestamp(), &badKey}},
                                            options,
                                            &numInserted);

            ASSERT_OK(removeStatus);
            ASSERT_OK(insertStatus);
            ASSERT_EQUALS(numDeleted, 1);
            ASSERT_EQUALS(numInserted, 1);
            wunit.commit();
        }
        releaseDb();
        ensureValidateFailed();
    }
};

class ValidateIndexMetadata : public ValidateBase {
public:
    ValidateIndexMetadata() : ValidateBase(/*full=*/false, /*background=*/false) {}

    void run() {
        SharedBufferFragmentBuilder pooledBuilder(
            KeyString::HeapBuilder::kHeapAllocatorDefaultBytes);

        // Create an index with bad index specs.
        lockDb(MODE_X);

        const std::string indexName = "bad_specs_index";
        auto status =
            dbtests::createIndexFromSpec(&_opCtx,
                                         coll()->ns().ns(),
                                         BSON("name" << indexName << "key" << BSON("a" << 1) << "v"
                                                     << static_cast<int>(kIndexVersion) << "sparse"
                                                     << "false"));

        ASSERT_OK(status);
        releaseDb();
        ensureValidateFailed();
    }
};

template <bool full, bool background>
class ValidateWildCardIndex : public ValidateBase {
public:
    ValidateWildCardIndex() : ValidateBase(full, background) {}

    void run() {
        if (_background) {
            return;
        }

        // Create a new collection.
        lockDb(MODE_X);

        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);
            wunit.commit();
        }

        // Create a $** index.
        const auto indexName = "wildcardIndex";
        const auto indexKey = BSON("$**" << 1);
        auto status = dbtests::createIndexFromSpec(
            &_opCtx,
            coll()->ns().ns(),
            BSON("name" << indexName << "key" << indexKey << "v" << static_cast<int>(kIndexVersion)
                        << "background" << false));
        ASSERT_OK(status);

        // Insert non-multikey documents.
        OpDebug* const nullOpDebug = nullptr;
        lockDb(MODE_X);
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx,
                coll(),
                InsertStatement(BSON("_id" << 1 << "a" << 1 << "b" << 1)),
                nullOpDebug,
                true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx,
                coll(),
                InsertStatement(BSON("_id" << 2 << "b" << BSON("0" << 1))),
                nullOpDebug,
                true));
            wunit.commit();
        }
        releaseDb();
        ensureValidateWorked();

        // Insert multikey documents.
        lockDb(MODE_X);
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx,
                coll(),
                InsertStatement(BSON("_id" << 3 << "mk_1" << BSON_ARRAY(1 << 2 << 3))),
                nullOpDebug,
                true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx,
                coll(),
                InsertStatement(BSON("_id" << 4 << "mk_2" << BSON_ARRAY(BSON("e" << 1)))),
                nullOpDebug,
                true));
            wunit.commit();
        }
        releaseDb();
        ensureValidateWorked();

        // Insert additional multikey path metadata index keys.
        lockDb(MODE_X);
        const RecordId recordId(record_id_helpers::reservedIdFor(
            record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, KeyFormat::Long));
        const IndexCatalog* indexCatalog = coll()->getIndexCatalog();
        auto descriptor = indexCatalog->findIndexByName(&_opCtx, indexName);
        auto accessMethod = indexCatalog->getEntry(descriptor)->accessMethod()->asSortedData();
        auto sortedDataInterface = accessMethod->getSortedDataInterface();
        {
            WriteUnitOfWork wunit(&_opCtx);
            const KeyString::Value indexKey =
                KeyString::HeapBuilder(sortedDataInterface->getKeyStringVersion(),
                                       BSON("" << 1 << ""
                                               << "non_existent_path"),
                                       sortedDataInterface->getOrdering(),
                                       recordId)
                    .release();
            auto insertStatus =
                sortedDataInterface->insert(&_opCtx, indexKey, true /* dupsAllowed */);
            ASSERT_OK(insertStatus);
            wunit.commit();
        }

        // An index whose set of multikey metadata paths is a superset of collection multikey
        // metadata paths is valid.
        releaseDb();
        ensureValidateWorked();

        // Remove the multikey path metadata index key for a path that exists and is multikey in the
        // collection.
        lockDb(MODE_X);
        {
            WriteUnitOfWork wunit(&_opCtx);
            const KeyString::Value indexKey =
                KeyString::HeapBuilder(sortedDataInterface->getKeyStringVersion(),
                                       BSON("" << 1 << ""
                                               << "mk_1"),
                                       sortedDataInterface->getOrdering(),
                                       recordId)
                    .release();
            sortedDataInterface->unindex(&_opCtx, indexKey, true /* dupsAllowed */);
            wunit.commit();
        }

        // An index that is missing one or more multikey metadata fields that exist in the
        // collection is not valid.
        releaseDb();
        ensureValidateFailed();
    }
};

template <bool full, bool background>
class ValidateWildCardIndexWithProjection : public ValidateBase {
public:
    ValidateWildCardIndexWithProjection() : ValidateBase(full, background) {}

    void run() {
        if (_background) {
            return;
        }

        // Create a new collection.
        lockDb(MODE_X);

        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);
            wunit.commit();
        }

        // Create a $** index with a projection on "a".
        const auto indexName = "wildcardIndex";
        const auto indexKey = BSON("a.$**" << 1);
        auto status = dbtests::createIndexFromSpec(
            &_opCtx,
            coll()->ns().ns(),
            BSON("name" << indexName << "key" << indexKey << "v" << static_cast<int>(kIndexVersion)
                        << "background" << false));
        ASSERT_OK(status);

        // Insert documents with indexed and not-indexed paths.
        OpDebug* const nullOpDebug = nullptr;
        lockDb(MODE_X);
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx,
                coll(),
                InsertStatement(BSON("_id" << 1 << "a" << 1 << "b" << 1)),
                nullOpDebug,
                true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx,
                coll(),
                InsertStatement(BSON("_id" << 2 << "a" << BSON("w" << 1))),
                nullOpDebug,
                true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx,
                coll(),
                InsertStatement(BSON("_id" << 3 << "a" << BSON_ARRAY("x" << 1))),
                nullOpDebug,
                true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 4 << "b" << 2)), nullOpDebug, true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx,
                coll(),
                InsertStatement(BSON("_id" << 5 << "b" << BSON("y" << 1))),
                nullOpDebug,
                true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx,
                coll(),
                InsertStatement(BSON("_id" << 6 << "b" << BSON_ARRAY("z" << 1))),
                nullOpDebug,
                true));
            wunit.commit();
        }
        releaseDb();
        ensureValidateWorked();

        lockDb(MODE_X);
        const IndexCatalog* indexCatalog = coll()->getIndexCatalog();
        auto descriptor = indexCatalog->findIndexByName(&_opCtx, indexName);
        auto accessMethod = indexCatalog->getEntry(descriptor)->accessMethod()->asSortedData();
        auto sortedDataInterface = accessMethod->getSortedDataInterface();

        // Removing a multikey metadata path for a path included in the projection causes validate
        // to fail.
        lockDb(MODE_X);
        {
            WriteUnitOfWork wunit(&_opCtx);
            RecordId recordId(record_id_helpers::reservedIdFor(
                record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, KeyFormat::Long));
            const KeyString::Value indexKey =
                KeyString::HeapBuilder(sortedDataInterface->getKeyStringVersion(),
                                       BSON("" << 1 << ""
                                               << "a"),
                                       sortedDataInterface->getOrdering(),
                                       recordId)
                    .release();
            sortedDataInterface->unindex(&_opCtx, indexKey, true /* dupsAllowed */);
            wunit.commit();
        }
        releaseDb();
        ensureValidateFailed();
    }
};

template <bool full, bool background>
class ValidateMissingAndExtraIndexEntryResults : public ValidateBase {
public:
    ValidateMissingAndExtraIndexEntryResults() : ValidateBase(full, background) {}

    void run() {
        if (_background) {
            return;
        }

        // Create a new collection.
        lockDb(MODE_X);

        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);
            wunit.commit();
        }

        // Create an index.
        const auto indexName = "a";
        const auto indexKey = BSON("a" << 1);
        auto status = dbtests::createIndexFromSpec(
            &_opCtx,
            coll()->ns().ns(),
            BSON("name" << indexName << "key" << indexKey << "v" << static_cast<int>(kIndexVersion)
                        << "background" << false));
        ASSERT_OK(status);

        // Insert documents.
        OpDebug* const nullOpDebug = nullptr;
        RecordId rid = RecordId::minLong();
        lockDb(MODE_X);
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 1 << "a" << 1)), nullOpDebug, true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 2 << "a" << 2)), nullOpDebug, true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 3 << "a" << 3)), nullOpDebug, true));
            rid = coll()->getCursor(&_opCtx)->next()->id;
            wunit.commit();
        }
        releaseDb();
        ensureValidateWorked();

        RecordStore* rs = coll()->getRecordStore();

        // Updating a document without updating the index entry should cause us to have a missing
        // index entry and an extra index entry.
        lockDb(MODE_X);
        {
            WriteUnitOfWork wunit(&_opCtx);
            auto doc = BSON("_id" << 1 << "a" << 5);
            auto updateStatus = rs->updateRecord(&_opCtx, rid, doc.objdata(), doc.objsize());
            ASSERT_OK(updateStatus);
            wunit.commit();
        }
        releaseDb();

        {
            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(
                CollectionValidation::validate(&_opCtx,
                                               _nss,
                                               CollectionValidation::ValidateMode::kForegroundFull,
                                               CollectionValidation::RepairMode::kNone,
                                               &results,
                                               &output,
                                               kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(false, results.valid);
            ASSERT_EQ(static_cast<size_t>(1), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(2), omitTransientWarningsFromCount(results));
            ASSERT_EQ(static_cast<size_t>(1), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(1), results.missingIndexEntries.size());

            dumpOnErrorGuard.dismiss();
        }
    }
};

template <bool full, bool background>
class ValidateMissingIndexEntryResults : public ValidateBase {
public:
    ValidateMissingIndexEntryResults() : ValidateBase(full, background) {}

    void run() {
        if (_background) {
            return;
        }

        SharedBufferFragmentBuilder pooledBuilder(
            KeyString::HeapBuilder::kHeapAllocatorDefaultBytes);

        // Create a new collection.
        lockDb(MODE_X);

        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);
            wunit.commit();
        }

        // Create an index.
        const auto indexName = "a";
        const auto indexKey = BSON("a" << 1);
        auto status = dbtests::createIndexFromSpec(
            &_opCtx,
            coll()->ns().ns(),
            BSON("name" << indexName << "key" << indexKey << "v" << static_cast<int>(kIndexVersion)
                        << "background" << false));
        ASSERT_OK(status);

        // Insert documents.
        OpDebug* const nullOpDebug = nullptr;
        RecordId rid = RecordId::minLong();
        lockDb(MODE_X);
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 1 << "a" << 1)), nullOpDebug, true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 2 << "a" << 2)), nullOpDebug, true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 3 << "a" << 3)), nullOpDebug, true));
            rid = coll()->getCursor(&_opCtx)->next()->id;
            wunit.commit();
        }
        releaseDb();
        ensureValidateWorked();

        // Removing an index entry without removing the document should cause us to have a missing
        // index entry.
        {
            lockDb(MODE_X);

            const IndexCatalog* indexCatalog = coll()->getIndexCatalog();
            auto descriptor = indexCatalog->findIndexByName(&_opCtx, indexName);
            auto iam = indexCatalog->getEntry(descriptor)->accessMethod()->asSortedData();

            WriteUnitOfWork wunit(&_opCtx);
            int64_t numDeleted;
            const BSONObj actualKey = BSON("a" << 1);
            InsertDeleteOptions options;
            options.logIfError = true;
            options.dupsAllowed = true;

            KeyStringSet keys;
            iam->getKeys(
                &_opCtx,
                coll(),
                pooledBuilder,
                actualKey,
                InsertDeleteOptions::ConstraintEnforcementMode::kRelaxConstraintsUnfiltered,
                SortedDataIndexAccessMethod::GetKeysContext::kRemovingKeys,
                &keys,
                nullptr,
                nullptr,
                rid);
            auto removeStatus =
                iam->removeKeys(&_opCtx, {keys.begin(), keys.end()}, options, &numDeleted);

            ASSERT_EQUALS(numDeleted, 1);
            ASSERT_OK(removeStatus);
            wunit.commit();

            releaseDb();
        }

        {
            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(
                CollectionValidation::validate(&_opCtx,
                                               _nss,
                                               CollectionValidation::ValidateMode::kForegroundFull,
                                               CollectionValidation::RepairMode::kNone,
                                               &results,
                                               &output,
                                               kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(false, results.valid);
            ASSERT_EQ(static_cast<size_t>(1), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(1), omitTransientWarningsFromCount(results));
            ASSERT_EQ(static_cast<size_t>(0), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(1), results.missingIndexEntries.size());

            dumpOnErrorGuard.dismiss();
        }
    }
};

template <bool full, bool background>
class ValidateExtraIndexEntryResults : public ValidateBase {
public:
    ValidateExtraIndexEntryResults() : ValidateBase(full, background) {}

    void run() {
        if (_background) {
            return;
        }

        // Create a new collection.
        lockDb(MODE_X);

        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);
            wunit.commit();
        }

        // Create an index.
        const auto indexName = "a";
        const auto indexKey = BSON("a" << 1);
        auto status = dbtests::createIndexFromSpec(
            &_opCtx,
            coll()->ns().ns(),
            BSON("name" << indexName << "key" << indexKey << "v" << static_cast<int>(kIndexVersion)
                        << "background" << false));
        ASSERT_OK(status);

        // Insert documents.
        OpDebug* const nullOpDebug = nullptr;
        RecordId rid = RecordId::minLong();
        lockDb(MODE_X);
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 1 << "a" << 1)), nullOpDebug, true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 2 << "a" << 2)), nullOpDebug, true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 3 << "a" << 3)), nullOpDebug, true));
            rid = coll()->getCursor(&_opCtx)->next()->id;
            wunit.commit();
        }
        releaseDb();
        ensureValidateWorked();

        // Removing a document without removing the index entries should cause us to have extra
        // index entries.
        {
            lockDb(MODE_X);
            RecordStore* rs = coll()->getRecordStore();

            WriteUnitOfWork wunit(&_opCtx);
            rs->deleteRecord(&_opCtx, rid);
            wunit.commit();
            releaseDb();
        }

        {
            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(
                CollectionValidation::validate(&_opCtx,
                                               _nss,
                                               CollectionValidation::ValidateMode::kForegroundFull,
                                               CollectionValidation::RepairMode::kNone,
                                               &results,
                                               &output,
                                               kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(false, results.valid);
            ASSERT_EQ(static_cast<size_t>(2), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(1), omitTransientWarningsFromCount(results));
            ASSERT_EQ(static_cast<size_t>(2), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), results.missingIndexEntries.size());

            dumpOnErrorGuard.dismiss();
        }
    }
};

class ValidateMissingAndExtraIndexEntryRepair : public ValidateBase {
public:
    // No need to test with background validation as repair mode is not supported in background
    // validation.
    ValidateMissingAndExtraIndexEntryRepair()
        : ValidateBase(/*full=*/false, /*background=*/false) {}

    void run() {
        // Create a new collection.
        lockDb(MODE_X);

        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);
            wunit.commit();
        }

        // Create index "a".
        const auto indexNameA = "a";
        const auto indexKeyA = BSON("a" << 1);
        ASSERT_OK(
            dbtests::createIndexFromSpec(&_opCtx,
                                         coll()->ns().ns(),
                                         BSON("name" << indexNameA << "key" << indexKeyA << "v"
                                                     << static_cast<int>(kIndexVersion))));

        // Create index "b".
        const auto indexNameB = "b";
        const auto indexKeyB = BSON("b" << 1);
        ASSERT_OK(
            dbtests::createIndexFromSpec(&_opCtx,
                                         coll()->ns().ns(),
                                         BSON("name" << indexNameB << "key" << indexKeyB << "v"
                                                     << static_cast<int>(kIndexVersion))));

        // Insert documents.
        OpDebug* const nullOpDebug = nullptr;
        ;
        lockDb(MODE_X);
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx,
                coll(),
                InsertStatement(BSON("_id" << 1 << "a" << 1 << "b" << 1)),
                nullOpDebug,
                true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx,
                coll(),
                InsertStatement(BSON("_id" << 2 << "a" << 3 << "b" << 3)),
                nullOpDebug,
                true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx,
                coll(),
                InsertStatement(BSON("_id" << 3 << "a" << 6 << "b" << 6)),
                nullOpDebug,
                true));
            wunit.commit();
        }
        releaseDb();
        ensureValidateWorked();

        RecordStore* rs = coll()->getRecordStore();

        // Updating documents without updating the index entries should cause us to have missing and
        // extra index entries.
        lockDb(MODE_X);
        {
            WriteUnitOfWork wunit(&_opCtx);
            auto doc1 = BSON("_id" << 1 << "a" << 8 << "b" << 8);
            auto doc2 = BSON("_id" << 2 << "a" << 3 << "b" << 7);
            std::unique_ptr<SeekableRecordCursor> cursor = coll()->getCursor(&_opCtx);
            auto record = cursor->next();
            RecordId rid = record->id;
            ASSERT_OK(rs->updateRecord(&_opCtx, rid, doc1.objdata(), doc1.objsize()));
            record = cursor->next();
            rid = record->id;
            ASSERT_OK(rs->updateRecord(&_opCtx, rid, doc2.objdata(), doc2.objsize()));
            wunit.commit();
        }
        releaseDb();

        // Confirm missing and extra index entries are detected.
        {
            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(
                CollectionValidation::validate(&_opCtx,
                                               _nss,
                                               CollectionValidation::ValidateMode::kForegroundFull,
                                               CollectionValidation::RepairMode::kNone,
                                               &results,
                                               &output,
                                               kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(false, results.valid);
            ASSERT_EQ(static_cast<size_t>(2), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(2), omitTransientWarningsFromCount(results));
            ASSERT_EQ(static_cast<size_t>(3), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(3), results.missingIndexEntries.size());

            dumpOnErrorGuard.dismiss();
        }

        // Run validate with repair, expect extra index entries are removed and missing index
        // entries are inserted.
        {
            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(
                CollectionValidation::validate(&_opCtx,
                                               _nss,
                                               CollectionValidation::ValidateMode::kForegroundFull,
                                               CollectionValidation::RepairMode::kFixErrors,
                                               &results,
                                               &output,
                                               kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });


            ASSERT_EQ(true, results.valid);
            ASSERT_EQ(true, results.repaired);
            ASSERT_EQ(static_cast<size_t>(0), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(2), omitTransientWarningsFromCount(results));
            ASSERT_EQ(static_cast<size_t>(0), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), results.missingIndexEntries.size());
            ASSERT_EQ(3, results.numRemovedExtraIndexEntries);
            ASSERT_EQ(3, results.numInsertedMissingIndexEntries);

            ASSERT_EQ(3, results.indexResultsMap[indexNameA].keysTraversed);
            ASSERT_EQ(3, results.indexResultsMap[indexNameB].keysTraversed);

            dumpOnErrorGuard.dismiss();
        }

        // Confirm repair worked such that results is now valid and no errors were suppressed by
        // repair.
        {
            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(
                CollectionValidation::validate(&_opCtx,
                                               _nss,
                                               CollectionValidation::ValidateMode::kForegroundFull,
                                               CollectionValidation::RepairMode::kFixErrors,
                                               &results,
                                               &output,
                                               kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.valid);
            ASSERT_EQ(false, results.repaired);
            ASSERT_EQ(static_cast<size_t>(0), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(0), omitTransientWarningsFromCount(results));
            ASSERT_EQ(static_cast<size_t>(0), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), results.missingIndexEntries.size());
            ASSERT_EQ(0, results.numRemovedExtraIndexEntries);
            ASSERT_EQ(0, results.numInsertedMissingIndexEntries);

            dumpOnErrorGuard.dismiss();
        }
    }
};

class ValidateMissingIndexEntryRepair : public ValidateBase {
public:
    // No need to test with background validation as repair mode is not supported in background
    // validation.
    ValidateMissingIndexEntryRepair() : ValidateBase(/*full=*/false, /*background=*/false) {}

    void run() {
        SharedBufferFragmentBuilder pooledBuilder(
            KeyString::HeapBuilder::kHeapAllocatorDefaultBytes);

        // Create a new collection.
        lockDb(MODE_X);

        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);
            wunit.commit();
        }

        // Create an index.
        const auto indexName = "a";
        const auto indexKey = BSON("a" << 1);
        auto status =
            dbtests::createIndexFromSpec(&_opCtx,
                                         coll()->ns().ns(),
                                         BSON("name" << indexName << "key" << indexKey << "v"
                                                     << static_cast<int>(kIndexVersion)));
        ASSERT_OK(status);

        // Insert documents.
        OpDebug* const nullOpDebug = nullptr;
        RecordId rid = RecordId::minLong();
        lockDb(MODE_X);
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 1 << "a" << 1)), nullOpDebug, true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 2 << "a" << 2)), nullOpDebug, true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 3 << "a" << 3)), nullOpDebug, true));
            rid = coll()->getCursor(&_opCtx)->next()->id;
            wunit.commit();
        }
        releaseDb();
        ensureValidateWorked();

        // Removing an index entry without removing the document should cause us to have a missing
        // index entry.
        {
            lockDb(MODE_X);

            const IndexCatalog* indexCatalog = coll()->getIndexCatalog();
            auto descriptor = indexCatalog->findIndexByName(&_opCtx, indexName);
            auto iam = indexCatalog->getEntry(descriptor)->accessMethod()->asSortedData();

            WriteUnitOfWork wunit(&_opCtx);
            int64_t numDeleted;
            const BSONObj actualKey = BSON("a" << 1);
            InsertDeleteOptions options;
            options.logIfError = true;
            options.dupsAllowed = true;

            KeyStringSet keys;
            iam->getKeys(
                &_opCtx,
                coll(),
                pooledBuilder,
                actualKey,
                InsertDeleteOptions::ConstraintEnforcementMode::kRelaxConstraintsUnfiltered,
                SortedDataIndexAccessMethod::GetKeysContext::kRemovingKeys,
                &keys,
                nullptr,
                nullptr,
                rid);
            auto removeStatus =
                iam->removeKeys(&_opCtx, {keys.begin(), keys.end()}, options, &numDeleted);

            ASSERT_EQUALS(numDeleted, 1);
            ASSERT_OK(removeStatus);
            wunit.commit();

            releaseDb();
        }

        // Confirm validate detects missing index entries.
        {
            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(
                CollectionValidation::validate(&_opCtx,
                                               _nss,
                                               CollectionValidation::ValidateMode::kForegroundFull,
                                               CollectionValidation::RepairMode::kNone,
                                               &results,
                                               &output,
                                               kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(false, results.valid);
            ASSERT_EQ(false, results.repaired);
            ASSERT_EQ(static_cast<size_t>(1), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(1), omitTransientWarningsFromCount(results));
            ASSERT_EQ(static_cast<size_t>(0), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(1), results.missingIndexEntries.size());
            ASSERT_EQ(0, results.numRemovedExtraIndexEntries);
            ASSERT_EQ(0, results.numInsertedMissingIndexEntries);

            dumpOnErrorGuard.dismiss();
        }

        // Run validate with repair, expect missing index entries are inserted.
        {
            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(
                CollectionValidation::validate(&_opCtx,
                                               _nss,
                                               CollectionValidation::ValidateMode::kForegroundFull,
                                               CollectionValidation::RepairMode::kFixErrors,
                                               &results,
                                               &output,
                                               kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.valid);
            ASSERT_EQ(true, results.valid);
            ASSERT_EQ(static_cast<size_t>(0), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(1), omitTransientWarningsFromCount(results));
            ASSERT_EQ(static_cast<size_t>(0), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), results.missingIndexEntries.size());
            ASSERT_EQ(0, results.numRemovedExtraIndexEntries);
            ASSERT_EQ(1, results.numInsertedMissingIndexEntries);

            dumpOnErrorGuard.dismiss();
        }

        // Confirm missing index entries have been inserted such that results are valid and no
        // errors were suppressed by repair.
        {
            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(
                CollectionValidation::validate(&_opCtx,
                                               _nss,
                                               CollectionValidation::ValidateMode::kForegroundFull,
                                               CollectionValidation::RepairMode::kNone,
                                               &results,
                                               &output,
                                               kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.valid);
            ASSERT_EQ(false, results.repaired);
            ASSERT_EQ(static_cast<size_t>(0), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(0), omitTransientWarningsFromCount(results));
            ASSERT_EQ(static_cast<size_t>(0), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), results.missingIndexEntries.size());
            ASSERT_EQ(0, results.numRemovedExtraIndexEntries);
            ASSERT_EQ(0, results.numInsertedMissingIndexEntries);

            dumpOnErrorGuard.dismiss();
        }
    }
};

class ValidateExtraIndexEntryRepair : public ValidateBase {
public:
    // No need to test with background validation as repair mode is not supported in background
    // validation.
    ValidateExtraIndexEntryRepair() : ValidateBase(/*full=*/false, /*background=*/false) {}

    void run() {
        // Create a new collection.
        lockDb(MODE_X);

        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);
            wunit.commit();
        }

        // Create an index.
        const auto indexName = "a";
        const auto indexKey = BSON("a" << 1);
        auto status =
            dbtests::createIndexFromSpec(&_opCtx,
                                         coll()->ns().ns(),
                                         BSON("name" << indexName << "key" << indexKey << "v"
                                                     << static_cast<int>(kIndexVersion)));
        ASSERT_OK(status);

        // Insert documents.
        OpDebug* const nullOpDebug = nullptr;
        RecordId rid = RecordId::minLong();
        lockDb(MODE_X);
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 1 << "a" << 1)), nullOpDebug, true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 2 << "a" << 2)), nullOpDebug, true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 3 << "a" << 3)), nullOpDebug, true));
            rid = coll()->getCursor(&_opCtx)->next()->id;
            wunit.commit();
        }
        releaseDb();
        ensureValidateWorked();

        // Removing a document without removing the index entries should cause us to have extra
        // index entries.
        {
            lockDb(MODE_X);
            RecordStore* rs = coll()->getRecordStore();

            WriteUnitOfWork wunit(&_opCtx);
            rs->deleteRecord(&_opCtx, rid);
            wunit.commit();
            releaseDb();
        }

        // Confirm validation detects extra index entries error.
        {
            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(
                CollectionValidation::validate(&_opCtx,
                                               _nss,
                                               CollectionValidation::ValidateMode::kForegroundFull,
                                               CollectionValidation::RepairMode::kNone,
                                               &results,
                                               &output,
                                               kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(false, results.valid);
            ASSERT_EQ(false, results.repaired);
            ASSERT_EQ(static_cast<size_t>(2), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(1), omitTransientWarningsFromCount(results));
            ASSERT_EQ(static_cast<size_t>(2), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), results.missingIndexEntries.size());
            ASSERT_EQ(0, results.numRemovedExtraIndexEntries);
            ASSERT_EQ(0, results.numInsertedMissingIndexEntries);

            dumpOnErrorGuard.dismiss();
        }

        // Run validate with repair, expect extra index entries are removed.
        {
            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(
                CollectionValidation::validate(&_opCtx,
                                               _nss,
                                               CollectionValidation::ValidateMode::kForegroundFull,
                                               CollectionValidation::RepairMode::kFixErrors,
                                               &results,
                                               &output,
                                               kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.valid);
            ASSERT_EQ(true, results.repaired);
            ASSERT_EQ(static_cast<size_t>(0), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(1), omitTransientWarningsFromCount(results));
            ASSERT_EQ(static_cast<size_t>(0), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), results.missingIndexEntries.size());
            ASSERT_EQ(2, results.numRemovedExtraIndexEntries);
            ASSERT_EQ(0, results.numInsertedMissingIndexEntries);

            dumpOnErrorGuard.dismiss();
        }

        // Confirm extra index entries are removed such that results are valid.
        {
            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(
                CollectionValidation::validate(&_opCtx,
                                               _nss,
                                               CollectionValidation::ValidateMode::kForegroundFull,
                                               CollectionValidation::RepairMode::kNone,
                                               &results,
                                               &output,
                                               kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.valid);
            ASSERT_EQ(false, results.repaired);
            ASSERT_EQ(static_cast<size_t>(0), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(0), omitTransientWarningsFromCount(results));
            ASSERT_EQ(static_cast<size_t>(0), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), results.missingIndexEntries.size());
            ASSERT_EQ(0, results.numRemovedExtraIndexEntries);
            ASSERT_EQ(0, results.numInsertedMissingIndexEntries);

            dumpOnErrorGuard.dismiss();
        }
    }
};

class ValidateDuplicateDocumentMissingIndexEntryRepair : public ValidateBase {
public:
    // No need to test with background validation as repair mode is not supported in background
    // validation.
    ValidateDuplicateDocumentMissingIndexEntryRepair()
        : ValidateBase(/*full=*/false, /*background=*/false) {}

    void run() {
        SharedBufferFragmentBuilder pooledBuilder(
            KeyString::HeapBuilder::kHeapAllocatorDefaultBytes);

        // Create a new collection and insert a document.
        lockDb(MODE_X);

        OpDebug* const nullOpDebug = nullptr;
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 1 << "a" << 1)), nullOpDebug, true));
            wunit.commit();
        }

        // Create a unique index.
        const auto indexName = "a";
        {
            const auto indexKey = BSON("a" << 1);
            auto status = dbtests::createIndexFromSpec(
                &_opCtx,
                coll()->ns().ns(),
                BSON("name" << indexName << "key" << indexKey << "v"
                            << static_cast<int>(kIndexVersion) << "unique" << true));
            ASSERT_OK(status);
        }

        // Confirm that inserting a document with the same value for "a" fails, verifying the
        // uniqueness constraint.
        BSONObj dupObj = BSON("_id" << 2 << "a" << 1);
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_NOT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(dupObj), nullOpDebug, true));
        }
        releaseDb();
        ensureValidateWorked();

        // Insert a document with a duplicate key for "a" but do not insert key into index a.
        RecordId rid;
        {
            lockDb(MODE_X);

            const IndexCatalog* indexCatalog = coll()->getIndexCatalog();

            InsertDeleteOptions options;
            options.logIfError = true;
            options.dupsAllowed = true;

            WriteUnitOfWork wunit(&_opCtx);

            // Insert a record and its keys separately. We do this to bypass duplicate constraint
            // checking. Inserting a record and its keys ensures that validation fails
            // because there are duplicate keys, and not just because there are keys without
            // corresponding records.
            auto swRecordId = coll()->getRecordStore()->insertRecord(
                &_opCtx, dupObj.objdata(), dupObj.objsize(), Timestamp());
            ASSERT_OK(swRecordId);
            rid = swRecordId.getValue();

            wunit.commit();

            // Insert the key on _id.
            {
                auto descriptor = indexCatalog->findIdIndex(&_opCtx);
                auto entry = const_cast<IndexCatalogEntry*>(indexCatalog->getEntry(descriptor));
                auto iam = entry->accessMethod()->asSortedData();
                auto interceptor = std::make_unique<IndexBuildInterceptor>(&_opCtx, entry);

                KeyStringSet keys;
                iam->getKeys(&_opCtx,
                             coll(),
                             pooledBuilder,
                             dupObj,
                             InsertDeleteOptions::ConstraintEnforcementMode::kRelaxConstraints,
                             SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                             &keys,
                             nullptr,
                             nullptr,
                             swRecordId.getValue());
                ASSERT_EQ(1, keys.size());

                {
                    WriteUnitOfWork wunit(&_opCtx);

                    int64_t numInserted;
                    auto insertStatus = iam->insertKeysAndUpdateMultikeyPaths(
                        &_opCtx,
                        coll(),
                        {keys.begin(), keys.end()},
                        {},
                        MultikeyPaths{},
                        options,
                        [this, &interceptor](const KeyString::Value& duplicateKey) {
                            return interceptor->recordDuplicateKey(&_opCtx, duplicateKey);
                        },
                        &numInserted);

                    ASSERT_EQUALS(numInserted, 1);
                    ASSERT_OK(insertStatus);

                    wunit.commit();
                }

                ASSERT_OK(interceptor->checkDuplicateKeyConstraints(&_opCtx));
            }

            releaseDb();
        }

        // Confirm validation detects missing index entry.
        {
            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(
                CollectionValidation::validate(&_opCtx,
                                               _nss,
                                               CollectionValidation::ValidateMode::kForegroundFull,
                                               CollectionValidation::RepairMode::kNone,
                                               &results,
                                               &output,
                                               kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(false, results.valid);
            ASSERT_EQ(false, results.repaired);
            ASSERT_EQ(static_cast<size_t>(1), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(1), omitTransientWarningsFromCount(results));
            ASSERT_EQ(static_cast<size_t>(0), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(1), results.missingIndexEntries.size());

            dumpOnErrorGuard.dismiss();
        }

        // Run validate with repair, expect missing index entry of duplicate document is removed
        // from record store and results are valid.
        {
            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(
                CollectionValidation::validate(&_opCtx,
                                               _nss,
                                               CollectionValidation::ValidateMode::kForegroundFull,
                                               CollectionValidation::RepairMode::kFixErrors,
                                               &results,
                                               &output,
                                               kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.valid);
            ASSERT_EQ(true, results.repaired);
            ASSERT_EQ(static_cast<size_t>(0), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(1), omitTransientWarningsFromCount(results));
            ASSERT_EQ(static_cast<size_t>(0), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), results.missingIndexEntries.size());
            ASSERT_EQ(0, results.numRemovedExtraIndexEntries);
            ASSERT_EQ(0, results.numInsertedMissingIndexEntries);
            ASSERT_EQ(1, results.numDocumentsMovedToLostAndFound);

            ASSERT_EQ(1, results.indexResultsMap[indexName].keysRemovedFromRecordStore);

            dumpOnErrorGuard.dismiss();
        }

        // Confirm duplicate document of missing index entries are removed such that results are
        // valid.
        {
            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(
                CollectionValidation::validate(&_opCtx,
                                               _nss,
                                               CollectionValidation::ValidateMode::kForegroundFull,
                                               CollectionValidation::RepairMode::kNone,
                                               &results,
                                               &output,
                                               kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.valid);
            ASSERT_EQ(false, results.repaired);
            ASSERT_EQ(static_cast<size_t>(0), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(0), omitTransientWarningsFromCount(results));
            ASSERT_EQ(static_cast<size_t>(0), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), results.missingIndexEntries.size());
            ASSERT_EQ(0, results.numRemovedExtraIndexEntries);
            ASSERT_EQ(0, results.numInsertedMissingIndexEntries);
            ASSERT_EQ(0, results.numDocumentsMovedToLostAndFound);

            ASSERT_EQ(0, results.indexResultsMap[indexName].keysRemovedFromRecordStore);

            dumpOnErrorGuard.dismiss();
        }
    }
};

class ValidateDoubleDuplicateDocumentMissingIndexEntryRepair : public ValidateBase {
public:
    // No need to test with background validation as repair mode is not supported in background
    // validation.
    ValidateDoubleDuplicateDocumentMissingIndexEntryRepair()
        : ValidateBase(/*full=*/false, /*background=*/false) {}

    void run() {
        SharedBufferFragmentBuilder pooledBuilder(
            KeyString::HeapBuilder::kHeapAllocatorDefaultBytes);

        // Create a new collection and insert a document.
        lockDb(MODE_X);

        OpDebug* const nullOpDebug = nullptr;
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx,
                coll(),
                InsertStatement(BSON("_id" << 1 << "a" << 1 << "b" << 1)),
                nullOpDebug,
                true));
            wunit.commit();
        }

        // Create unique indexes.
        const auto indexNameA = "a";
        {
            const auto indexKeyA = BSON("a" << 1);
            auto status = dbtests::createIndexFromSpec(
                &_opCtx,
                coll()->ns().ns(),
                BSON("name" << indexNameA << "key" << indexKeyA << "v"
                            << static_cast<int>(kIndexVersion) << "unique" << true));
            ASSERT_OK(status);
        }

        const auto indexNameB = "b";
        {
            const auto indexKeyB = BSON("b" << 1);
            auto status = dbtests::createIndexFromSpec(
                &_opCtx,
                coll()->ns().ns(),
                BSON("name" << indexNameB << "key" << indexKeyB << "v"
                            << static_cast<int>(kIndexVersion) << "unique" << true));
            ASSERT_OK(status);
        }


        // Confirm that inserting a document with the same value for "a" and "b" fails, verifying
        // the uniqueness constraint.
        BSONObj dupObj = BSON("_id" << 2 << "a" << 1 << "b" << 1);
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_NOT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(dupObj), nullOpDebug, true));
        }
        releaseDb();
        ensureValidateWorked();


        // Insert a document with a duplicate key for "a" and "b" but do not insert into respective
        // indexes.
        RecordId rid;
        {
            lockDb(MODE_X);

            const IndexCatalog* indexCatalog = coll()->getIndexCatalog();

            InsertDeleteOptions options;
            options.logIfError = true;
            options.dupsAllowed = true;

            WriteUnitOfWork wunit(&_opCtx);

            // Insert a record and its keys separately. We do this to bypass duplicate constraint
            // checking. Inserting a record without inserting keys results in the duplicate record
            // to be missing from both unique indexes.
            auto swRecordId = coll()->getRecordStore()->insertRecord(
                &_opCtx, dupObj.objdata(), dupObj.objsize(), Timestamp());
            ASSERT_OK(swRecordId);
            rid = swRecordId.getValue();

            wunit.commit();

            // Insert the key on _id.
            {
                auto descriptor = indexCatalog->findIdIndex(&_opCtx);
                auto entry = const_cast<IndexCatalogEntry*>(indexCatalog->getEntry(descriptor));
                auto iam = entry->accessMethod()->asSortedData();
                auto interceptor = std::make_unique<IndexBuildInterceptor>(&_opCtx, entry);

                KeyStringSet keys;
                iam->getKeys(&_opCtx,
                             coll(),
                             pooledBuilder,
                             dupObj,
                             InsertDeleteOptions::ConstraintEnforcementMode::kRelaxConstraints,
                             SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                             &keys,
                             nullptr,
                             nullptr,
                             swRecordId.getValue());
                ASSERT_EQ(1, keys.size());

                {
                    WriteUnitOfWork wunit(&_opCtx);

                    int64_t numInserted;
                    auto insertStatus = iam->insertKeysAndUpdateMultikeyPaths(
                        &_opCtx,
                        coll(),
                        {keys.begin(), keys.end()},
                        {},
                        MultikeyPaths{},
                        options,
                        [this, &interceptor](const KeyString::Value& duplicateKey) {
                            return interceptor->recordDuplicateKey(&_opCtx, duplicateKey);
                        },
                        &numInserted);

                    ASSERT_EQUALS(numInserted, 1);
                    ASSERT_OK(insertStatus);

                    wunit.commit();
                }

                ASSERT_OK(interceptor->checkDuplicateKeyConstraints(&_opCtx));
            }

            releaseDb();
        }

        // Confirm validation detects missing index entries.
        {
            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(
                CollectionValidation::validate(&_opCtx,
                                               _nss,
                                               CollectionValidation::ValidateMode::kForegroundFull,
                                               CollectionValidation::RepairMode::kNone,
                                               &results,
                                               &output,
                                               kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(false, results.valid);
            ASSERT_EQ(false, results.repaired);
            ASSERT_EQ(static_cast<size_t>(2), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(1), omitTransientWarningsFromCount(results));
            ASSERT_EQ(static_cast<size_t>(0), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(2), results.missingIndexEntries.size());

            dumpOnErrorGuard.dismiss();
        }

        // Run validate with repair, expect missing index entry document is removed from record
        // store and no action is taken on outdated missing index entry. Results should be valid.
        {
            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(
                CollectionValidation::validate(&_opCtx,
                                               _nss,
                                               CollectionValidation::ValidateMode::kForegroundFull,
                                               CollectionValidation::RepairMode::kFixErrors,
                                               &results,
                                               &output,
                                               kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.valid);
            ASSERT_EQ(true, results.repaired);
            ASSERT_EQ(static_cast<size_t>(0), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(1), omitTransientWarningsFromCount(results));
            ASSERT_EQ(static_cast<size_t>(0), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), results.missingIndexEntries.size());
            ASSERT_EQ(0, results.numRemovedExtraIndexEntries);
            ASSERT_EQ(0, results.numInsertedMissingIndexEntries);
            ASSERT_EQ(1, results.numDocumentsMovedToLostAndFound);
            ASSERT_EQ(1, results.numOutdatedMissingIndexEntry);

            ASSERT_EQ(1, results.indexResultsMap[indexNameA].keysRemovedFromRecordStore);
            ASSERT_EQ(0, results.indexResultsMap[indexNameB].keysRemovedFromRecordStore);

            dumpOnErrorGuard.dismiss();
        }

        // Confirm extra index entries are removed such that results are valid.
        {
            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(
                CollectionValidation::validate(&_opCtx,
                                               _nss,
                                               CollectionValidation::ValidateMode::kForegroundFull,
                                               CollectionValidation::RepairMode::kNone,
                                               &results,
                                               &output,
                                               kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.valid);
            ASSERT_EQ(false, results.repaired);
            ASSERT_EQ(static_cast<size_t>(0), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(0), omitTransientWarningsFromCount(results));
            ASSERT_EQ(static_cast<size_t>(0), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), results.missingIndexEntries.size());
            ASSERT_EQ(0, results.numRemovedExtraIndexEntries);
            ASSERT_EQ(0, results.numInsertedMissingIndexEntries);
            ASSERT_EQ(0, results.numDocumentsMovedToLostAndFound);
            ASSERT_EQ(0, results.numOutdatedMissingIndexEntry);

            ASSERT_EQ(0, results.indexResultsMap[indexNameA].keysRemovedFromRecordStore);
            ASSERT_EQ(0, results.indexResultsMap[indexNameB].keysRemovedFromRecordStore);

            dumpOnErrorGuard.dismiss();
        }
    }
};

class ValidateDoubleDuplicateDocumentOppositeMissingIndexEntryRepair : public ValidateBase {
public:
    // No need to test with background validation as repair mode is not supported in background
    // validation.
    ValidateDoubleDuplicateDocumentOppositeMissingIndexEntryRepair()
        : ValidateBase(/*full=*/false, /*background=*/false) {}

    void run() {
        SharedBufferFragmentBuilder pooledBuilder(
            KeyString::HeapBuilder::kHeapAllocatorDefaultBytes);

        // Create a new collection and insert a document.
        lockDb(MODE_X);

        OpDebug* const nullOpDebug = nullptr;
        RecordId rid1 = RecordId::minLong();
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx,
                coll(),
                InsertStatement(BSON("_id" << 1 << "a" << 1 << "b" << 1)),
                nullOpDebug,
                true));
            rid1 = coll()->getCursor(&_opCtx)->next()->id;
            wunit.commit();
        }

        // Create unique indexes.
        const auto indexNameA = "a";
        {
            const auto indexKeyA = BSON("a" << 1);
            auto status = dbtests::createIndexFromSpec(
                &_opCtx,
                coll()->ns().ns(),
                BSON("name" << indexNameA << "key" << indexKeyA << "v"
                            << static_cast<int>(kIndexVersion) << "unique" << true));
            ASSERT_OK(status);
        }

        const auto indexNameB = "b";
        {
            const auto indexKeyB = BSON("b" << 1);
            auto status = dbtests::createIndexFromSpec(
                &_opCtx,
                coll()->ns().ns(),
                BSON("name" << indexNameB << "key" << indexKeyB << "v"
                            << static_cast<int>(kIndexVersion) << "unique" << true));
            ASSERT_OK(status);
        }


        // Confirm that inserting a document with the same value for "a" and "b" fails, verifying
        // the uniqueness constraint.
        BSONObj dupObj = BSON("_id" << 2 << "a" << 1 << "b" << 1);
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_NOT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(dupObj), nullOpDebug, true));
        }
        releaseDb();
        ensureValidateWorked();

        // Remove index entry of first document from index b to make it a missing index entry.
        {
            lockDb(MODE_X);

            const IndexCatalog* indexCatalog = coll()->getIndexCatalog();

            InsertDeleteOptions options;
            options.logIfError = true;
            options.dupsAllowed = true;

            {
                auto descriptor = indexCatalog->findIndexByName(&_opCtx, indexNameB);
                auto iam = indexCatalog->getEntry(descriptor)->accessMethod()->asSortedData();

                WriteUnitOfWork wunit(&_opCtx);
                int64_t numDeleted;
                const BSONObj actualKey = BSON("b" << 1);

                KeyStringSet keys;
                iam->getKeys(
                    &_opCtx,
                    coll(),
                    pooledBuilder,
                    actualKey,
                    InsertDeleteOptions::ConstraintEnforcementMode::kRelaxConstraintsUnfiltered,
                    SortedDataIndexAccessMethod::GetKeysContext::kRemovingKeys,
                    &keys,
                    nullptr,
                    nullptr,
                    rid1);
                auto removeStatus =
                    iam->removeKeys(&_opCtx, {keys.begin(), keys.end()}, options, &numDeleted);

                ASSERT_EQUALS(numDeleted, 1);
                ASSERT_OK(removeStatus);
                wunit.commit();
            }

            releaseDb();
        }

        // Insert a document with a duplicate key for "a" and "b".
        RecordId rid2;
        {
            lockDb(MODE_X);

            const IndexCatalog* indexCatalog = coll()->getIndexCatalog();

            InsertDeleteOptions options;
            options.logIfError = true;
            options.dupsAllowed = true;

            WriteUnitOfWork wunit(&_opCtx);

            // Insert a record and its keys separately. We do this to bypass duplicate constraint
            // checking. Inserting a record and all of its keys ensures that validation fails
            // because there are duplicate keys, and not just because there are keys without
            // corresponding records.
            auto swRecordId = coll()->getRecordStore()->insertRecord(
                &_opCtx, dupObj.objdata(), dupObj.objsize(), Timestamp());
            ASSERT_OK(swRecordId);
            rid2 = swRecordId.getValue();

            wunit.commit();

            // Insert the key on _id.
            {
                auto descriptor = indexCatalog->findIdIndex(&_opCtx);
                auto entry = const_cast<IndexCatalogEntry*>(indexCatalog->getEntry(descriptor));
                auto iam = entry->accessMethod()->asSortedData();
                auto interceptor = std::make_unique<IndexBuildInterceptor>(&_opCtx, entry);

                KeyStringSet keys;
                iam->getKeys(&_opCtx,
                             coll(),
                             pooledBuilder,
                             dupObj,
                             InsertDeleteOptions::ConstraintEnforcementMode::kRelaxConstraints,
                             SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                             &keys,
                             nullptr,
                             nullptr,
                             swRecordId.getValue());
                ASSERT_EQ(1, keys.size());

                {
                    WriteUnitOfWork wunit(&_opCtx);

                    int64_t numInserted;
                    auto insertStatus = iam->insertKeysAndUpdateMultikeyPaths(
                        &_opCtx,
                        coll(),
                        {keys.begin(), keys.end()},
                        {},
                        MultikeyPaths{},
                        options,
                        [this, &interceptor](const KeyString::Value& duplicateKey) {
                            return interceptor->recordDuplicateKey(&_opCtx, duplicateKey);
                        },
                        &numInserted);

                    ASSERT_EQUALS(numInserted, 1);
                    ASSERT_OK(insertStatus);

                    wunit.commit();
                }

                ASSERT_OK(interceptor->checkDuplicateKeyConstraints(&_opCtx));
            }

            // Insert the key on b.
            {
                auto descriptor = indexCatalog->findIndexByName(&_opCtx, indexNameB);
                auto entry = const_cast<IndexCatalogEntry*>(indexCatalog->getEntry(descriptor));
                auto iam = entry->accessMethod()->asSortedData();
                auto interceptor = std::make_unique<IndexBuildInterceptor>(&_opCtx, entry);

                KeyStringSet keys;
                iam->getKeys(&_opCtx,
                             coll(),
                             pooledBuilder,
                             dupObj,
                             InsertDeleteOptions::ConstraintEnforcementMode::kRelaxConstraints,
                             SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                             &keys,
                             nullptr,
                             nullptr,
                             swRecordId.getValue());
                ASSERT_EQ(1, keys.size());

                {
                    WriteUnitOfWork wunit(&_opCtx);

                    int64_t numInserted;
                    auto insertStatus = iam->insertKeysAndUpdateMultikeyPaths(
                        &_opCtx,
                        coll(),
                        {keys.begin(), keys.end()},
                        {},
                        MultikeyPaths{},
                        options,
                        [this, &interceptor](const KeyString::Value& duplicateKey) {
                            return interceptor->recordDuplicateKey(&_opCtx, duplicateKey);
                        },
                        &numInserted);

                    ASSERT_EQUALS(numInserted, 1);
                    ASSERT_OK(insertStatus);

                    wunit.commit();
                }

                ASSERT_OK(interceptor->checkDuplicateKeyConstraints(&_opCtx));
            }

            releaseDb();
        }

        // Confirm validation detects missing index entries.
        {
            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(
                CollectionValidation::validate(&_opCtx,
                                               _nss,
                                               CollectionValidation::ValidateMode::kForegroundFull,
                                               CollectionValidation::RepairMode::kNone,
                                               &results,
                                               &output,
                                               kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(false, results.valid);
            ASSERT_EQ(false, results.repaired);
            ASSERT_EQ(static_cast<size_t>(2), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(1), omitTransientWarningsFromCount(results));
            ASSERT_EQ(static_cast<size_t>(0), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(2), results.missingIndexEntries.size());

            dumpOnErrorGuard.dismiss();
        }

        // Run validate with repair, expect duplicate missing index entry document is removed from
        // record store and missing index entry is inserted into index. Results will not be valid
        // because IndexInfo.numKeys is not subtracted from when deleteDocument is called.
        // TODO SERVER-62257: Update test to expect valid results when numKeys can be correctly
        // updated.
        {
            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(
                CollectionValidation::validate(&_opCtx,
                                               _nss,
                                               CollectionValidation::ValidateMode::kForegroundFull,
                                               CollectionValidation::RepairMode::kFixErrors,
                                               &results,
                                               &output,
                                               kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(false, results.valid);
            ASSERT_EQ(true, results.repaired);
            ASSERT_EQ(static_cast<size_t>(1), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(2), omitTransientWarningsFromCount(results));
            ASSERT_EQ(static_cast<size_t>(0), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), results.missingIndexEntries.size());
            ASSERT_EQ(0, results.numRemovedExtraIndexEntries);
            ASSERT_EQ(1, results.numInsertedMissingIndexEntries);
            ASSERT_EQ(1, results.numDocumentsMovedToLostAndFound);
            ASSERT_EQ(0, results.numOutdatedMissingIndexEntry);

            ASSERT_EQ(1, results.indexResultsMap[indexNameA].keysRemovedFromRecordStore);
            ASSERT_EQ(0, results.indexResultsMap[indexNameB].keysRemovedFromRecordStore);

            dumpOnErrorGuard.dismiss();
        }

        // Confirm extra index entries are removed such that results are valid.
        {
            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(
                CollectionValidation::validate(&_opCtx,
                                               _nss,
                                               CollectionValidation::ValidateMode::kForegroundFull,
                                               CollectionValidation::RepairMode::kNone,
                                               &results,
                                               &output,
                                               kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.valid);
            ASSERT_EQ(false, results.repaired);
            ASSERT_EQ(static_cast<size_t>(0), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(0), omitTransientWarningsFromCount(results));
            ASSERT_EQ(static_cast<size_t>(0), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), results.missingIndexEntries.size());
            ASSERT_EQ(0, results.numRemovedExtraIndexEntries);
            ASSERT_EQ(0, results.numInsertedMissingIndexEntries);
            ASSERT_EQ(0, results.numDocumentsMovedToLostAndFound);
            ASSERT_EQ(0, results.numOutdatedMissingIndexEntry);

            ASSERT_EQ(0, results.indexResultsMap[indexNameA].keysRemovedFromRecordStore);
            ASSERT_EQ(0, results.indexResultsMap[indexNameB].keysRemovedFromRecordStore);

            dumpOnErrorGuard.dismiss();
        }
    }
};

class ValidateIndexWithMissingMultikeyDocRepair : public ValidateBase {
public:
    // No need to test with background validation as repair mode is not supported in background
    // validation.
    ValidateIndexWithMissingMultikeyDocRepair()
        : ValidateBase(/*full=*/false, /*background=*/false) {}

    void run() {
        SharedBufferFragmentBuilder pooledBuilder(
            KeyString::HeapBuilder::kHeapAllocatorDefaultBytes);

        // Create a new collection and insert non-multikey document.
        lockDb(MODE_X);

        RecordId id1;
        BSONObj doc = BSON("_id" << 1 << "a" << 1);
        {
            OpDebug* const nullOpDebug = nullptr;
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);

            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(doc), nullOpDebug, true));
            id1 = coll()->getCursor(&_opCtx)->next()->id;
            wunit.commit();
        }

        // Create non-multikey index.
        const auto indexName = "non_mk_index";
        auto status =
            dbtests::createIndexFromSpec(&_opCtx,
                                         coll()->ns().ns(),
                                         BSON("name" << indexName << "key" << BSON("a" << 1) << "v"
                                                     << static_cast<int>(kIndexVersion)));
        ASSERT_OK(status);

        releaseDb();
        ensureValidateWorked();

        // Set up a non-multikey index with multikey document.
        {
            lockDb(MODE_X);
            const IndexCatalog* indexCatalog = coll()->getIndexCatalog();
            auto descriptor = indexCatalog->findIndexByName(&_opCtx, indexName);
            auto iam = indexCatalog->getEntry(descriptor)->accessMethod()->asSortedData();
            InsertDeleteOptions options;
            options.dupsAllowed = true;
            options.logIfError = true;

            // Remove non-multikey index entry.
            {
                WriteUnitOfWork wunit(&_opCtx);
                KeyStringSet keys;
                iam->getKeys(
                    &_opCtx,
                    coll(),
                    pooledBuilder,
                    doc,
                    InsertDeleteOptions::ConstraintEnforcementMode::kRelaxConstraintsUnfiltered,
                    SortedDataIndexAccessMethod::GetKeysContext::kRemovingKeys,
                    &keys,
                    nullptr,
                    nullptr,
                    id1);
                ASSERT_EQ(keys.size(), 1);

                int64_t numDeleted;
                auto removeStatus =
                    iam->removeKeys(&_opCtx, {keys.begin(), keys.end()}, options, &numDeleted);
                ASSERT_OK(removeStatus);
                ASSERT_EQUALS(numDeleted, 1);
                wunit.commit();
            }

            // Update non-multikey document with multikey document.   {a: 1}   ->   {a: [2, 3]}
            BSONObj mkDoc = BSON("_id" << 1 << "a" << BSON_ARRAY(2 << 3));
            {
                WriteUnitOfWork wunit(&_opCtx);
                auto updateStatus = coll()->getRecordStore()->updateRecord(
                    &_opCtx, id1, mkDoc.objdata(), mkDoc.objsize());
                ASSERT_OK(updateStatus);
                wunit.commit();
            }

            // Not inserting keys into index should create missing multikey entries.
            releaseDb();
        }

        // Confirm missing multikey document found on non-multikey index error detected by validate.
        {
            ValidateResults results;
            BSONObjBuilder output;
            ASSERT_OK(
                CollectionValidation::validate(&_opCtx,
                                               _nss,
                                               CollectionValidation::ValidateMode::kForeground,
                                               CollectionValidation::RepairMode::kNone,
                                               &results,
                                               &output,
                                               kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(false, results.valid);
            ASSERT_EQ(false, results.repaired);
            ASSERT_EQ(static_cast<size_t>(1), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(1), omitTransientWarningsFromCount(results));
            ASSERT_EQ(static_cast<size_t>(0), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(2), results.missingIndexEntries.size());
            ASSERT_EQ(0, results.numRemovedExtraIndexEntries);
            ASSERT_EQ(0, results.numInsertedMissingIndexEntries);

            dumpOnErrorGuard.dismiss();
        }

        // Run validate in repair mode.
        {
            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(
                CollectionValidation::validate(&_opCtx,
                                               _nss,
                                               CollectionValidation::ValidateMode::kForeground,
                                               CollectionValidation::RepairMode::kFixErrors,
                                               &results,
                                               &output,
                                               kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.valid);
            ASSERT_EQ(true, results.repaired);
            ASSERT_EQ(static_cast<size_t>(0), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(2), omitTransientWarningsFromCount(results));
            ASSERT_EQ(static_cast<size_t>(0), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), results.missingIndexEntries.size());
            ASSERT_EQ(0, results.numRemovedExtraIndexEntries);
            ASSERT_EQ(2, results.numInsertedMissingIndexEntries);

            dumpOnErrorGuard.dismiss();
        }

        // Confirm index updated as multikey and missing index entries are inserted such that valid
        // is true.
        {
            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(
                CollectionValidation::validate(&_opCtx,
                                               _nss,
                                               CollectionValidation::ValidateMode::kForeground,
                                               CollectionValidation::RepairMode::kFixErrors,
                                               &results,
                                               &output,
                                               kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });
            StorageDebugUtil::printValidateResults(results);
            ASSERT_EQ(true, results.valid);
            ASSERT_EQ(false, results.repaired);
            ASSERT_EQ(static_cast<size_t>(0), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(0), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), results.missingIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), omitTransientWarningsFromCount(results));
            ASSERT_EQ(0, results.numRemovedExtraIndexEntries);
            ASSERT_EQ(0, results.numInsertedMissingIndexEntries);

            dumpOnErrorGuard.dismiss();
        }
    }
};

class ValidateDuplicateDocumentIndexKeySet : public ValidateBase {
public:
    ValidateDuplicateDocumentIndexKeySet() : ValidateBase(/*full=*/false, /*background=*/false) {}

    void run() {
        SharedBufferFragmentBuilder pooledBuilder(
            KeyString::HeapBuilder::kHeapAllocatorDefaultBytes);

        // Create a new collection.
        lockDb(MODE_X);

        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);
            wunit.commit();
        }

        // Create two identical indexes only differing by key pattern and name.
        {
            const auto indexName = "a";
            const auto indexKey = BSON("a" << 1);
            auto status = dbtests::createIndexFromSpec(
                &_opCtx,
                coll()->ns().ns(),
                BSON("name" << indexName << "key" << indexKey << "v"
                            << static_cast<int>(kIndexVersion) << "background" << false));
            ASSERT_OK(status);
        }

        {
            const auto indexName = "b";
            const auto indexKey = BSON("b" << 1);
            auto status = dbtests::createIndexFromSpec(
                &_opCtx,
                coll()->ns().ns(),
                BSON("name" << indexName << "key" << indexKey << "v"
                            << static_cast<int>(kIndexVersion) << "background" << false));
            ASSERT_OK(status);
        }

        // Insert a document.
        OpDebug* const nullOpDebug = nullptr;
        RecordId rid = RecordId::minLong();
        lockDb(MODE_X);
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx,
                coll(),
                InsertStatement(BSON("_id" << 1 << "a" << 1 << "b" << 1)),
                nullOpDebug,
                true));
            rid = coll()->getCursor(&_opCtx)->next()->id;
            wunit.commit();
        }
        releaseDb();
        ensureValidateWorked();

        // Remove the index entry for index "a".
        {
            lockDb(MODE_X);

            const IndexCatalog* indexCatalog = coll()->getIndexCatalog();
            const std::string indexName = "a";
            auto descriptor = indexCatalog->findIndexByName(&_opCtx, indexName);
            auto iam = indexCatalog->getEntry(descriptor)->accessMethod()->asSortedData();

            WriteUnitOfWork wunit(&_opCtx);
            int64_t numDeleted;
            const BSONObj actualKey = BSON("a" << 1);
            InsertDeleteOptions options;
            options.logIfError = true;
            options.dupsAllowed = true;

            KeyStringSet keys;
            iam->getKeys(
                &_opCtx,
                coll(),
                pooledBuilder,
                actualKey,
                InsertDeleteOptions::ConstraintEnforcementMode::kRelaxConstraintsUnfiltered,
                SortedDataIndexAccessMethod::GetKeysContext::kRemovingKeys,
                &keys,
                nullptr,
                nullptr,
                rid);
            auto removeStatus =
                iam->removeKeys(&_opCtx, {keys.begin(), keys.end()}, options, &numDeleted);

            ASSERT_EQUALS(numDeleted, 1);
            ASSERT_OK(removeStatus);
            wunit.commit();

            releaseDb();
        }

        // Remove the index entry for index "b".
        {
            lockDb(MODE_X);

            const IndexCatalog* indexCatalog = coll()->getIndexCatalog();
            const std::string indexName = "b";
            auto descriptor = indexCatalog->findIndexByName(&_opCtx, indexName);
            auto iam = indexCatalog->getEntry(descriptor)->accessMethod()->asSortedData();

            WriteUnitOfWork wunit(&_opCtx);
            int64_t numDeleted;
            const BSONObj actualKey = BSON("b" << 1);
            InsertDeleteOptions options;
            options.logIfError = true;
            options.dupsAllowed = true;

            KeyStringSet keys;
            iam->getKeys(
                &_opCtx,
                coll(),
                pooledBuilder,
                actualKey,
                InsertDeleteOptions::ConstraintEnforcementMode::kRelaxConstraintsUnfiltered,
                SortedDataIndexAccessMethod::GetKeysContext::kRemovingKeys,
                &keys,
                nullptr,
                nullptr,
                rid);
            auto removeStatus =
                iam->removeKeys(&_opCtx, {keys.begin(), keys.end()}, options, &numDeleted);

            ASSERT_EQUALS(numDeleted, 1);
            ASSERT_OK(removeStatus);
            wunit.commit();

            releaseDb();
        }

        {
            // Now we have two missing index entries with the keys { : 1 } since the KeyStrings
            // aren't hydrated with their field names.
            ensureValidateFailed();
        }
    }
};

template <bool full, bool background>
class ValidateDuplicateKeysUniqueIndex : public ValidateBase {
public:
    ValidateDuplicateKeysUniqueIndex() : ValidateBase(full, background) {}

    void run() {
        if (_background) {
            return;
        }

        SharedBufferFragmentBuilder pooledBuilder(
            KeyString::HeapBuilder::kHeapAllocatorDefaultBytes);

        // Create a new collection.
        lockDb(MODE_X);

        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);
            wunit.commit();
        }

        // Create a unique index.
        const auto indexName = "a";
        {
            const auto indexKey = BSON("a" << 1);
            auto status = dbtests::createIndexFromSpec(
                &_opCtx,
                coll()->ns().ns(),
                BSON("name" << indexName << "key" << indexKey << "v"
                            << static_cast<int>(kIndexVersion) << "background" << false << "unique"
                            << true));
            ASSERT_OK(status);
        }

        // Insert a document.
        OpDebug* const nullOpDebug = nullptr;
        lockDb(MODE_X);
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 1 << "a" << 1)), nullOpDebug, true));
            wunit.commit();
        }

        // Confirm that inserting a document with the same value for "a" fails, verifying the
        // uniqueness constraint.
        BSONObj dupObj = BSON("_id" << 2 << "a" << 1);
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_NOT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(dupObj), nullOpDebug, true));
        }
        releaseDb();
        ensureValidateWorked();

        // Insert a document with a duplicate key for "a".
        {
            lockDb(MODE_X);

            const IndexCatalog* indexCatalog = coll()->getIndexCatalog();

            InsertDeleteOptions options;
            options.logIfError = true;
            options.dupsAllowed = true;

            WriteUnitOfWork wunit(&_opCtx);

            // Insert a record and its keys separately. We do this to bypass duplicate constraint
            // checking. Inserting a record and all of its keys ensures that validation fails
            // because there are duplicate keys, and not just because there are keys without
            // corresponding records.
            auto swRecordId = coll()->getRecordStore()->insertRecord(
                &_opCtx, dupObj.objdata(), dupObj.objsize(), Timestamp());
            ASSERT_OK(swRecordId);

            wunit.commit();

            // Insert the key on _id.
            {
                auto descriptor = indexCatalog->findIdIndex(&_opCtx);
                auto entry = const_cast<IndexCatalogEntry*>(indexCatalog->getEntry(descriptor));
                auto iam = entry->accessMethod()->asSortedData();
                auto interceptor = std::make_unique<IndexBuildInterceptor>(&_opCtx, entry);

                KeyStringSet keys;
                iam->getKeys(&_opCtx,
                             coll(),
                             pooledBuilder,
                             dupObj,
                             InsertDeleteOptions::ConstraintEnforcementMode::kRelaxConstraints,
                             SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                             &keys,
                             nullptr,
                             nullptr,
                             swRecordId.getValue());
                ASSERT_EQ(1, keys.size());

                {
                    WriteUnitOfWork wunit(&_opCtx);

                    int64_t numInserted;
                    auto insertStatus = iam->insertKeysAndUpdateMultikeyPaths(
                        &_opCtx,
                        coll(),
                        {keys.begin(), keys.end()},
                        {},
                        MultikeyPaths{},
                        options,
                        [this, &interceptor](const KeyString::Value& duplicateKey) {
                            return interceptor->recordDuplicateKey(&_opCtx, duplicateKey);
                        },
                        &numInserted);

                    ASSERT_EQUALS(numInserted, 1);
                    ASSERT_OK(insertStatus);

                    wunit.commit();
                }

                ASSERT_OK(interceptor->checkDuplicateKeyConstraints(&_opCtx));
            }

            // Insert the key on "a".
            {
                auto descriptor = indexCatalog->findIndexByName(&_opCtx, indexName);
                auto entry = const_cast<IndexCatalogEntry*>(indexCatalog->getEntry(descriptor));
                auto iam = entry->accessMethod()->asSortedData();
                auto interceptor = std::make_unique<IndexBuildInterceptor>(&_opCtx, entry);

                KeyStringSet keys;
                iam->getKeys(&_opCtx,
                             coll(),
                             pooledBuilder,
                             dupObj,
                             InsertDeleteOptions::ConstraintEnforcementMode::kRelaxConstraints,
                             SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                             &keys,
                             nullptr,
                             nullptr,
                             swRecordId.getValue());
                ASSERT_EQ(1, keys.size());

                {
                    WriteUnitOfWork wunit(&_opCtx);

                    int64_t numInserted;
                    auto insertStatus = iam->insertKeysAndUpdateMultikeyPaths(
                        &_opCtx,
                        coll(),
                        {keys.begin(), keys.end()},
                        {},
                        MultikeyPaths{},
                        options,
                        [this, &interceptor](const KeyString::Value& duplicateKey) {
                            return interceptor->recordDuplicateKey(&_opCtx, duplicateKey);
                        },
                        &numInserted);

                    ASSERT_EQUALS(numInserted, 1);
                    ASSERT_OK(insertStatus);

                    wunit.commit();
                }

                ASSERT_NOT_OK(interceptor->checkDuplicateKeyConstraints(&_opCtx));
            }

            releaseDb();
        }

        ValidateResults results = runValidate();

        ScopeGuard dumpOnErrorGuard([&] {
            StorageDebugUtil::printValidateResults(results);
            StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
        });

        ASSERT_FALSE(results.valid) << "Validation worked when it should have failed.";
        ASSERT_EQ(static_cast<size_t>(1), results.errors.size());
        ASSERT_EQ(static_cast<size_t>(0), omitTransientWarningsFromCount(results));
        ASSERT_EQ(static_cast<size_t>(0), results.extraIndexEntries.size());
        ASSERT_EQ(static_cast<size_t>(0), results.missingIndexEntries.size());

        dumpOnErrorGuard.dismiss();
    }
};

template <bool full, bool background>
class ValidateInvalidBSONResults : public ValidateBase {
public:
    ValidateInvalidBSONResults() : ValidateBase(full, background) {}

    void run() {
        if (_background) {
            return;
        }

        // Create a new collection.
        lockDb(MODE_X);

        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);
            wunit.commit();
        }

        // Encode an invalid BSON Object with an invalid type, x90 and insert record
        const char* buffer = "\x0c\x00\x00\x00\x90\x41\x00\x10\x00\x00\x00\x00";
        BSONObj obj(buffer);
        lockDb(MODE_X);
        RecordStore* rs = coll()->getRecordStore();
        RecordId rid;
        {
            WriteUnitOfWork wunit(&_opCtx);
            auto swRecordId = rs->insertRecord(&_opCtx, obj.objdata(), obj.objsize(), Timestamp());
            ASSERT_OK(swRecordId);
            rid = swRecordId.getValue();
            wunit.commit();
        }
        releaseDb();

        {
            auto mode = _background ? CollectionValidation::ValidateMode::kBackground
                                    : CollectionValidation::ValidateMode::kForeground;

            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(CollectionValidation::validate(&_opCtx,
                                                     _nss,
                                                     mode,
                                                     CollectionValidation::RepairMode::kNone,
                                                     &results,
                                                     &output,
                                                     kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(false, results.valid);
            ASSERT_EQ(static_cast<size_t>(1), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(0), omitTransientWarningsFromCount(results));
            ASSERT_EQ(static_cast<size_t>(0), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), results.missingIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(1), results.corruptRecords.size());
            ASSERT_EQ(rid, results.corruptRecords[0]);

            dumpOnErrorGuard.dismiss();
        }
    }
};
class ValidateInvalidBSONRepair : public ValidateBase {
public:
    // No need to test with background validation as repair mode is not supported in background
    // validation.
    ValidateInvalidBSONRepair() : ValidateBase(/*full=*/false, /*background=*/false) {}

    void run() {
        // Create a new collection.
        lockDb(MODE_X);

        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);
            wunit.commit();
        }

        // Encode BSON Objects with invalid type x90, size less than 5 bytes, and BSON length and
        // object size mismatch, respectively. Insert invalid BSON objects into record store.
        const char* buf1 = "\x0c\x00\x00\x00\x90\x41\x00\x10\x00\x00\x00\x00";
        const char* buf2 = "\x04\x00\x00\x00\x90\x41\x00\x10\x00\x00\x00\x00";
        const char* buf3 = "\x0f\x00\x00\x00\x00\x41\x00\x10\x00\x00\x00\x00";
        BSONObj obj1(buf1);
        BSONObj obj2(buf2);
        BSONObj obj3(buf3);
        lockDb(MODE_X);
        RecordStore* rs = coll()->getRecordStore();
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(rs->insertRecord(&_opCtx, obj1.objdata(), 12ULL, Timestamp()));
            ASSERT_OK(rs->insertRecord(&_opCtx, obj2.objdata(), 12ULL, Timestamp()));
            ASSERT_OK(rs->insertRecord(&_opCtx, obj3.objdata(), 12ULL, Timestamp()));
            wunit.commit();
        }
        releaseDb();

        // Confirm all of the different corrupt records previously inserted are detected by
        // validate.
        {
            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(
                CollectionValidation::validate(&_opCtx,
                                               _nss,
                                               CollectionValidation::ValidateMode::kForeground,
                                               CollectionValidation::RepairMode::kNone,
                                               &results,
                                               &output,
                                               kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(false, results.valid);
            ASSERT_EQ(false, results.repaired);
            ASSERT_EQ(static_cast<size_t>(1), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(0), omitTransientWarningsFromCount(results));
            ASSERT_EQ(static_cast<size_t>(0), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), results.missingIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(3), results.corruptRecords.size());
            ASSERT_EQ(0, results.numRemovedCorruptRecords);

            dumpOnErrorGuard.dismiss();
        }

        // Run validate with repair, expect corrupted records are removed.
        {
            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(
                CollectionValidation::validate(&_opCtx,
                                               _nss,
                                               CollectionValidation::ValidateMode::kForeground,
                                               CollectionValidation::RepairMode::kFixErrors,
                                               &results,
                                               &output,
                                               kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.valid);
            ASSERT_EQ(true, results.repaired);
            ASSERT_EQ(static_cast<size_t>(0), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(1), omitTransientWarningsFromCount(results));
            ASSERT_EQ(static_cast<size_t>(0), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), results.missingIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), results.corruptRecords.size());
            ASSERT_EQ(3, results.numRemovedCorruptRecords);

            // Check that the corrupted records have been removed from the record store.
            ASSERT_EQ(0, rs->numRecords(&_opCtx));

            dumpOnErrorGuard.dismiss();
        }

        // Confirm corrupt records have been removed such that repair does not need to run and
        // results are valid.
        {
            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(
                CollectionValidation::validate(&_opCtx,
                                               _nss,
                                               CollectionValidation::ValidateMode::kForeground,
                                               CollectionValidation::RepairMode::kFixErrors,
                                               &results,
                                               &output,
                                               kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.valid);
            ASSERT_EQ(false, results.repaired);
            ASSERT_EQ(static_cast<size_t>(0), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(0), omitTransientWarningsFromCount(results));
            ASSERT_EQ(static_cast<size_t>(0), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), results.missingIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), results.corruptRecords.size());
            ASSERT_EQ(0, results.numRemovedCorruptRecords);

            dumpOnErrorGuard.dismiss();
        }

        // Confirm repair mode does not silently suppress validation errors.
        {
            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(
                CollectionValidation::validate(&_opCtx,
                                               _nss,
                                               CollectionValidation::ValidateMode::kForeground,
                                               CollectionValidation::RepairMode::kNone,
                                               &results,
                                               &output,
                                               kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.valid);
            ASSERT_EQ(false, results.repaired);
            ASSERT_EQ(static_cast<size_t>(0), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(0), omitTransientWarningsFromCount(results));
            ASSERT_EQ(static_cast<size_t>(0), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), results.missingIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), results.corruptRecords.size());
            ASSERT_EQ(0, results.numRemovedCorruptRecords);

            dumpOnErrorGuard.dismiss();
        }
    }
};

class ValidateIndexWithMultikeyDocRepair : public ValidateBase {
public:
    // No need to test with background validation as repair mode is not supported in background
    // validation.
    ValidateIndexWithMultikeyDocRepair() : ValidateBase(/*full=*/false, /*background=*/false) {}

    void run() {
        SharedBufferFragmentBuilder pooledBuilder(
            KeyString::HeapBuilder::kHeapAllocatorDefaultBytes);

        // Create a new collection and insert non-multikey document.
        lockDb(MODE_X);

        RecordId id1;
        BSONObj doc = BSON("_id" << 1 << "a" << 1);
        {
            OpDebug* const nullOpDebug = nullptr;
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);

            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(doc), nullOpDebug, true));
            id1 = coll()->getCursor(&_opCtx)->next()->id;
            wunit.commit();
        }

        // Create non-multikey index.
        const auto indexName = "non_mk_index";
        auto status =
            dbtests::createIndexFromSpec(&_opCtx,
                                         coll()->ns().ns(),
                                         BSON("name" << indexName << "key" << BSON("a" << 1) << "v"
                                                     << static_cast<int>(kIndexVersion)));
        ASSERT_OK(status);

        releaseDb();
        ensureValidateWorked();

        // Set up a non-multikey index with multikey document.
        {
            lockDb(MODE_X);
            const IndexCatalog* indexCatalog = coll()->getIndexCatalog();
            auto descriptor = indexCatalog->findIndexByName(&_opCtx, indexName);
            auto iam = indexCatalog->getEntry(descriptor)->accessMethod()->asSortedData();
            InsertDeleteOptions options;
            options.dupsAllowed = true;
            options.logIfError = true;

            // Remove non-multikey index entry.
            {
                WriteUnitOfWork wunit(&_opCtx);
                KeyStringSet keys;
                iam->getKeys(
                    &_opCtx,
                    coll(),
                    pooledBuilder,
                    doc,
                    InsertDeleteOptions::ConstraintEnforcementMode::kRelaxConstraintsUnfiltered,
                    SortedDataIndexAccessMethod::GetKeysContext::kRemovingKeys,
                    &keys,
                    nullptr,
                    nullptr,
                    id1);
                ASSERT_EQ(keys.size(), 1);

                int64_t numDeleted;
                auto removeStatus =
                    iam->removeKeys(&_opCtx, {keys.begin(), keys.end()}, options, &numDeleted);
                ASSERT_OK(removeStatus);
                ASSERT_EQUALS(numDeleted, 1);
                wunit.commit();
            }

            // Update non-multikey document with multikey document.   {a: 1}   ->   {a: [2, 3]}
            BSONObj mkDoc = BSON("_id" << 1 << "a" << BSON_ARRAY(2 << 3));
            {
                WriteUnitOfWork wunit(&_opCtx);
                auto updateStatus = coll()->getRecordStore()->updateRecord(
                    &_opCtx, id1, mkDoc.objdata(), mkDoc.objsize());
                ASSERT_OK(updateStatus);
                wunit.commit();
            }

            // Insert index entries which satisfy the new multikey document.
            {
                WriteUnitOfWork wunit(&_opCtx);
                KeyStringSet keys;
                MultikeyPaths multikeyPaths;
                iam->getKeys(
                    &_opCtx,
                    coll(),
                    pooledBuilder,
                    mkDoc,
                    InsertDeleteOptions::ConstraintEnforcementMode::kRelaxConstraintsUnfiltered,
                    SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                    &keys,
                    nullptr,
                    &multikeyPaths,
                    id1);
                ASSERT_EQ(keys.size(), 2);
                ASSERT_EQ(multikeyPaths.size(), 1);

                // Insert index keys one at a time in order to avoid marking index as multikey
                // and allows us to pass in an empty set of MultikeyPaths.
                int64_t numInserted;
                auto keysIterator = keys.begin();
                auto insertStatus = iam->insertKeysAndUpdateMultikeyPaths(&_opCtx,
                                                                          coll(),
                                                                          {*keysIterator},
                                                                          {},
                                                                          MultikeyPaths{},
                                                                          options,
                                                                          nullptr,
                                                                          &numInserted);
                ASSERT_EQUALS(numInserted, 1);
                ASSERT_OK(insertStatus);

                keysIterator++;
                numInserted = 0;
                insertStatus = iam->insertKeysAndUpdateMultikeyPaths(&_opCtx,
                                                                     coll(),
                                                                     {*keysIterator},
                                                                     {},
                                                                     MultikeyPaths{},
                                                                     options,
                                                                     nullptr,
                                                                     &numInserted);
                ASSERT_EQUALS(numInserted, 1);
                ASSERT_OK(insertStatus);
                wunit.commit();
            }
            releaseDb();
        }

        // Confirm multikey document found on non-multikey index error detected by validate.
        {
            ValidateResults results;
            BSONObjBuilder output;
            ASSERT_OK(
                CollectionValidation::validate(&_opCtx,
                                               _nss,
                                               CollectionValidation::ValidateMode::kForeground,
                                               CollectionValidation::RepairMode::kNone,
                                               &results,
                                               &output,
                                               kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(false, results.valid);
            ASSERT_EQ(false, results.repaired);
            ASSERT_EQ(static_cast<size_t>(1), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(0), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), results.missingIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), omitTransientWarningsFromCount(results));

            dumpOnErrorGuard.dismiss();
        }

        // Run validate in repair mode.
        {
            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(
                CollectionValidation::validate(&_opCtx,
                                               _nss,
                                               CollectionValidation::ValidateMode::kForeground,
                                               CollectionValidation::RepairMode::kFixErrors,
                                               &results,
                                               &output,
                                               kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.valid);
            ASSERT_EQ(true, results.repaired);
            ASSERT_EQ(static_cast<size_t>(0), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(0), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), results.missingIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(1), omitTransientWarningsFromCount(results));

            dumpOnErrorGuard.dismiss();
        }

        // Confirm index updated as multikey and multikey paths added such that index does not have
        // to be rebuilt.
        {
            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(
                CollectionValidation::validate(&_opCtx,
                                               _nss,
                                               CollectionValidation::ValidateMode::kForeground,
                                               CollectionValidation::RepairMode::kFixErrors,
                                               &results,
                                               &output,
                                               kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.valid);
            ASSERT_EQ(false, results.repaired);
            ASSERT_EQ(static_cast<size_t>(0), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(0), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), results.missingIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), omitTransientWarningsFromCount(results));

            dumpOnErrorGuard.dismiss();
        }
    }
};

class ValidateMultikeyPathCoverageRepair : public ValidateBase {
public:
    // No need to test with background validation as repair mode is not supported in background
    // validation.
    ValidateMultikeyPathCoverageRepair() : ValidateBase(/*full=*/false, /*background=*/false) {}

    void run() {
        SharedBufferFragmentBuilder pooledBuilder(
            KeyString::HeapBuilder::kHeapAllocatorDefaultBytes);

        // Create a new collection and insert multikey document.
        lockDb(MODE_X);

        RecordId id1;
        BSONObj doc1 = BSON("_id" << 1 << "a" << BSON_ARRAY(1 << 2) << "b" << 1);
        {
            OpDebug* const nullOpDebug = nullptr;

            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);

            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(doc1), nullOpDebug, true));
            id1 = coll()->getCursor(&_opCtx)->next()->id;
            wunit.commit();
        }

        // Create a multikey index.
        const auto indexName = "mk_index";
        auto status = dbtests::createIndexFromSpec(&_opCtx,
                                                   coll()->ns().ns(),
                                                   BSON("name" << indexName << "key"
                                                               << BSON("a" << 1 << "b" << 1) << "v"
                                                               << static_cast<int>(kIndexVersion)));
        ASSERT_OK(status);

        releaseDb();
        ensureValidateWorked();

        // Add a multikey document such that the multikey index's multikey paths do not cover it.
        {
            lockDb(MODE_X);

            const IndexCatalog* indexCatalog = coll()->getIndexCatalog();
            auto descriptor = indexCatalog->findIndexByName(&_opCtx, indexName);
            auto iam = indexCatalog->getEntry(descriptor)->accessMethod()->asSortedData();
            InsertDeleteOptions options;
            options.dupsAllowed = true;
            options.logIfError = true;

            // Remove index keys for original document.
            MultikeyPaths oldMultikeyPaths;
            {
                WriteUnitOfWork wunit(&_opCtx);
                KeyStringSet keys;
                iam->getKeys(
                    &_opCtx,
                    coll(),
                    pooledBuilder,
                    doc1,
                    InsertDeleteOptions::ConstraintEnforcementMode::kRelaxConstraintsUnfiltered,
                    SortedDataIndexAccessMethod::GetKeysContext::kRemovingKeys,
                    &keys,
                    nullptr,
                    &oldMultikeyPaths,
                    id1);
                ASSERT_EQ(keys.size(), 2);

                int64_t numDeleted;
                auto removeStatus =
                    iam->removeKeys(&_opCtx, {keys.begin(), keys.end()}, options, &numDeleted);
                ASSERT_OK(removeStatus);
                ASSERT_EQ(numDeleted, 2);
                wunit.commit();
            }

            // Update multikey document with a different multikey documents (not covered by multikey
            // paths).   {a: [1, 2], b: 1}   ->   {a: 1, b: [4, 5]}
            BSONObj doc2 = BSON("_id" << 1 << "a" << 1 << "b" << BSON_ARRAY(4 << 5));
            {
                WriteUnitOfWork wunit(&_opCtx);
                auto updateStatus = coll()->getRecordStore()->updateRecord(
                    &_opCtx, id1, doc2.objdata(), doc2.objsize());
                ASSERT_OK(updateStatus);
                wunit.commit();
            }

            // We are using the multikeyPaths of the old document and passing them to this insert
            // call (to avoid changing the multikey state).
            {
                WriteUnitOfWork wunit(&_opCtx);
                KeyStringSet keys;
                iam->getKeys(
                    &_opCtx,
                    coll(),
                    pooledBuilder,
                    doc2,
                    InsertDeleteOptions::ConstraintEnforcementMode::kRelaxConstraintsUnfiltered,
                    SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                    &keys,
                    nullptr,
                    nullptr,
                    id1);
                ASSERT_EQ(keys.size(), 2);

                int64_t numInserted;
                auto insertStatus = iam->insertKeysAndUpdateMultikeyPaths(
                    &_opCtx, coll(), keys, {}, oldMultikeyPaths, options, nullptr, &numInserted);

                ASSERT_EQUALS(numInserted, 2);
                ASSERT_OK(insertStatus);
                wunit.commit();
            }
            releaseDb();
        }

        // Confirm multikey paths' insufficient coverage of multikey document detected by validate.
        {
            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(
                CollectionValidation::validate(&_opCtx,
                                               _nss,
                                               CollectionValidation::ValidateMode::kForeground,
                                               CollectionValidation::RepairMode::kNone,
                                               &results,
                                               &output,
                                               kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(false, results.valid);
            ASSERT_EQ(false, results.repaired);
            ASSERT_EQ(static_cast<size_t>(1), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(0), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), results.missingIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), omitTransientWarningsFromCount(results));

            dumpOnErrorGuard.dismiss();
        }

        // Run validate in repair mode.
        {
            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(
                CollectionValidation::validate(&_opCtx,
                                               _nss,
                                               CollectionValidation::ValidateMode::kForeground,
                                               CollectionValidation::RepairMode::kFixErrors,
                                               &results,
                                               &output,
                                               kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.valid);
            ASSERT_EQ(true, results.repaired);
            ASSERT_EQ(static_cast<size_t>(0), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(0), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), results.missingIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(2), omitTransientWarningsFromCount(results));

            dumpOnErrorGuard.dismiss();
        }

        // Confirm repair mode does not silently suppress validation errors.
        {
            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(
                CollectionValidation::validate(&_opCtx,
                                               _nss,
                                               CollectionValidation::ValidateMode::kForeground,
                                               CollectionValidation::RepairMode::kNone,
                                               &results,
                                               &output,
                                               kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.valid);
            ASSERT_EQ(false, results.repaired);
            ASSERT_EQ(static_cast<size_t>(0), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(0), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), results.missingIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), omitTransientWarningsFromCount(results));

            dumpOnErrorGuard.dismiss();
        }
    }
};

// Tests that multikey paths can be added to an index for the first time.
class ValidateAddNewMultikeyPaths : public ValidateBase {
public:
    // No need to test with background validation as repair mode is not supported in background
    // validation.
    ValidateAddNewMultikeyPaths() : ValidateBase(/*full=*/false, /*background=*/false) {}

    void run() {

        // Create a new collection and create an index.
        lockDb(MODE_X);

        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);
            wunit.commit();
        }
        CollectionWriter writer(&_opCtx, coll()->ns());

        const auto indexName = "mk_index";
        auto status = dbtests::createIndexFromSpec(&_opCtx,
                                                   coll()->ns().ns(),
                                                   BSON("name" << indexName << "key"
                                                               << BSON("a" << 1 << "b" << 1) << "v"
                                                               << static_cast<int>(kIndexVersion)));
        ASSERT_OK(status);

        // Remove the multikeyPaths from the index catalog entry. This simulates the catalog state
        // of a pre-3.4 index.
        {
            WriteUnitOfWork wunit(&_opCtx);
            auto collMetadata =
                DurableCatalog::get(&_opCtx)->getMetaData(&_opCtx, coll()->getCatalogId());
            int offset = collMetadata->findIndexOffset(indexName);
            ASSERT_GTE(offset, 0);

            auto& indexMetadata = collMetadata->indexes[offset];
            indexMetadata.multikeyPaths = {};
            writer.getWritableCollection(&_opCtx)->replaceMetadata(&_opCtx,
                                                                   std::move(collMetadata));
            wunit.commit();
        }

        // Reload the index from the modified catalog.
        auto descriptor = coll()->getIndexCatalog()->findIndexByName(&_opCtx, indexName);
        {
            WriteUnitOfWork wunit(&_opCtx);
            auto writableCatalog = writer.getWritableCollection(&_opCtx)->getIndexCatalog();
            descriptor = writableCatalog->refreshEntry(&_opCtx,
                                                       writer.getWritableCollection(&_opCtx),
                                                       descriptor,
                                                       CreateIndexEntryFlags::kIsReady);
            wunit.commit();
        }

        // Insert a multikey document. The multikeyPaths should not get updated in this old
        // state.
        RecordId id1;
        BSONObj doc1 = BSON("_id" << 0 << "a" << BSON_ARRAY(1 << 2) << "b" << 1);
        OpDebug* const nullOpDebug = nullptr;
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(doc1), nullOpDebug, true));
            id1 = coll()->getCursor(&_opCtx)->next()->id;
            wunit.commit();
        }

        auto catalogEntry = coll()->getIndexCatalog()->getEntry(descriptor);
        auto expectedPathsBefore = MultikeyPaths{};
        ASSERT(catalogEntry->isMultikey(&_opCtx, coll()));
        ASSERT(catalogEntry->getMultikeyPaths(&_opCtx, coll()) == expectedPathsBefore);

        releaseDb();
        ensureValidateWorked();

        // Confirm multikeyPaths are added by validate.
        {
            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(
                CollectionValidation::validate(&_opCtx,
                                               _nss,
                                               CollectionValidation::ValidateMode::kForeground,
                                               CollectionValidation::RepairMode::kAdjustMultikey,
                                               &results,
                                               &output,
                                               kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.valid);
            ASSERT_EQ(true, results.repaired);
            ASSERT_EQ(static_cast<size_t>(0), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(0), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), results.missingIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(1), omitTransientWarningsFromCount(results));

            dumpOnErrorGuard.dismiss();
        }

        auto expectedPathsAfter = MultikeyPaths{{0}, {}};
        ASSERT(catalogEntry->isMultikey(&_opCtx, coll()));
        ASSERT(catalogEntry->getMultikeyPaths(&_opCtx, coll()) == expectedPathsAfter);

        // Confirm validate does not make changes when run a second time.
        {
            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(
                CollectionValidation::validate(&_opCtx,
                                               _nss,
                                               CollectionValidation::ValidateMode::kForeground,
                                               CollectionValidation::RepairMode::kAdjustMultikey,
                                               &results,
                                               &output,
                                               kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.valid);
            ASSERT_EQ(false, results.repaired);
            ASSERT_EQ(static_cast<size_t>(0), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(0), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), results.missingIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), omitTransientWarningsFromCount(results));

            dumpOnErrorGuard.dismiss();
        }

        ASSERT(catalogEntry->isMultikey(&_opCtx, coll()));
        ASSERT(catalogEntry->getMultikeyPaths(&_opCtx, coll()) == expectedPathsAfter);
    }
};

template <bool background>
class ValidateInvalidBSONOnClusteredCollection : public ValidateBase {
public:
    ValidateInvalidBSONOnClusteredCollection()
        : ValidateBase(/*full=*/false, background, /*clustered=*/true) {}

    void run() {
        if (_background) {
            return;
        }

        lockDb(MODE_X);
        ASSERT(coll());

        // Encode an invalid BSON Object with an invalid type, x90 and insert record
        const char* buffer = "\x0c\x00\x00\x00\x90\x41\x00\x10\x00\x00\x00\x00";
        BSONObj obj(buffer);

        RecordStore* rs = coll()->getRecordStore();
        RecordId rid(OID::gen().view().view(), OID::kOIDSize);
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(rs->insertRecord(&_opCtx, rid, obj.objdata(), obj.objsize(), Timestamp()));
            wunit.commit();
        }
        releaseDb();

        {
            auto mode = _background ? CollectionValidation::ValidateMode::kBackground
                                    : CollectionValidation::ValidateMode::kForeground;

            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(CollectionValidation::validate(&_opCtx,
                                                     _nss,
                                                     mode,
                                                     CollectionValidation::RepairMode::kNone,
                                                     &results,
                                                     &output,
                                                     kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(false, results.valid);
            ASSERT_EQ(static_cast<size_t>(1), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(0), omitTransientWarningsFromCount(results));
            ASSERT_EQ(static_cast<size_t>(0), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), results.missingIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(1), results.corruptRecords.size());
            ASSERT_EQ(rid, results.corruptRecords[0]);

            dumpOnErrorGuard.dismiss();
        }
    }
};

template <bool background>
class ValidateReportInfoOnClusteredCollection : public ValidateBase {
public:
    ValidateReportInfoOnClusteredCollection()
        : ValidateBase(/*full=*/false, background, /*clustered=*/true) {}

    void run() {
        if (_background) {
            return;
        }

        lockDb(MODE_X);
        ASSERT(coll());

        // Create an index.
        const auto indexName = "a";
        const auto indexKey = BSON("a" << 1);
        auto status = dbtests::createIndexFromSpec(
            &_opCtx,
            coll()->ns().ns(),
            BSON("name" << indexName << "key" << indexKey << "v" << static_cast<int>(kIndexVersion)
                        << "background" << false));
        ASSERT_OK(status);

        // Insert documents.
        OpDebug* const nullOpDebug = nullptr;
        RecordId rid = RecordId::minLong();
        lockDb(MODE_X);

        const OID firstRecordId = OID::gen();
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx,
                coll(),
                InsertStatement(BSON("_id" << firstRecordId << "a" << 1)),
                nullOpDebug,
                true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx,
                coll(),
                InsertStatement(BSON("_id" << OID::gen() << "a" << 2)),
                nullOpDebug,
                true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx,
                coll(),
                InsertStatement(BSON("_id" << OID::gen() << "a" << 3)),
                nullOpDebug,
                true));
            rid = coll()->getCursor(&_opCtx)->next()->id;
            wunit.commit();
        }
        releaseDb();
        ensureValidateWorked();
        lockDb(MODE_X);

        RecordStore* rs = coll()->getRecordStore();

        // Updating a document without updating the index entry will cause validation to detect a
        // missing index entry and an extra index entry.
        {
            WriteUnitOfWork wunit(&_opCtx);
            auto doc = BSON("_id" << firstRecordId << "a" << 5);
            auto updateStatus = rs->updateRecord(&_opCtx, rid, doc.objdata(), doc.objsize());
            ASSERT_OK(updateStatus);
            wunit.commit();
        }
        releaseDb();

        {
            auto mode = _background ? CollectionValidation::ValidateMode::kBackground
                                    : CollectionValidation::ValidateMode::kForeground;

            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(CollectionValidation::validate(&_opCtx,
                                                     _nss,
                                                     mode,
                                                     CollectionValidation::RepairMode::kNone,
                                                     &results,
                                                     &output,
                                                     kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(false, results.valid);
            ASSERT_EQ(static_cast<size_t>(1), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(2), omitTransientWarningsFromCount(results));
            ASSERT_EQ(static_cast<size_t>(1), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(1), results.missingIndexEntries.size());

            dumpOnErrorGuard.dismiss();
        }
    }
};

class ValidateRepairOnClusteredCollection : public ValidateBase {
public:
    ValidateRepairOnClusteredCollection()
        : ValidateBase(/*full=*/false, /*background=*/false, /*clustered=*/true) {}

    void run() {
        if (_background) {
            return;
        }

        lockDb(MODE_X);
        ASSERT(coll());

        // Create an index.
        const auto indexName = "a";
        const auto indexKey = BSON("a" << 1);
        auto status = dbtests::createIndexFromSpec(
            &_opCtx,
            coll()->ns().ns(),
            BSON("name" << indexName << "key" << indexKey << "v" << static_cast<int>(kIndexVersion)
                        << "background" << false));
        ASSERT_OK(status);

        // Insert documents.
        OpDebug* const nullOpDebug = nullptr;
        RecordId rid = RecordId::minLong();
        lockDb(MODE_X);

        const OID firstRecordId = OID::gen();
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx,
                coll(),
                InsertStatement(BSON("_id" << firstRecordId << "a" << 1)),
                nullOpDebug,
                true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx,
                coll(),
                InsertStatement(BSON("_id" << OID::gen() << "a" << 2)),
                nullOpDebug,
                true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx,
                coll(),
                InsertStatement(BSON("_id" << OID::gen() << "a" << 3)),
                nullOpDebug,
                true));
            rid = coll()->getCursor(&_opCtx)->next()->id;
            wunit.commit();
        }
        releaseDb();
        ensureValidateWorked();
        lockDb(MODE_X);

        RecordStore* rs = coll()->getRecordStore();

        // Updating a document without updating the index entry will cause validation to detect a
        // missing index entry and an extra index entry.
        {
            WriteUnitOfWork wunit(&_opCtx);
            auto doc = BSON("_id" << firstRecordId << "a" << 5);
            auto updateStatus = rs->updateRecord(&_opCtx, rid, doc.objdata(), doc.objsize());
            ASSERT_OK(updateStatus);
            wunit.commit();
        }
        releaseDb();

        {
            auto mode = _background ? CollectionValidation::ValidateMode::kBackground
                                    : CollectionValidation::ValidateMode::kForeground;

            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(CollectionValidation::validate(&_opCtx,
                                                     _nss,
                                                     mode,
                                                     CollectionValidation::RepairMode::kNone,
                                                     &results,
                                                     &output,
                                                     kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(false, results.valid);
            ASSERT_EQ(static_cast<size_t>(1), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(2), omitTransientWarningsFromCount(results));
            ASSERT_EQ(static_cast<size_t>(1), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(1), results.missingIndexEntries.size());

            dumpOnErrorGuard.dismiss();
        }

        // Run validate with repair, expect that extra index entries are removed and missing index
        // entries are inserted.
        {
            auto mode = _background ? CollectionValidation::ValidateMode::kBackground
                                    : CollectionValidation::ValidateMode::kForeground;

            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(CollectionValidation::validate(&_opCtx,
                                                     _nss,
                                                     mode,
                                                     CollectionValidation::RepairMode::kFixErrors,
                                                     &results,
                                                     &output,
                                                     kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });


            ASSERT_EQ(true, results.valid);
            ASSERT_EQ(true, results.repaired);
            ASSERT_EQ(static_cast<size_t>(0), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(2), omitTransientWarningsFromCount(results));
            ASSERT_EQ(static_cast<size_t>(0), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), results.missingIndexEntries.size());
            ASSERT_EQ(1, results.numRemovedExtraIndexEntries);
            ASSERT_EQ(1, results.numInsertedMissingIndexEntries);

            dumpOnErrorGuard.dismiss();
        }
    }
};

/**
 * Validate the detection of inconsistent RecordId's in a clustered collection.
 * Covered scenarios: a document with missing _id and a Record whose
 * RecordId doesn't match the document _id.
 */
template <bool background>
class ValidateInvalidRecordIdOnClusteredCollection : public ValidateBase {
public:
    ValidateInvalidRecordIdOnClusteredCollection(bool withSecondaryIndex)
        : ValidateBase(/*full=*/true, /*background=*/background, /*clustered=*/true),
          _withSecondaryIndex(withSecondaryIndex) {}

    void run() {
        if (_background) {
            return;
        }

        lockDb(MODE_X);
        ASSERT(coll());

        if (_withSecondaryIndex) {
            // Create index on {a: 1}
            const auto indexName = "a";
            const auto indexKey = BSON("a" << 1);
            auto status = dbtests::createIndexFromSpec(
                &_opCtx,
                coll()->ns().ns(),
                BSON("name" << indexName << "key" << indexKey << "v"
                            << static_cast<int>(kIndexVersion) << "background" << false));
            ASSERT_OK(status);
        }

        // Insert documents
        OpDebug* const nullOpDebug = nullptr;
        lockDb(MODE_X);

        const OID firstRecordId = OID::gen();
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx,
                coll(),
                InsertStatement(BSON("_id" << firstRecordId << "a" << 1)),
                nullOpDebug,
                true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx,
                coll(),
                InsertStatement(BSON("_id" << OID::gen() << "a" << 2)),
                nullOpDebug,
                true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx,
                coll(),
                InsertStatement(BSON("_id" << OID::gen() << "a" << 3)),
                nullOpDebug,
                true));
            wunit.commit();
        }
        releaseDb();
        ensureValidateWorked();
        lockDb(MODE_X);

        // Corrupt the first record in the RecordStore by dropping the document's _id field.
        // Corrupt the second record in the RecordStore by having the RecordId not match the _id
        // field. Leave the third record untocuhed.

        RecordStore* rs = coll()->getRecordStore();
        auto cursor = coll()->getCursor(&_opCtx);
        const auto ridMissingId = cursor->next()->id;
        {
            WriteUnitOfWork wunit(&_opCtx);
            auto doc = BSON("a" << 1);
            auto updateStatus =
                rs->updateRecord(&_opCtx, ridMissingId, doc.objdata(), doc.objsize());
            ASSERT_OK(updateStatus);
            wunit.commit();
        }
        const auto ridMismatchedId = cursor->next()->id;
        {
            WriteUnitOfWork wunit(&_opCtx);
            auto doc = BSON("_id" << OID::gen() << "a" << 2);
            auto updateStatus =
                rs->updateRecord(&_opCtx, ridMismatchedId, doc.objdata(), doc.objsize());
            ASSERT_OK(updateStatus);
            wunit.commit();
        }
        cursor.reset();

        releaseDb();

        // Verify that validate() detects the two corrupt records.
        {
            ValidateResults results;
            BSONObjBuilder output;

            ASSERT_OK(
                CollectionValidation::validate(&_opCtx,
                                               _nss,
                                               CollectionValidation::ValidateMode::kForegroundFull,
                                               CollectionValidation::RepairMode::kNone,
                                               &results,
                                               &output,
                                               kTurnOnExtraLoggingForTest));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(false, results.valid);
            ASSERT_EQ(static_cast<size_t>(2), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(0), omitTransientWarningsFromCount(results));
            ASSERT_EQ(static_cast<size_t>(0), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), results.missingIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(2), results.corruptRecords.size());
            ASSERT_EQ(ridMissingId, results.corruptRecords[0]);
            ASSERT_EQ(ridMismatchedId, results.corruptRecords[1]);

            dumpOnErrorGuard.dismiss();
        }
    }

private:
    const bool _withSecondaryIndex;
};

class ValidateTests : public OldStyleSuiteSpecification {
public:
    ValidateTests() : OldStyleSuiteSpecification("validate_tests") {}

    void setupTests() {
        // Add tests for both full validate and non-full validate.
        add<ValidateIdIndexCount<true, false>>();
        add<ValidateIdIndexCount<false, false>>();
        add<ValidateIdIndexCount<false, true>>();
        add<ValidateSecondaryIndexCount<true, false>>();
        add<ValidateSecondaryIndexCount<false, false>>();
        add<ValidateSecondaryIndexCount<false, true>>();

        // These tests are only needed for non-full validate.
        add<ValidateIdIndex<false, false>>();
        add<ValidateIdIndex<false, true>>();
        add<ValidateSecondaryIndex<false, false>>();
        add<ValidateSecondaryIndex<false, true>>();
        add<ValidateMultiKeyIndex<false, false>>();
        add<ValidateMultiKeyIndex<false, true>>();
        add<ValidateSparseIndex<false, false>>();
        add<ValidateSparseIndex<false, true>>();
        add<ValidateCompoundIndex<false, false>>();
        add<ValidateCompoundIndex<false, true>>();
        add<ValidatePartialIndex<false, false>>();
        add<ValidatePartialIndex<false, true>>();
        add<ValidatePartialIndexOnCollectionWithNonIndexableFields<false, false>>();
        add<ValidatePartialIndexOnCollectionWithNonIndexableFields<false, true>>();
        add<ValidateWildCardIndex<false, false>>();
        add<ValidateWildCardIndex<false, true>>();
        add<ValidateWildCardIndexWithProjection<false, false>>();
        add<ValidateWildCardIndexWithProjection<false, true>>();

        // Tests for index validation.
        add<ValidateIndexEntry<false, false>>();
        add<ValidateIndexEntry<false, true>>();
        add<ValidateIndexMetadata>();

        // Tests that the 'missingIndexEntries' and 'extraIndexEntries' field are populated
        // correctly.
        add<ValidateMissingAndExtraIndexEntryResults<false, false>>();
        add<ValidateMissingIndexEntryResults<false, false>>();
        add<ValidateExtraIndexEntryResults<false, false>>();

        add<ValidateMissingAndExtraIndexEntryRepair>();
        add<ValidateMissingIndexEntryRepair>();
        add<ValidateExtraIndexEntryRepair>();
        add<ValidateDuplicateDocumentMissingIndexEntryRepair>();
        add<ValidateDoubleDuplicateDocumentMissingIndexEntryRepair>();
        add<ValidateDoubleDuplicateDocumentOppositeMissingIndexEntryRepair>();
        add<ValidateIndexWithMissingMultikeyDocRepair>();

        add<ValidateDuplicateDocumentIndexKeySet>();

        add<ValidateDuplicateKeysUniqueIndex<false, false>>();
        add<ValidateDuplicateKeysUniqueIndex<false, true>>();

        add<ValidateInvalidBSONResults<false, false>>();
        add<ValidateInvalidBSONResults<false, true>>();
        add<ValidateInvalidBSONRepair>();

        add<ValidateIndexWithMultikeyDocRepair>();
        add<ValidateMultikeyPathCoverageRepair>();

        add<ValidateAddNewMultikeyPaths>();

        // Tests that validation works on clustered collections.
        add<ValidateInvalidBSONOnClusteredCollection<false>>();
        add<ValidateInvalidBSONOnClusteredCollection<true>>();
        add<ValidateReportInfoOnClusteredCollection<false>>();
        add<ValidateReportInfoOnClusteredCollection<true>>();
        add<ValidateRepairOnClusteredCollection>();

        add<ValidateInvalidRecordIdOnClusteredCollection<false>>(false /*withSecondaryIndex*/);
        add<ValidateInvalidRecordIdOnClusteredCollection<false>>(true /*withSecondaryIndex*/);
        add<ValidateInvalidRecordIdOnClusteredCollection<true>>(false /*withSecondaryIndex*/);
        add<ValidateInvalidRecordIdOnClusteredCollection<true>>(true /*withSecondaryIndex*/);
    }
};

OldStyleSuiteInitializer<ValidateTests> validateTests;

}  // namespace ValidateTests
}  // namespace mongo
