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

#include <boost/container/small_vector.hpp>
#include <boost/container/vector.hpp>
#include <fmt/format.h>
// IWYU pragma: no_include "boost/intrusive/detail/iterator.hpp"
// IWYU pragma: no_include "boost/move/algo/detail/set_difference.hpp"
#include <algorithm>
#include <boost/move/algo/move.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/data_view.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/clustered_collection_options_gen.h"
#include "mongo/db/catalog/clustered_collection_util.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/collection_validation.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/catalog/multi_index_block.h"
#include "mongo/db/catalog/validate_results.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_build_interceptor.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/bson_collection_catalog_entry.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/durable_catalog_entry.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/storage/sorted_data_interface_test_assert.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/dbtests/storage_debug_util.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util_core.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/shared_buffer_fragment.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace ValidateTests {
namespace {

using CollectionValidation::ValidationOptions;

const auto kIndexVersion = IndexDescriptor::IndexVersion::kV2;
const bool kLogDiagnostics = true;

std::size_t totalNonTransientWarnings(const ValidateResults& results) {
    auto countNonTransientWarnings = [](const auto& warnings) {
        return std::count_if(warnings.begin(), warnings.end(), [](const std::string& elem) {
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
    };
    return countNonTransientWarnings(results.getWarnings()) +
        std::accumulate(results.getIndexResultsMap().begin(),
                        results.getIndexResultsMap().end(),
                        0,
                        [&countNonTransientWarnings](size_t current, const auto& ivr) {
                            return current + countNonTransientWarnings(ivr.second.getWarnings());
                        });
}

std::size_t totalErrors(const ValidateResults& results) {
    return results.getErrors().size() +
        std::accumulate(results.getIndexResultsMap().begin(),
                        results.getIndexResultsMap().end(),
                        0,
                        [](size_t current, const auto& ivr) {
                            return current + ivr.second.getErrors().size();
                        });
}

Timestamp timestampToUse = Timestamp(1, 1);
void advanceTimestamp() {
    timestampToUse = Timestamp(timestampToUse.getSecs() + 1, timestampToUse.getInc() + 1);
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
        : _full(full),
          _background(background),
          _nss(NamespaceString::createNamespaceString_forTest(_ns)),
          _autoDb(nullptr),
          _db(nullptr) {

        // Disable table logging. When table logging is enabled, timestamps are discarded by the
        // storage engine.
        storageGlobalParams.forceDisableTableLogging = true;

        CollectionOptions options;
        if (clustered) {
            options.clusteredIndex = clustered_util::makeCanonicalClusteredInfoForLegacyFormat();
        }

        _opCtx.getServiceContext()->getStorageEngine()->setInitialDataTimestamp(timestampToUse);

        const bool createIdIndex = !clustered;

        AutoGetCollection autoColl(&_opCtx, _nss, MODE_IX);
        auto db = autoColl.ensureDbExists(&_opCtx);
        ASSERT_TRUE(db) << _nss.toStringForErrorMsg();

        beginTransaction();
        auto coll = db->createCollection(&_opCtx, _nss, options, createIdIndex);
        ASSERT_TRUE(coll) << _nss.toStringForErrorMsg();
        commitTransaction();
    }

    explicit ValidateBase(bool full, bool background)
        : ValidateBase(full, background, /*clustered=*/false) {}

    ~ValidateBase() {
        AutoGetDb autoDb(&_opCtx, _nss.dbName(), MODE_X);
        auto db = autoDb.getDb();
        ASSERT_TRUE(db);

        beginTransaction();
        ASSERT_OK(db->dropCollection(&_opCtx, _nss));
        commitTransaction();

        getGlobalServiceContext()->unsetKillAllOperations();
    }

    // Helper to refetch the Collection from the catalog in order to see any changes made to it
    CollectionPtr coll() const {
        return CollectionPtr(
            CollectionCatalog::get(&_opCtx)->lookupCollectionByNamespace(&_opCtx, _nss));
    }

protected:
    void forceCheckpoint(bool background) {
        if (background) {
            // Checkpoint all of the data.
            _opCtx.getServiceContext()->getStorageEngine()->checkpoint();
        }
    }

    ValidateResults runValidate() {
        // validate() will set a kProvided read source. Callers continue to do operations after
        // running validate, so we must reset the read source back to normal before returning.
        auto originalReadSource =
            shard_role_details::getRecoveryUnit(&_opCtx)->getTimestampReadSource();
        ON_BLOCK_EXIT([&] {
            shard_role_details::getRecoveryUnit(&_opCtx)->abandonSnapshot();
            shard_role_details::getRecoveryUnit(&_opCtx)->setTimestampReadSource(
                originalReadSource);
        });

        auto mode = [&] {
            if (_background)
                return CollectionValidation::ValidateMode::kBackground;
            return _full ? CollectionValidation::ValidateMode::kForegroundFull
                         : CollectionValidation::ValidateMode::kForeground;
        }();
        auto repairMode = CollectionValidation::RepairMode::kNone;
        ValidateResults results;

        forceCheckpoint(_background);
        ASSERT_OK(CollectionValidation::validate(
            &_opCtx, _nss, ValidationOptions{mode, repairMode, kLogDiagnostics}, &results));

        //  Check if errors are reported if and only if valid is set to false.
        ASSERT_EQ(results.isValid(), totalErrors(results) == 0);

        if (_full) {
            bool allIndexesValid = std::all_of(
                results.getIndexResultsMap().begin(),
                results.getIndexResultsMap().end(),
                [](const auto& name_results_pair) { return name_results_pair.second.isValid(); });
            ASSERT_EQ(results.isValid(), allIndexesValid);
        }

        return results;
    }

    void ensureValidateWorked() {
        ValidateResults results = runValidate();

        ScopeGuard dumpOnErrorGuard([&] {
            StorageDebugUtil::printValidateResults(results);
            StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, _nss);
        });

        ASSERT_TRUE(results.isValid()) << "Validation failed when it should've worked.";

        dumpOnErrorGuard.dismiss();
    }

    void ensureValidateWarned() {
        ValidateResults results = runValidate();

        ScopeGuard dumpOnErrorGuard([&] {
            StorageDebugUtil::printValidateResults(results);
            StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, _nss);
        });

        ASSERT_TRUE(results.isValid()) << "Validation failed when it should've worked.";
        ASSERT_EQ(0, totalErrors(results)) << "Validation reported errors when it should not have.";
        ASSERT_NE(0, totalNonTransientWarnings(results))
            << "Validation did not report a warning when it should have.";

        dumpOnErrorGuard.dismiss();
    }

    void ensureValidateFailed() {
        ValidateResults results = runValidate();

        ScopeGuard dumpOnErrorGuard([&] {
            StorageDebugUtil::printValidateResults(results);
            StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, _nss);
        });

        ASSERT_FALSE(results.isValid()) << "Validation worked when it should've failed.";

        dumpOnErrorGuard.dismiss();
    }

    void lockDb(LockMode mode) {
        _autoDb.reset();
        invariant(
            shard_role_details::getLocker(&_opCtx)->isDbLockedForMode(_nss.dbName(), MODE_NONE));
        _autoDb.reset(new AutoGetDb(&_opCtx, _nss.dbName(), mode));
        invariant(shard_role_details::getLocker(&_opCtx)->isDbLockedForMode(_nss.dbName(), mode));
        _db = _autoDb.get()->getDb();
    }

    void beginTransaction() {
        advanceTimestamp();
        _wuow = std::make_unique<WriteUnitOfWork>(&_opCtx);
        ASSERT_OK(shard_role_details::getRecoveryUnit(&_opCtx)->setTimestamp(timestampToUse));
    }

    void commitTransaction() {
        _wuow->commit();
        _opCtx.getServiceContext()->getStorageEngine()->setStableTimestamp(timestampToUse);
    }

    void abortTransaction() {
        _wuow.reset();
    }

    Status createIndexFromSpec(const BSONObj& spec) {
        AutoGetDb autoDb(&_opCtx, _nss.dbName(), MODE_IX);
        MultiIndexBlock indexer;
        ScopeGuard abortOnExit([&] {
            Lock::CollectionLock collLock(&_opCtx, _nss, MODE_X);
            CollectionWriter collection(&_opCtx, _nss);
            beginTransaction();
            indexer.abortIndexBuild(&_opCtx, collection, MultiIndexBlock::kNoopOnCleanUpFn);
            commitTransaction();
        });
        auto status = Status::OK();
        {
            Lock::CollectionLock collLock(&_opCtx, _nss, MODE_X);
            CollectionWriter collection(&_opCtx, _nss);
            beginTransaction();
            status =
                indexer
                    .init(&_opCtx,
                          collection,
                          spec,
                          [](const std::vector<BSONObj>& specs) -> Status { return Status::OK(); })
                    .getStatus();
            commitTransaction();
            if (status == ErrorCodes::IndexAlreadyExists) {
                return Status::OK();
            }
            if (!status.isOK()) {
                return status;
            }
        }
        {
            Lock::CollectionLock collLock(&_opCtx, _nss, MODE_IX);
            CollectionWriter collection(&_opCtx, _nss);
            status = indexer.insertAllDocumentsInCollection(&_opCtx, collection.get());
            if (!status.isOK()) {
                return status;
            }
        }
        {
            Lock::CollectionLock collLock(&_opCtx, _nss, MODE_X);
            CollectionWriter collection(&_opCtx, _nss);
            status = indexer.retrySkippedRecords(&_opCtx, collection.get());
            if (!status.isOK()) {
                return status;
            }

            status = indexer.checkConstraints(&_opCtx, collection.get());
            if (!status.isOK()) {
                return status;
            }
            beginTransaction();
            ASSERT_OK(indexer.commit(&_opCtx,
                                     collection.getWritableCollection(&_opCtx),
                                     MultiIndexBlock::kNoopOnCreateEachFn,
                                     MultiIndexBlock::kNoopOnCommitFn));
            commitTransaction();
        }
        abortOnExit.dismiss();
        return Status::OK();
    }

    void releaseDb() {
        _autoDb.reset();
        _db = nullptr;
        invariant(
            shard_role_details::getLocker(&_opCtx)->isDbLockedForMode(_nss.dbName(), MODE_NONE));
    }

    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_txnPtr;
    bool _full;
    bool _background;
    const NamespaceString _nss;
    std::unique_ptr<AutoGetDb> _autoDb;
    Database* _db;
    std::unique_ptr<WriteUnitOfWork> _wuow;
};

template <bool full, bool background>
class ValidateIdIndexCount : public ValidateBase {
public:
    ValidateIdIndexCount() : ValidateBase(full, background) {}

    void run() {
        // Create a new collection, insert records {_id: 1} and {_id: 2} and check it's valid.
        lockDb(MODE_X);

        RecordId id1;
        {
            OpDebug* const nullOpDebug = nullptr;
            beginTransaction();
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);

            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 1)), nullOpDebug, true));
            id1 = coll()->getCursor(&_opCtx)->next()->id;
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 2)), nullOpDebug, true));
            commitTransaction();
        }
        releaseDb();
        ensureValidateWorked();

        lockDb(MODE_X);
        RecordStore* rs = coll()->getRecordStore();

        // Remove {_id: 1} from the record store, so we get more _id entries than records.
        {
            beginTransaction();
            rs->deleteRecord(&_opCtx, id1);
            commitTransaction();
        }
        releaseDb();
        ensureValidateFailed();

        lockDb(MODE_X);

        // Insert records {_id: 0} and {_id: 1} , so we get too few _id entries, and verify
        // validate fails.
        {
            beginTransaction();
            for (int j = 0; j < 2; j++) {
                auto doc = BSON("_id" << j);
                ASSERT_OK(rs->insertRecord(&_opCtx, doc.objdata(), doc.objsize(), timestampToUse));
            }
            commitTransaction();
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
        // Create a new collection, insert two documents.
        lockDb(MODE_X);
        RecordId id1;
        {
            OpDebug* const nullOpDebug = nullptr;
            beginTransaction();
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 1 << "a" << 1)), nullOpDebug, true));
            id1 = coll()->getCursor(&_opCtx)->next()->id;
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 2 << "a" << 2)), nullOpDebug, true));
            commitTransaction();
        }

        auto status = createIndexFromSpec(BSON("name"
                                               << "a"
                                               << "key" << BSON("a" << 1) << "v"
                                               << static_cast<int>(kIndexVersion)));

        ASSERT_OK(status);
        releaseDb();
        ensureValidateWorked();

        lockDb(MODE_X);
        RecordStore* rs = coll()->getRecordStore();

        // Remove a record, so we get more _id entries than records, and verify validate fails.
        {
            beginTransaction();
            rs->deleteRecord(&_opCtx, id1);
            commitTransaction();
        }
        releaseDb();
        ensureValidateFailed();

        lockDb(MODE_X);

        // Insert two more records, so we get too few entries for a non-sparse index, and
        // verify validate fails.
        {
            beginTransaction();
            for (int j = 0; j < 2; j++) {
                auto doc = BSON("_id" << j);
                ASSERT_OK(rs->insertRecord(&_opCtx, doc.objdata(), doc.objsize(), timestampToUse));
            }
            commitTransaction();
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
        // Create a new collection, insert three records.
        lockDb(MODE_X);
        OpDebug* const nullOpDebug = nullptr;
        RecordId id1;
        {
            beginTransaction();
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 1 << "a" << 1)), nullOpDebug, true));
            id1 = coll()->getCursor(&_opCtx)->next()->id;
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 2 << "a" << 2)), nullOpDebug, true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 3 << "b" << 3)), nullOpDebug, true));
            commitTransaction();
        }

        auto status = createIndexFromSpec(BSON("name"
                                               << "a"
                                               << "key" << BSON("a" << 1) << "v"
                                               << static_cast<int>(kIndexVersion)));

        ASSERT_OK(status);
        releaseDb();
        ensureValidateWorked();

        lockDb(MODE_X);
        RecordStore* rs = coll()->getRecordStore();

        // Update {a: 1} to {a: 9} without updating the index, so we get inconsistent values
        // between the index and the document. Verify validate fails.
        {
            beginTransaction();
            auto doc = BSON("_id" << 1 << "a" << 9);
            auto updateStatus = rs->updateRecord(&_opCtx, id1, doc.objdata(), doc.objsize());

            ASSERT_OK(updateStatus);
            commitTransaction();
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
        // Create a new collection, insert records {_id: 1} and {_id: 2} and check it's valid.
        lockDb(MODE_X);
        OpDebug* const nullOpDebug = nullptr;
        RecordId id1;
        {
            beginTransaction();
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);

            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 1)), nullOpDebug, true));
            id1 = coll()->getCursor(&_opCtx)->next()->id;
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 2)), nullOpDebug, true));
            commitTransaction();
        }
        releaseDb();
        ensureValidateWorked();

        lockDb(MODE_X);
        RecordStore* rs = coll()->getRecordStore();

        // Update {_id: 1} to {_id: 9} without updating the index, so we get inconsistent values
        // between the index and the document. Verify validate fails.
        {
            beginTransaction();
            auto doc = BSON("_id" << 9);
            auto updateStatus = rs->updateRecord(&_opCtx, id1, doc.objdata(), doc.objsize());
            ASSERT_OK(updateStatus);
            commitTransaction();
        }
        releaseDb();
        ensureValidateFailed();

        lockDb(MODE_X);

        // Revert {_id: 9} to {_id: 1} and verify that validate succeeds.
        {
            beginTransaction();
            auto doc = BSON("_id" << 1);
            auto updateStatus = rs->updateRecord(&_opCtx, id1, doc.objdata(), doc.objsize());
            ASSERT_OK(updateStatus);
            commitTransaction();
        }
        releaseDb();
        ensureValidateWorked();

        lockDb(MODE_X);

        // Remove the {_id: 1} document and insert a new document without an index entry, so there
        // will still be the same number of index entries and documents, but one document will not
        // have an index entry.
        {
            beginTransaction();
            rs->deleteRecord(&_opCtx, id1);
            auto doc = BSON("_id" << 3);
            ASSERT_OK(rs->insertRecord(&_opCtx, doc.objdata(), doc.objsize(), timestampToUse)
                          .getStatus());
            commitTransaction();
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
            beginTransaction();
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);


            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(doc1), nullOpDebug, true));
            id1 = coll()->getCursor(&_opCtx)->next()->id;
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(doc2), nullOpDebug, true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(doc3), nullOpDebug, true));
            commitTransaction();
        }
        releaseDb();
        ensureValidateWorked();

        lockDb(MODE_X);

        // Create multi-key index.
        auto status = createIndexFromSpec(BSON("name"
                                               << "multikey_index"
                                               << "key" << BSON("a.b" << 1) << "v"
                                               << static_cast<int>(kIndexVersion)));

        ASSERT_OK(status);
        releaseDb();
        ensureValidateWorked();

        lockDb(MODE_X);
        RecordStore* rs = coll()->getRecordStore();

        // Update a document's indexed field without updating the index.
        {
            beginTransaction();
            auto updateStatus = rs->updateRecord(&_opCtx, id1, doc1_b.objdata(), doc1_b.objsize());
            ASSERT_OK(updateStatus);
            commitTransaction();
        }
        releaseDb();
        ensureValidateFailed();

        lockDb(MODE_X);

        // Update a document's non-indexed field without updating the index.
        // Index validation should still be valid.
        {
            beginTransaction();
            auto updateStatus = rs->updateRecord(&_opCtx, id1, doc1_c.objdata(), doc1_c.objsize());
            ASSERT_OK(updateStatus);
            commitTransaction();
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
        // Create a new collection, insert three records and check it's valid.
        lockDb(MODE_X);
        OpDebug* const nullOpDebug = nullptr;
        RecordId id1;
        {
            beginTransaction();
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);

            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 1 << "a" << 1)), nullOpDebug, true));
            id1 = coll()->getCursor(&_opCtx)->next()->id;
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 2 << "a" << 2)), nullOpDebug, true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 3 << "b" << 1)), nullOpDebug, true));
            commitTransaction();
        }

        // Create a sparse index.
        auto status = createIndexFromSpec(BSON("name"
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
            beginTransaction();
            auto doc = BSON("_id" << 2 << "a" << 3);
            auto updateStatus = rs->updateRecord(&_opCtx, id1, doc.objdata(), doc.objsize());
            ASSERT_OK(updateStatus);
            commitTransaction();
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
        // Create a new collection, insert three records and check it's valid.
        lockDb(MODE_X);
        OpDebug* const nullOpDebug = nullptr;
        RecordId id1;
        {
            beginTransaction();
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
            commitTransaction();
        }

        // Create a partial index.
        auto status = createIndexFromSpec(BSON("name"
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
            beginTransaction();
            auto doc = BSON("_id" << 1);
            auto updateStatus = rs->updateRecord(&_opCtx, id1, doc.objdata(), doc.objsize());
            ASSERT_OK(updateStatus);
            commitTransaction();
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
        // Create a new collection and insert a record that has a non-indexable value on the indexed
        // field.
        lockDb(MODE_X);
        OpDebug* const nullOpDebug = nullptr;

        RecordId id1;
        {
            beginTransaction();
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx,
                coll(),
                InsertStatement(BSON("_id" << 1 << "x" << 1 << "a" << 2)),
                nullOpDebug,
                true));
            commitTransaction();
        }

        // Create a partial geo index that indexes the document. This should return an error.
        ASSERT_NOT_OK(createIndexFromSpec(BSON("name"
                                               << "partial_index"
                                               << "key"
                                               << BSON("x"
                                                       << "2dsphere")
                                               << "v" << static_cast<int>(kIndexVersion)
                                               << "partialFilterExpression"
                                               << BSON("a" << BSON("$eq" << 2)))));

        // Create a partial geo index that does not index the document.
        auto status = createIndexFromSpec(BSON("name"
                                               << "partial_index"
                                               << "key"
                                               << BSON("x"
                                                       << "2dsphere")
                                               << "v" << static_cast<int>(kIndexVersion)
                                               << "partialFilterExpression"
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
        // Create a new collection, insert five records and check it's valid.
        lockDb(MODE_X);
        OpDebug* const nullOpDebug = nullptr;

        RecordId id1;
        {
            beginTransaction();
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
            commitTransaction();
        }

        // Create two compound indexes, one forward and one reverse, to test
        // validate()'s index direction parsing.
        auto status = createIndexFromSpec(BSON("name"
                                               << "compound_index_1"
                                               << "key" << BSON("a" << 1 << "b" << -1) << "v"
                                               << static_cast<int>(kIndexVersion)));
        ASSERT_OK(status);

        status = createIndexFromSpec(BSON("name"
                                          << "compound_index_2"
                                          << "key" << BSON("a" << -1 << "b" << 1) << "v"
                                          << static_cast<int>(kIndexVersion)));

        ASSERT_OK(status);
        releaseDb();
        ensureValidateWorked();

        lockDb(MODE_X);
        RecordStore* rs = coll()->getRecordStore();

        // Update a document's indexed field without updating the index.
        {
            beginTransaction();
            auto doc = BSON("_id" << 1 << "a" << 1 << "b" << 3);
            auto updateStatus = rs->updateRecord(&_opCtx, id1, doc.objdata(), doc.objsize());
            ASSERT_OK(updateStatus);
            commitTransaction();
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
        SharedBufferFragmentBuilder pooledBuilder(
            key_string::HeapBuilder::kHeapAllocatorDefaultBytes);

        // Create a new collection, insert three records and check it's valid.
        lockDb(MODE_X);
        OpDebug* const nullOpDebug = nullptr;

        RecordId id1;
        {
            beginTransaction();
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);

            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 1 << "a" << 1)), nullOpDebug, true));
            id1 = coll()->getCursor(&_opCtx)->next()->id;
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 2 << "a" << 2)), nullOpDebug, true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 3 << "b" << 1)), nullOpDebug, true));
            commitTransaction();
        }

        const std::string indexName = "bad_index";
        auto status = createIndexFromSpec(BSON("name" << indexName << "key" << BSON("a" << 1) << "v"
                                                      << static_cast<int>(kIndexVersion)));

        ASSERT_OK(status);
        releaseDb();
        ensureValidateWorked();

        lockDb(MODE_X);

        // Replace a correct index entry with a bad one and check it's invalid.
        const IndexCatalog* indexCatalog = coll()->getIndexCatalog();
        auto descriptor = indexCatalog->findIndexByName(&_opCtx, indexName);
        auto iam = indexCatalog->getEntry(descriptor)->accessMethod()->asSortedData();

        {
            beginTransaction();
            int64_t numDeleted = 0;
            int64_t numInserted = 0;
            const BSONObj actualKey = BSON("a" << 1);
            const BSONObj badKey = BSON("a" << -1);
            InsertDeleteOptions options;
            options.dupsAllowed = true;

            KeyStringSet keys;
            iam->getKeys(
                &_opCtx,
                coll(),
                descriptor->getEntry(),
                pooledBuilder,
                actualKey,
                InsertDeleteOptions::ConstraintEnforcementMode::kRelaxConstraintsUnfiltered,
                SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                &keys,
                nullptr,
                nullptr,
                id1);

            auto removeStatus = iam->removeKeys(
                &_opCtx, descriptor->getEntry(), {keys.begin(), keys.end()}, options, &numDeleted);
            auto insertStatus = iam->insert(&_opCtx,
                                            pooledBuilder,
                                            coll(),
                                            descriptor->getEntry(),
                                            {{id1, timestampToUse, &badKey}},
                                            options,
                                            &numInserted);

            ASSERT_OK(removeStatus);
            ASSERT_OK(insertStatus);
            ASSERT_EQUALS(numDeleted, 1);
            ASSERT_EQUALS(numInserted, 1);
            commitTransaction();
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
            key_string::HeapBuilder::kHeapAllocatorDefaultBytes);

        // Create an index with bad index specs.
        lockDb(MODE_X);

        const std::string indexName = "bad_specs_index";
        auto status = createIndexFromSpec(BSON("name" << indexName << "key" << BSON("a" << 1) << "v"
                                                      << static_cast<int>(kIndexVersion) << "sparse"
                                                      << "false"));

        ASSERT_OK(status);
        releaseDb();
        ensureValidateWarned();
    }
};

template <bool full, bool background>
class ValidateWildCardIndex : public ValidateBase {
public:
    ValidateWildCardIndex() : ValidateBase(full, background) {}

    void run() {
        // Create a new collection.
        lockDb(MODE_X);

        {
            beginTransaction();
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);
            commitTransaction();
        }

        // Create a $** index.
        const auto indexName = "wildcardIndex";
        const auto indexKey = BSON("$**" << 1);
        auto status = createIndexFromSpec(BSON("name" << indexName << "key" << indexKey << "v"
                                                      << static_cast<int>(kIndexVersion)));
        ASSERT_OK(status);

        // Insert non-multikey documents.
        OpDebug* const nullOpDebug = nullptr;
        lockDb(MODE_X);
        {
            beginTransaction();
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
            commitTransaction();
        }
        releaseDb();
        ensureValidateWorked();

        // Insert multikey documents.
        lockDb(MODE_X);
        {
            beginTransaction();
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
            commitTransaction();
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
            beginTransaction();
            const key_string::Value indexKey =
                key_string::HeapBuilder(sortedDataInterface->getKeyStringVersion(),
                                        BSON("" << 1 << ""
                                                << "non_existent_path"),
                                        sortedDataInterface->getOrdering(),
                                        recordId)
                    .release();
            ASSERT_SDI_INSERT_OK(
                sortedDataInterface->insert(&_opCtx, indexKey, true /* dupsAllowed */));
            commitTransaction();
        }

        // An index whose set of multikey metadata paths is a superset of collection multikey
        // metadata paths is valid.
        releaseDb();
        ensureValidateWorked();

        // Remove the multikey path metadata index key for a path that exists and is multikey in the
        // collection.
        lockDb(MODE_X);
        {
            beginTransaction();
            const key_string::Value indexKey =
                key_string::HeapBuilder(sortedDataInterface->getKeyStringVersion(),
                                        BSON("" << 1 << ""
                                                << "mk_1"),
                                        sortedDataInterface->getOrdering(),
                                        recordId)
                    .release();
            sortedDataInterface->unindex(&_opCtx, indexKey, true /* dupsAllowed */);
            commitTransaction();
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
        // Create a new collection.
        lockDb(MODE_X);

        {
            beginTransaction();
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);
            commitTransaction();
        }

        // Create a $** index with a projection on "a".
        const auto indexName = "wildcardIndex";
        const auto indexKey = BSON("a.$**" << 1);
        auto status = createIndexFromSpec(BSON("name" << indexName << "key" << indexKey << "v"
                                                      << static_cast<int>(kIndexVersion)));
        ASSERT_OK(status);

        // Insert documents with indexed and not-indexed paths.
        OpDebug* const nullOpDebug = nullptr;
        lockDb(MODE_X);
        {
            beginTransaction();
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
            commitTransaction();
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
            beginTransaction();
            RecordId recordId(record_id_helpers::reservedIdFor(
                record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, KeyFormat::Long));
            const key_string::Value indexKey =
                key_string::HeapBuilder(sortedDataInterface->getKeyStringVersion(),
                                        BSON("" << 1 << ""
                                                << "a"),
                                        sortedDataInterface->getOrdering(),
                                        recordId)
                    .release();
            sortedDataInterface->unindex(&_opCtx, indexKey, true /* dupsAllowed */);
            commitTransaction();
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
        // Create a new collection.
        lockDb(MODE_X);

        {
            beginTransaction();
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);
            commitTransaction();
        }

        // Create an index.
        const auto indexName = "a";
        const auto indexKey = BSON("a" << 1);
        auto status = createIndexFromSpec(BSON("name" << indexName << "key" << indexKey << "v"
                                                      << static_cast<int>(kIndexVersion)));
        ASSERT_OK(status);

        // Insert documents.
        OpDebug* const nullOpDebug = nullptr;
        RecordId rid = RecordId::minLong();
        lockDb(MODE_X);
        {
            beginTransaction();
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 1 << "a" << 1)), nullOpDebug, true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 2 << "a" << 2)), nullOpDebug, true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 3 << "a" << 3)), nullOpDebug, true));
            rid = coll()->getCursor(&_opCtx)->next()->id;
            commitTransaction();
        }
        releaseDb();
        ensureValidateWorked();

        RecordStore* rs = coll()->getRecordStore();

        // Updating a document without updating the index entry should cause us to have a missing
        // index entry and an extra index entry.
        lockDb(MODE_X);
        {
            beginTransaction();
            auto doc = BSON("_id" << 1 << "a" << 5);
            auto updateStatus = rs->updateRecord(&_opCtx, rid, doc.objdata(), doc.objsize());
            ASSERT_OK(updateStatus);
            commitTransaction();
        }
        releaseDb();

        {
            ValidateResults results;

            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{CollectionValidation::ValidateMode::kForegroundFull,
                                  CollectionValidation::RepairMode::kNone,
                                  kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(false, results.isValid());
            ASSERT_EQ(static_cast<size_t>(1), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(2), totalNonTransientWarnings(results));
            ASSERT_EQ(static_cast<size_t>(1), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(1), results.getMissingIndexEntries().size());

            dumpOnErrorGuard.dismiss();
        }
    }
};

template <bool full, bool background>
class ValidateMissingIndexEntryResults : public ValidateBase {
public:
    ValidateMissingIndexEntryResults() : ValidateBase(full, background) {}

    void run() {
        SharedBufferFragmentBuilder pooledBuilder(
            key_string::HeapBuilder::kHeapAllocatorDefaultBytes);

        // Create a new collection.
        lockDb(MODE_X);

        {
            beginTransaction();
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);
            commitTransaction();
        }

        // Create an index.
        const auto indexName = "a";
        const auto indexKey = BSON("a" << 1);
        auto status = createIndexFromSpec(BSON("name" << indexName << "key" << indexKey << "v"
                                                      << static_cast<int>(kIndexVersion)));
        ASSERT_OK(status);

        // Insert documents.
        OpDebug* const nullOpDebug = nullptr;
        RecordId rid = RecordId::minLong();
        lockDb(MODE_X);
        {
            beginTransaction();
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 1 << "a" << 1)), nullOpDebug, true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 2 << "a" << 2)), nullOpDebug, true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 3 << "a" << 3)), nullOpDebug, true));
            rid = coll()->getCursor(&_opCtx)->next()->id;
            commitTransaction();
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

            beginTransaction();
            int64_t numDeleted;
            const BSONObj actualKey = BSON("a" << 1);
            InsertDeleteOptions options;
            options.dupsAllowed = true;

            KeyStringSet keys;
            iam->getKeys(
                &_opCtx,
                coll(),
                descriptor->getEntry(),
                pooledBuilder,
                actualKey,
                InsertDeleteOptions::ConstraintEnforcementMode::kRelaxConstraintsUnfiltered,
                SortedDataIndexAccessMethod::GetKeysContext::kRemovingKeys,
                &keys,
                nullptr,
                nullptr,
                rid);
            auto removeStatus = iam->removeKeys(
                &_opCtx, descriptor->getEntry(), {keys.begin(), keys.end()}, options, &numDeleted);

            ASSERT_EQUALS(numDeleted, 1);
            ASSERT_OK(removeStatus);
            commitTransaction();

            releaseDb();
        }

        {
            ValidateResults results;

            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{CollectionValidation::ValidateMode::kForegroundFull,
                                  CollectionValidation::RepairMode::kNone,
                                  kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(false, results.isValid());
            ASSERT_EQ(static_cast<size_t>(1), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(1), totalNonTransientWarnings(results));
            ASSERT_EQ(static_cast<size_t>(0), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(1), results.getMissingIndexEntries().size());

            dumpOnErrorGuard.dismiss();
        }
    }
};

template <bool full, bool background>
class ValidateExtraIndexEntryResults : public ValidateBase {
public:
    ValidateExtraIndexEntryResults() : ValidateBase(full, background) {}

    void run() {
        // Create a new collection.
        lockDb(MODE_X);

        {
            beginTransaction();
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);
            commitTransaction();
        }

        // Create an index.
        const auto indexName = "a";
        const auto indexKey = BSON("a" << 1);
        auto status = createIndexFromSpec(BSON("name" << indexName << "key" << indexKey << "v"
                                                      << static_cast<int>(kIndexVersion)));
        ASSERT_OK(status);

        // Insert documents.
        OpDebug* const nullOpDebug = nullptr;
        RecordId rid = RecordId::minLong();
        lockDb(MODE_X);
        {
            beginTransaction();
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 1 << "a" << 1)), nullOpDebug, true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 2 << "a" << 2)), nullOpDebug, true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 3 << "a" << 3)), nullOpDebug, true));
            rid = coll()->getCursor(&_opCtx)->next()->id;
            commitTransaction();
        }
        releaseDb();
        ensureValidateWorked();

        // Removing a document without removing the index entries should cause us to have extra
        // index entries.
        {
            lockDb(MODE_X);
            RecordStore* rs = coll()->getRecordStore();

            beginTransaction();
            rs->deleteRecord(&_opCtx, rid);
            commitTransaction();
            releaseDb();
        }

        {
            ValidateResults results;

            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{CollectionValidation::ValidateMode::kForegroundFull,
                                  CollectionValidation::RepairMode::kNone,
                                  kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(false, results.isValid());
            ASSERT_EQ(static_cast<size_t>(2), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(1), totalNonTransientWarnings(results));
            ASSERT_EQ(static_cast<size_t>(2), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), results.getMissingIndexEntries().size());

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
            beginTransaction();
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);
            commitTransaction();
        }

        // Create index "a".
        const auto indexNameA = "a";
        const auto indexKeyA = BSON("a" << 1);
        ASSERT_OK(createIndexFromSpec(BSON("name" << indexNameA << "key" << indexKeyA << "v"
                                                  << static_cast<int>(kIndexVersion))));

        // Create index "b".
        const auto indexNameB = "b";
        const auto indexKeyB = BSON("b" << 1);
        ASSERT_OK(createIndexFromSpec(BSON("name" << indexNameB << "key" << indexKeyB << "v"
                                                  << static_cast<int>(kIndexVersion))));

        // Insert documents.
        OpDebug* const nullOpDebug = nullptr;
        lockDb(MODE_X);
        {
            beginTransaction();
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
            commitTransaction();
        }
        releaseDb();
        ensureValidateWorked();

        RecordStore* rs = coll()->getRecordStore();

        // Updating documents without updating the index entries should cause us to have missing and
        // extra index entries.
        lockDb(MODE_X);
        {
            beginTransaction();
            auto doc1 = BSON("_id" << 1 << "a" << 8 << "b" << 8);
            auto doc2 = BSON("_id" << 2 << "a" << 3 << "b" << 7);
            std::unique_ptr<SeekableRecordCursor> cursor = coll()->getCursor(&_opCtx);
            auto record = cursor->next();
            RecordId rid = record->id;
            ASSERT_OK(rs->updateRecord(&_opCtx, rid, doc1.objdata(), doc1.objsize()));
            record = cursor->next();
            rid = record->id;
            ASSERT_OK(rs->updateRecord(&_opCtx, rid, doc2.objdata(), doc2.objsize()));
            commitTransaction();
        }
        releaseDb();

        // Confirm missing and extra index entries are detected.
        {
            ValidateResults results;

            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{CollectionValidation::ValidateMode::kForegroundFull,
                                  CollectionValidation::RepairMode::kNone,
                                  kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(false, results.isValid());
            ASSERT_EQ(static_cast<size_t>(2), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(2), totalNonTransientWarnings(results));
            ASSERT_EQ(static_cast<size_t>(3), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(3), results.getMissingIndexEntries().size());

            dumpOnErrorGuard.dismiss();
        }

        // Run validate with repair, expect extra index entries are removed and missing index
        // entries are inserted.
        {
            ValidateResults results;

            // Validate in repair mode can only be used in standalone mode, so ignore timestamp
            // assertions.
            shard_role_details::getRecoveryUnit(&_opCtx)->allowAllUntimestampedWrites();
            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{CollectionValidation::ValidateMode::kForegroundFull,
                                  CollectionValidation::RepairMode::kFixErrors,
                                  kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });


            ASSERT_EQ(true, results.isValid());
            ASSERT_EQ(true, results.getRepaired());
            ASSERT_EQ(static_cast<size_t>(0), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(2), totalNonTransientWarnings(results));
            ASSERT_EQ(static_cast<size_t>(0), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), results.getMissingIndexEntries().size());
            ASSERT_EQ(3, results.getNumRemovedExtraIndexEntries());
            ASSERT_EQ(3, results.getNumInsertedMissingIndexEntries());

            ASSERT_EQ(3, results.getIndexValidateResult(indexNameA).getKeysTraversed());
            ASSERT_EQ(3, results.getIndexValidateResult(indexNameB).getKeysTraversed());

            dumpOnErrorGuard.dismiss();
        }

        // Confirm repair worked such that results is now valid and no errors were suppressed by
        // repair.
        {
            ValidateResults results;

            // Validate in repair mode can only be used in standalone mode, so ignore timestamp
            // assertions.
            shard_role_details::getRecoveryUnit(&_opCtx)->allowAllUntimestampedWrites();
            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{CollectionValidation::ValidateMode::kForegroundFull,
                                  CollectionValidation::RepairMode::kFixErrors,
                                  kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.isValid());
            ASSERT_EQ(false, results.getRepaired());
            ASSERT_EQ(static_cast<size_t>(0), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(0), totalNonTransientWarnings(results));
            ASSERT_EQ(static_cast<size_t>(0), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), results.getMissingIndexEntries().size());
            ASSERT_EQ(0, results.getNumRemovedExtraIndexEntries());
            ASSERT_EQ(0, results.getNumInsertedMissingIndexEntries());

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
            key_string::HeapBuilder::kHeapAllocatorDefaultBytes);

        // Create a new collection.
        lockDb(MODE_X);

        {
            beginTransaction();
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);
            commitTransaction();
        }

        // Create an index.
        const auto indexName = "a";
        const auto indexKey = BSON("a" << 1);
        auto status = createIndexFromSpec(BSON("name" << indexName << "key" << indexKey << "v"
                                                      << static_cast<int>(kIndexVersion)));
        ASSERT_OK(status);

        // Insert documents.
        OpDebug* const nullOpDebug = nullptr;
        RecordId rid = RecordId::minLong();
        lockDb(MODE_X);
        {
            beginTransaction();
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 1 << "a" << 1)), nullOpDebug, true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 2 << "a" << 2)), nullOpDebug, true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 3 << "a" << 3)), nullOpDebug, true));
            rid = coll()->getCursor(&_opCtx)->next()->id;
            commitTransaction();
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

            beginTransaction();
            int64_t numDeleted;
            const BSONObj actualKey = BSON("a" << 1);
            InsertDeleteOptions options;
            options.dupsAllowed = true;

            KeyStringSet keys;
            iam->getKeys(
                &_opCtx,
                coll(),
                descriptor->getEntry(),
                pooledBuilder,
                actualKey,
                InsertDeleteOptions::ConstraintEnforcementMode::kRelaxConstraintsUnfiltered,
                SortedDataIndexAccessMethod::GetKeysContext::kRemovingKeys,
                &keys,
                nullptr,
                nullptr,
                rid);
            auto removeStatus = iam->removeKeys(
                &_opCtx, descriptor->getEntry(), {keys.begin(), keys.end()}, options, &numDeleted);

            ASSERT_EQUALS(numDeleted, 1);
            ASSERT_OK(removeStatus);
            commitTransaction();

            releaseDb();
        }

        // Confirm validate detects missing index entries.
        {
            ValidateResults results;

            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{CollectionValidation::ValidateMode::kForegroundFull,
                                  CollectionValidation::RepairMode::kNone,
                                  kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(false, results.isValid());
            ASSERT_EQ(false, results.getRepaired());
            ASSERT_EQ(static_cast<size_t>(1), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(1), totalNonTransientWarnings(results));
            ASSERT_EQ(static_cast<size_t>(0), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(1), results.getMissingIndexEntries().size());
            ASSERT_EQ(0, results.getNumRemovedExtraIndexEntries());
            ASSERT_EQ(0, results.getNumInsertedMissingIndexEntries());

            dumpOnErrorGuard.dismiss();
        }

        // Run validate with repair, expect missing index entries are inserted.
        {
            ValidateResults results;

            // Validate in repair mode can only be used in standalone mode, so ignore timestamp
            // assertions.
            shard_role_details::getRecoveryUnit(&_opCtx)->allowAllUntimestampedWrites();
            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{CollectionValidation::ValidateMode::kForegroundFull,
                                  CollectionValidation::RepairMode::kFixErrors,
                                  kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.isValid());
            ASSERT_EQ(true, results.isValid());
            ASSERT_EQ(static_cast<size_t>(0), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(1), totalNonTransientWarnings(results));
            ASSERT_EQ(static_cast<size_t>(0), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), results.getMissingIndexEntries().size());
            ASSERT_EQ(0, results.getNumRemovedExtraIndexEntries());
            ASSERT_EQ(1, results.getNumInsertedMissingIndexEntries());

            dumpOnErrorGuard.dismiss();
        }

        // Confirm missing index entries have been inserted such that results are valid and no
        // errors were suppressed by repair.
        {
            ValidateResults results;

            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{CollectionValidation::ValidateMode::kForegroundFull,
                                  CollectionValidation::RepairMode::kNone,
                                  kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.isValid());
            ASSERT_EQ(false, results.getRepaired());
            ASSERT_EQ(static_cast<size_t>(0), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(0), totalNonTransientWarnings(results));
            ASSERT_EQ(static_cast<size_t>(0), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), results.getMissingIndexEntries().size());
            ASSERT_EQ(0, results.getNumRemovedExtraIndexEntries());
            ASSERT_EQ(0, results.getNumInsertedMissingIndexEntries());

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
            beginTransaction();
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);
            commitTransaction();
        }

        // Create an index.
        const auto indexName = "a";
        const auto indexKey = BSON("a" << 1);
        auto status = createIndexFromSpec(BSON("name" << indexName << "key" << indexKey << "v"
                                                      << static_cast<int>(kIndexVersion)));
        ASSERT_OK(status);

        // Insert documents.
        OpDebug* const nullOpDebug = nullptr;
        RecordId rid = RecordId::minLong();
        lockDb(MODE_X);
        {
            beginTransaction();
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 1 << "a" << 1)), nullOpDebug, true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 2 << "a" << 2)), nullOpDebug, true));
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 3 << "a" << 3)), nullOpDebug, true));
            rid = coll()->getCursor(&_opCtx)->next()->id;
            commitTransaction();
        }
        releaseDb();
        ensureValidateWorked();

        // Removing a document without removing the index entries should cause us to have extra
        // index entries.
        {
            lockDb(MODE_X);
            RecordStore* rs = coll()->getRecordStore();

            beginTransaction();
            rs->deleteRecord(&_opCtx, rid);
            commitTransaction();
            releaseDb();
        }

        // Confirm validation detects extra index entries error.
        {
            ValidateResults results;

            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{CollectionValidation::ValidateMode::kForegroundFull,
                                  CollectionValidation::RepairMode::kNone,
                                  kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(false, results.isValid());
            ASSERT_EQ(false, results.getRepaired());
            ASSERT_EQ(static_cast<size_t>(2), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(1), totalNonTransientWarnings(results));
            ASSERT_EQ(static_cast<size_t>(2), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), results.getMissingIndexEntries().size());
            ASSERT_EQ(0, results.getNumRemovedExtraIndexEntries());
            ASSERT_EQ(0, results.getNumInsertedMissingIndexEntries());

            dumpOnErrorGuard.dismiss();
        }

        // Run validate with repair, expect extra index entries are removed.
        {
            ValidateResults results;

            // Validate in repair mode can only be used in standalone mode, so ignore timestamp
            // assertions.
            shard_role_details::getRecoveryUnit(&_opCtx)->allowAllUntimestampedWrites();
            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{CollectionValidation::ValidateMode::kForegroundFull,
                                  CollectionValidation::RepairMode::kFixErrors,
                                  kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.isValid());
            ASSERT_EQ(true, results.getRepaired());
            ASSERT_EQ(static_cast<size_t>(0), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(1), totalNonTransientWarnings(results));
            ASSERT_EQ(static_cast<size_t>(0), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), results.getMissingIndexEntries().size());
            ASSERT_EQ(2, results.getNumRemovedExtraIndexEntries());
            ASSERT_EQ(0, results.getNumInsertedMissingIndexEntries());

            dumpOnErrorGuard.dismiss();
        }

        // Confirm extra index entries are removed such that results are valid.
        {
            ValidateResults results;

            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{CollectionValidation::ValidateMode::kForegroundFull,
                                  CollectionValidation::RepairMode::kNone,
                                  kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.isValid());
            ASSERT_EQ(false, results.getRepaired());
            ASSERT_EQ(static_cast<size_t>(0), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(0), totalNonTransientWarnings(results));
            ASSERT_EQ(static_cast<size_t>(0), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), results.getMissingIndexEntries().size());
            ASSERT_EQ(0, results.getNumRemovedExtraIndexEntries());
            ASSERT_EQ(0, results.getNumInsertedMissingIndexEntries());

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
            key_string::HeapBuilder::kHeapAllocatorDefaultBytes);

        // Create a new collection and insert a document.
        lockDb(MODE_X);

        OpDebug* const nullOpDebug = nullptr;
        {
            beginTransaction();
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 1 << "a" << 1)), nullOpDebug, true));
            commitTransaction();
        }

        // Create a unique index.
        const auto indexName = "a";
        {
            const auto indexKey = BSON("a" << 1);
            auto status = createIndexFromSpec(BSON("name" << indexName << "key" << indexKey << "v"
                                                          << static_cast<int>(kIndexVersion)
                                                          << "unique" << true));
            ASSERT_OK(status);
        }

        // Confirm that inserting a document with the same value for "a" fails, verifying the
        // uniqueness constraint.
        BSONObj dupObj = BSON("_id" << 2 << "a" << 1);
        {
            beginTransaction();
            ASSERT_NOT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(dupObj), nullOpDebug, true));
            abortTransaction();
        }
        releaseDb();
        ensureValidateWorked();

        // Insert a document with a duplicate key for "a" but do not insert key into index a.
        RecordId rid;
        {
            lockDb(MODE_X);

            const IndexCatalog* indexCatalog = coll()->getIndexCatalog();

            InsertDeleteOptions options;
            options.dupsAllowed = true;

            beginTransaction();

            // Insert a record and its keys separately. We do this to bypass duplicate constraint
            // checking. Inserting a record and its keys ensures that validation fails
            // because there are duplicate keys, and not just because there are keys without
            // corresponding records.
            auto swRecordId = coll()->getRecordStore()->insertRecord(
                &_opCtx, dupObj.objdata(), dupObj.objsize(), timestampToUse);
            ASSERT_OK(swRecordId);
            rid = swRecordId.getValue();

            commitTransaction();

            // Insert the key on _id.
            {
                auto descriptor = indexCatalog->findIdIndex(&_opCtx);
                auto entry = const_cast<IndexCatalogEntry*>(indexCatalog->getEntry(descriptor));
                auto iam = entry->accessMethod()->asSortedData();
                auto interceptor = std::make_unique<IndexBuildInterceptor>(&_opCtx, entry);

                KeyStringSet keys;
                iam->getKeys(&_opCtx,
                             coll(),
                             entry,
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
                    beginTransaction();

                    int64_t numInserted;
                    auto insertStatus = iam->insertKeysAndUpdateMultikeyPaths(
                        &_opCtx,
                        coll(),
                        entry,
                        {keys.begin(), keys.end()},
                        {},
                        MultikeyPaths{},
                        options,
                        [this, &entry, &interceptor](const key_string::Value& duplicateKey) {
                            return interceptor->recordDuplicateKey(&_opCtx, entry, duplicateKey);
                        },
                        &numInserted);

                    ASSERT_EQUALS(numInserted, 1);
                    ASSERT_OK(insertStatus);

                    commitTransaction();
                }

                ASSERT_OK(interceptor->checkDuplicateKeyConstraints(&_opCtx, coll(), entry));
            }

            releaseDb();
        }

        // Confirm validation detects missing index entry.
        {
            ValidateResults results;

            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{CollectionValidation::ValidateMode::kForegroundFull,
                                  CollectionValidation::RepairMode::kNone,
                                  kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(false, results.isValid());
            ASSERT_EQ(false, results.getRepaired());
            ASSERT_EQ(static_cast<size_t>(1), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(1), totalNonTransientWarnings(results));
            ASSERT_EQ(static_cast<size_t>(0), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(1), results.getMissingIndexEntries().size());

            dumpOnErrorGuard.dismiss();
        }

        // Run validate with repair, expect missing index entry of duplicate document is removed
        // from record store and results are valid.
        {
            ValidateResults results;

            // Validate in repair mode can only be used in standalone mode, so ignore timestamp
            // assertions.
            shard_role_details::getRecoveryUnit(&_opCtx)->allowAllUntimestampedWrites();
            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{CollectionValidation::ValidateMode::kForegroundFull,
                                  CollectionValidation::RepairMode::kFixErrors,
                                  kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.isValid());
            ASSERT_EQ(true, results.getRepaired());
            ASSERT_EQ(static_cast<size_t>(0), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(1), totalNonTransientWarnings(results));
            ASSERT_EQ(static_cast<size_t>(0), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), results.getMissingIndexEntries().size());
            ASSERT_EQ(0, results.getNumRemovedExtraIndexEntries());
            ASSERT_EQ(0, results.getNumInsertedMissingIndexEntries());
            ASSERT_EQ(1, results.getNumDocumentsMovedToLostAndFound());

            ASSERT_EQ(1, results.getIndexValidateResult(indexName).getKeysRemovedFromRecordStore());

            // Verify the older duplicate document appears in the lost-and-found as expected.
            {
                const NamespaceString lostAndFoundNss = NamespaceString::makeLocalCollection(
                    "lost_and_found." + coll()->uuid().toString());
                AutoGetCollectionForRead autoColl(&_opCtx, lostAndFoundNss);
                Snapshotted<BSONObj> result;
                ASSERT(autoColl.getCollection()->findDoc(&_opCtx, RecordId(1), &result));
                ASSERT_BSONOBJ_EQ(result.value(), fromjson("{_id:1, a:1}"));
            }

            // Verify the newer duplicate document still appears in the collection as expected.
            {
                AutoGetCollectionForRead autoColl(&_opCtx, _nss);
                Snapshotted<BSONObj> result;
                ASSERT(autoColl.getCollection()->findDoc(&_opCtx, RecordId(3), &result));
                ASSERT_BSONOBJ_EQ(result.value(), fromjson("{_id:2, a:1}"));
            }

            dumpOnErrorGuard.dismiss();
        }

        // Confirm duplicate document of missing index entries are removed such that results are
        // valid.
        {
            ValidateResults results;

            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{CollectionValidation::ValidateMode::kForegroundFull,
                                  CollectionValidation::RepairMode::kNone,
                                  kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.isValid());
            ASSERT_EQ(false, results.getRepaired());
            ASSERT_EQ(static_cast<size_t>(0), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(0), totalNonTransientWarnings(results));
            ASSERT_EQ(static_cast<size_t>(0), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), results.getMissingIndexEntries().size());
            ASSERT_EQ(0, results.getNumRemovedExtraIndexEntries());
            ASSERT_EQ(0, results.getNumInsertedMissingIndexEntries());
            ASSERT_EQ(0, results.getNumDocumentsMovedToLostAndFound());

            ASSERT_EQ(0, results.getIndexValidateResult(indexName).getKeysRemovedFromRecordStore());

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
            key_string::HeapBuilder::kHeapAllocatorDefaultBytes);

        // Create a new collection and insert a document.
        lockDb(MODE_X);

        OpDebug* const nullOpDebug = nullptr;
        {
            beginTransaction();
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx,
                coll(),
                InsertStatement(BSON("_id" << 1 << "a" << 1 << "b" << 1)),
                nullOpDebug,
                true));
            commitTransaction();
        }

        // Create unique indexes.
        const auto indexNameA = "a";
        {
            const auto indexKeyA = BSON("a" << 1);
            auto status = createIndexFromSpec(BSON("name" << indexNameA << "key" << indexKeyA << "v"
                                                          << static_cast<int>(kIndexVersion)
                                                          << "unique" << true));
            ASSERT_OK(status);
        }

        const auto indexNameB = "b";
        {
            const auto indexKeyB = BSON("b" << 1);
            auto status = createIndexFromSpec(BSON("name" << indexNameB << "key" << indexKeyB << "v"
                                                          << static_cast<int>(kIndexVersion)
                                                          << "unique" << true));
            ASSERT_OK(status);
        }


        // Confirm that inserting a document with the same value for "a" and "b" fails, verifying
        // the uniqueness constraint.
        BSONObj dupObj = BSON("_id" << 2 << "a" << 1 << "b" << 1);
        {
            beginTransaction();
            ASSERT_NOT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(dupObj), nullOpDebug, true));
            abortTransaction();
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
            options.dupsAllowed = true;

            beginTransaction();

            // Insert a record and its keys separately. We do this to bypass duplicate constraint
            // checking. Inserting a record without inserting keys results in the duplicate record
            // to be missing from both unique indexes.
            auto swRecordId = coll()->getRecordStore()->insertRecord(
                &_opCtx, dupObj.objdata(), dupObj.objsize(), timestampToUse);
            ASSERT_OK(swRecordId);
            rid = swRecordId.getValue();

            commitTransaction();

            // Insert the key on _id.
            {
                auto descriptor = indexCatalog->findIdIndex(&_opCtx);
                auto entry = const_cast<IndexCatalogEntry*>(indexCatalog->getEntry(descriptor));
                auto iam = entry->accessMethod()->asSortedData();
                auto interceptor = std::make_unique<IndexBuildInterceptor>(&_opCtx, entry);

                KeyStringSet keys;
                iam->getKeys(&_opCtx,
                             coll(),
                             entry,
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
                    beginTransaction();

                    int64_t numInserted;
                    auto insertStatus = iam->insertKeysAndUpdateMultikeyPaths(
                        &_opCtx,
                        coll(),
                        entry,
                        {keys.begin(), keys.end()},
                        {},
                        MultikeyPaths{},
                        options,
                        [this, &entry, &interceptor](const key_string::Value& duplicateKey) {
                            return interceptor->recordDuplicateKey(&_opCtx, entry, duplicateKey);
                        },
                        &numInserted);

                    ASSERT_EQUALS(numInserted, 1);
                    ASSERT_OK(insertStatus);

                    commitTransaction();
                }

                ASSERT_OK(interceptor->checkDuplicateKeyConstraints(&_opCtx, coll(), entry));
            }

            releaseDb();
        }

        // Confirm validation detects missing index entries.
        {
            ValidateResults results;

            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{CollectionValidation::ValidateMode::kForegroundFull,
                                  CollectionValidation::RepairMode::kNone,
                                  kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(false, results.isValid());
            ASSERT_EQ(false, results.getRepaired());
            ASSERT_EQ(static_cast<size_t>(2), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(1), totalNonTransientWarnings(results));
            ASSERT_EQ(static_cast<size_t>(0), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(2), results.getMissingIndexEntries().size());

            dumpOnErrorGuard.dismiss();
        }

        // Run validate with repair, expect missing index entry document is removed from record
        // store and no action is taken on outdated missing index entry.
        // Results will not be valid because IndexInfo.numKeys is not subtracted from when
        // deleteDocument is called. TODO SERVER-62257: Update test to expect valid results when
        // numKeys can be correctly updated.
        {
            ValidateResults results;

            // Validate in repair mode can only be used in standalone mode, so ignore timestamp
            // assertions.
            shard_role_details::getRecoveryUnit(&_opCtx)->allowAllUntimestampedWrites();
            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{CollectionValidation::ValidateMode::kForegroundFull,
                                  CollectionValidation::RepairMode::kFixErrors,
                                  kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(false, results.isValid());
            ASSERT_EQ(true, results.getRepaired());
            ASSERT_EQ(static_cast<size_t>(1), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(2), totalNonTransientWarnings(results));
            ASSERT_EQ(static_cast<size_t>(0), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), results.getMissingIndexEntries().size());
            ASSERT_EQ(0, results.getNumRemovedExtraIndexEntries());
            ASSERT_EQ(1, results.getNumInsertedMissingIndexEntries());
            ASSERT_EQ(1, results.getNumDocumentsMovedToLostAndFound());
            ASSERT_EQ(0, results.getNumOutdatedMissingIndexEntry());

            ASSERT_EQ(1,
                      results.getIndexValidateResult(indexNameA).getKeysRemovedFromRecordStore());
            ASSERT_EQ(0,
                      results.getIndexValidateResult(indexNameB).getKeysRemovedFromRecordStore());

            // Verify the older document appears in the lost-and-found as expected.
            {
                const NamespaceString lostAndFoundNss = NamespaceString::makeLocalCollection(
                    "lost_and_found." + coll()->uuid().toString());
                AutoGetCollectionForRead autoColl(&_opCtx, lostAndFoundNss);
                Snapshotted<BSONObj> result;
                ASSERT(autoColl.getCollection()->findDoc(&_opCtx, RecordId(1), &result));
                ASSERT_BSONOBJ_EQ(result.value(), fromjson("{_id:1, a:1, b:1}"));
            }

            // Verify the newer duplicate document still appears in the collection as expected.
            {
                AutoGetCollectionForRead autoColl(&_opCtx, _nss);
                Snapshotted<BSONObj> result;
                ASSERT(autoColl.getCollection()->findDoc(&_opCtx, RecordId(3), &result));
                ASSERT_BSONOBJ_EQ(result.value(), fromjson("{_id:2, a:1, b:1}"));
            }

            dumpOnErrorGuard.dismiss();
        }

        // Confirm extra index entries are removed such that results are valid.
        {
            ValidateResults results;

            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{CollectionValidation::ValidateMode::kForegroundFull,
                                  CollectionValidation::RepairMode::kNone,
                                  kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.isValid());
            ASSERT_EQ(false, results.getRepaired());
            ASSERT_EQ(static_cast<size_t>(0), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(0), totalNonTransientWarnings(results));
            ASSERT_EQ(static_cast<size_t>(0), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), results.getMissingIndexEntries().size());
            ASSERT_EQ(0, results.getNumRemovedExtraIndexEntries());
            ASSERT_EQ(0, results.getNumInsertedMissingIndexEntries());
            ASSERT_EQ(0, results.getNumDocumentsMovedToLostAndFound());
            ASSERT_EQ(0, results.getNumOutdatedMissingIndexEntry());

            ASSERT_EQ(0,
                      results.getIndexValidateResult(indexNameA).getKeysRemovedFromRecordStore());
            ASSERT_EQ(0,
                      results.getIndexValidateResult(indexNameB).getKeysRemovedFromRecordStore());

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
            key_string::HeapBuilder::kHeapAllocatorDefaultBytes);

        // Create a new collection and insert a document.
        lockDb(MODE_X);

        OpDebug* const nullOpDebug = nullptr;
        RecordId rid1 = RecordId::minLong();
        {
            beginTransaction();
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx,
                coll(),
                InsertStatement(BSON("_id" << 1 << "a" << 1 << "b" << 1)),
                nullOpDebug,
                true));
            rid1 = coll()->getCursor(&_opCtx)->next()->id;
            commitTransaction();
        }

        // Create unique indexes.
        const auto indexNameA = "a";
        {
            const auto indexKeyA = BSON("a" << 1);
            auto status = createIndexFromSpec(BSON("name" << indexNameA << "key" << indexKeyA << "v"
                                                          << static_cast<int>(kIndexVersion)
                                                          << "unique" << true));
            ASSERT_OK(status);
        }

        const auto indexNameB = "b";
        {
            const auto indexKeyB = BSON("b" << 1);
            auto status = createIndexFromSpec(BSON("name" << indexNameB << "key" << indexKeyB << "v"
                                                          << static_cast<int>(kIndexVersion)
                                                          << "unique" << true));
            ASSERT_OK(status);
        }


        // Confirm that inserting a document with the same value for "a" and "b" fails, verifying
        // the uniqueness constraint.
        BSONObj dupObj = BSON("_id" << 2 << "a" << 1 << "b" << 1);
        {
            beginTransaction();
            ASSERT_NOT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(dupObj), nullOpDebug, true));
            abortTransaction();
        }
        releaseDb();
        ensureValidateWorked();

        // Remove index entry of first document from index b to make it a missing index entry.
        {
            lockDb(MODE_X);

            const IndexCatalog* indexCatalog = coll()->getIndexCatalog();

            InsertDeleteOptions options;
            options.dupsAllowed = true;

            {
                auto descriptor = indexCatalog->findIndexByName(&_opCtx, indexNameB);
                auto iam = indexCatalog->getEntry(descriptor)->accessMethod()->asSortedData();

                beginTransaction();
                int64_t numDeleted;
                const BSONObj actualKey = BSON("b" << 1);

                KeyStringSet keys;
                iam->getKeys(
                    &_opCtx,
                    coll(),
                    descriptor->getEntry(),
                    pooledBuilder,
                    actualKey,
                    InsertDeleteOptions::ConstraintEnforcementMode::kRelaxConstraintsUnfiltered,
                    SortedDataIndexAccessMethod::GetKeysContext::kRemovingKeys,
                    &keys,
                    nullptr,
                    nullptr,
                    rid1);
                auto removeStatus = iam->removeKeys(&_opCtx,
                                                    descriptor->getEntry(),
                                                    {keys.begin(), keys.end()},
                                                    options,
                                                    &numDeleted);

                ASSERT_EQUALS(numDeleted, 1);
                ASSERT_OK(removeStatus);
                commitTransaction();
            }

            releaseDb();
        }

        // Insert a document with a duplicate key for "a" and "b".
        RecordId rid2;
        {
            lockDb(MODE_X);

            const IndexCatalog* indexCatalog = coll()->getIndexCatalog();

            InsertDeleteOptions options;
            options.dupsAllowed = true;

            beginTransaction();

            // Insert a record and its keys separately. We do this to bypass duplicate constraint
            // checking. Inserting a record and all of its keys ensures that validation fails
            // because there are duplicate keys, and not just because there are keys without
            // corresponding records.
            auto swRecordId = coll()->getRecordStore()->insertRecord(
                &_opCtx, dupObj.objdata(), dupObj.objsize(), timestampToUse);
            ASSERT_OK(swRecordId);
            rid2 = swRecordId.getValue();

            commitTransaction();

            // Insert the key on _id.
            {
                auto descriptor = indexCatalog->findIdIndex(&_opCtx);
                auto entry = const_cast<IndexCatalogEntry*>(indexCatalog->getEntry(descriptor));
                auto iam = entry->accessMethod()->asSortedData();
                auto interceptor = std::make_unique<IndexBuildInterceptor>(&_opCtx, entry);

                KeyStringSet keys;
                iam->getKeys(&_opCtx,
                             coll(),
                             entry,
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
                    beginTransaction();

                    int64_t numInserted;
                    auto insertStatus = iam->insertKeysAndUpdateMultikeyPaths(
                        &_opCtx,
                        coll(),
                        entry,
                        {keys.begin(), keys.end()},
                        {},
                        MultikeyPaths{},
                        options,
                        [this, &entry, &interceptor](const key_string::Value& duplicateKey) {
                            return interceptor->recordDuplicateKey(&_opCtx, entry, duplicateKey);
                        },
                        &numInserted);

                    ASSERT_EQUALS(numInserted, 1);
                    ASSERT_OK(insertStatus);

                    commitTransaction();
                }

                ASSERT_OK(interceptor->checkDuplicateKeyConstraints(&_opCtx, coll(), entry));
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
                             entry,
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
                    beginTransaction();

                    int64_t numInserted;
                    auto insertStatus = iam->insertKeysAndUpdateMultikeyPaths(
                        &_opCtx,
                        coll(),
                        entry,
                        {keys.begin(), keys.end()},
                        {},
                        MultikeyPaths{},
                        options,
                        [this, &entry, &interceptor](const key_string::Value& duplicateKey) {
                            return interceptor->recordDuplicateKey(&_opCtx, entry, duplicateKey);
                        },
                        &numInserted);

                    ASSERT_EQUALS(numInserted, 1);
                    ASSERT_OK(insertStatus);

                    commitTransaction();
                }

                ASSERT_OK(interceptor->checkDuplicateKeyConstraints(&_opCtx, coll(), entry));
            }

            releaseDb();
        }

        // Confirm validation detects missing index entries.
        {
            ValidateResults results;

            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{CollectionValidation::ValidateMode::kForegroundFull,
                                  CollectionValidation::RepairMode::kNone,
                                  kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(false, results.isValid());
            ASSERT_EQ(false, results.getRepaired());
            ASSERT_EQ(static_cast<size_t>(2), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(1), totalNonTransientWarnings(results));
            ASSERT_EQ(static_cast<size_t>(0), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(2), results.getMissingIndexEntries().size());

            dumpOnErrorGuard.dismiss();
        }

        // Run validate with repair, expect duplicate missing index entry document is removed from
        // record store and missing index entry is inserted into index. Results should be valid.
        {
            ValidateResults results;

            // Validate in repair mode can only be used in standalone mode, so ignore timestamp
            // assertions.
            shard_role_details::getRecoveryUnit(&_opCtx)->allowAllUntimestampedWrites();
            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{CollectionValidation::ValidateMode::kForegroundFull,
                                  CollectionValidation::RepairMode::kFixErrors,
                                  kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.isValid());
            ASSERT_EQ(true, results.getRepaired());
            ASSERT_EQ(static_cast<size_t>(0), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(1), totalNonTransientWarnings(results));
            ASSERT_EQ(static_cast<size_t>(0), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), results.getMissingIndexEntries().size());
            ASSERT_EQ(0, results.getNumRemovedExtraIndexEntries());
            ASSERT_EQ(0, results.getNumInsertedMissingIndexEntries());
            ASSERT_EQ(1, results.getNumDocumentsMovedToLostAndFound());
            ASSERT_EQ(1, results.getNumOutdatedMissingIndexEntry());

            ASSERT_EQ(1,
                      results.getIndexValidateResult(indexNameA).getKeysRemovedFromRecordStore());
            ASSERT_EQ(0,
                      results.getIndexValidateResult(indexNameB).getKeysRemovedFromRecordStore());

            // Verify the older duplicate document appears in the lost-and-found as expected.
            {
                const NamespaceString lostAndFoundNss = NamespaceString::makeLocalCollection(
                    "lost_and_found." + coll()->uuid().toString());
                AutoGetCollectionForRead autoColl(&_opCtx, lostAndFoundNss);
                Snapshotted<BSONObj> result;
                ASSERT(autoColl.getCollection()->findDoc(&_opCtx, RecordId(1), &result));
                ASSERT_BSONOBJ_EQ(result.value(), fromjson("{_id:1, a:1, b:1}"));
            }

            // Verify the newer duplicate document still appears in the collection as expected.
            {
                AutoGetCollectionForRead autoColl(&_opCtx, _nss);
                Snapshotted<BSONObj> result;
                ASSERT(autoColl.getCollection()->findDoc(&_opCtx, RecordId(3), &result));
                ASSERT_BSONOBJ_EQ(result.value(), fromjson("{_id:2, a:1, b:1}"));
            }

            dumpOnErrorGuard.dismiss();
        }

        // Confirm extra index entries are removed such that results are valid.
        {
            ValidateResults results;

            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{CollectionValidation::ValidateMode::kForegroundFull,
                                  CollectionValidation::RepairMode::kNone,
                                  kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.isValid());
            ASSERT_EQ(false, results.getRepaired());
            ASSERT_EQ(static_cast<size_t>(0), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(0), totalNonTransientWarnings(results));
            ASSERT_EQ(static_cast<size_t>(0), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), results.getMissingIndexEntries().size());
            ASSERT_EQ(0, results.getNumRemovedExtraIndexEntries());
            ASSERT_EQ(0, results.getNumInsertedMissingIndexEntries());
            ASSERT_EQ(0, results.getNumDocumentsMovedToLostAndFound());
            ASSERT_EQ(0, results.getNumOutdatedMissingIndexEntry());

            ASSERT_EQ(0,
                      results.getIndexValidateResult(indexNameA).getKeysRemovedFromRecordStore());
            ASSERT_EQ(0,
                      results.getIndexValidateResult(indexNameB).getKeysRemovedFromRecordStore());

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
            key_string::HeapBuilder::kHeapAllocatorDefaultBytes);

        // Create a new collection and insert non-multikey document.
        lockDb(MODE_X);

        RecordId id1;
        BSONObj doc = BSON("_id" << 1 << "a" << 1);
        {
            OpDebug* const nullOpDebug = nullptr;
            beginTransaction();
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);

            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(doc), nullOpDebug, true));
            id1 = coll()->getCursor(&_opCtx)->next()->id;
            commitTransaction();
        }

        // Create non-multikey index.
        const auto indexName = "non_mk_index";
        auto status = createIndexFromSpec(BSON("name" << indexName << "key" << BSON("a" << 1) << "v"
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

            // Remove non-multikey index entry.
            {
                beginTransaction();
                KeyStringSet keys;
                iam->getKeys(
                    &_opCtx,
                    coll(),
                    descriptor->getEntry(),
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
                auto removeStatus = iam->removeKeys(&_opCtx,
                                                    descriptor->getEntry(),
                                                    {keys.begin(), keys.end()},
                                                    options,
                                                    &numDeleted);
                ASSERT_OK(removeStatus);
                ASSERT_EQUALS(numDeleted, 1);
                commitTransaction();
            }

            // Update non-multikey document with multikey document.   {a: 1}   ->   {a: [2, 3]}
            BSONObj mkDoc = BSON("_id" << 1 << "a" << BSON_ARRAY(2 << 3));
            {
                beginTransaction();
                auto updateStatus = coll()->getRecordStore()->updateRecord(
                    &_opCtx, id1, mkDoc.objdata(), mkDoc.objsize());
                ASSERT_OK(updateStatus);
                commitTransaction();
            }

            // Not inserting keys into index should create missing multikey entries.
            releaseDb();
        }

        // Confirm missing multikey document found on non-multikey index error detected by validate.
        {
            ValidateResults results;
            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{CollectionValidation::ValidateMode::kForeground,
                                  CollectionValidation::RepairMode::kNone,
                                  kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(false, results.isValid());
            ASSERT_EQ(false, results.getRepaired());
            ASSERT_EQ(static_cast<size_t>(1), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(1), totalNonTransientWarnings(results));
            ASSERT_EQ(static_cast<size_t>(0), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(2), results.getMissingIndexEntries().size());
            ASSERT_EQ(0, results.getNumRemovedExtraIndexEntries());
            ASSERT_EQ(0, results.getNumInsertedMissingIndexEntries());

            dumpOnErrorGuard.dismiss();
        }

        // Run validate in repair mode.
        {
            ValidateResults results;

            // Validate in repair mode can only be used in standalone mode, so ignore timestamp
            // assertions.
            shard_role_details::getRecoveryUnit(&_opCtx)->allowAllUntimestampedWrites();
            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{CollectionValidation::ValidateMode::kForeground,
                                  CollectionValidation::RepairMode::kFixErrors,
                                  kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.isValid());
            ASSERT_EQ(true, results.getRepaired());
            ASSERT_EQ(static_cast<size_t>(0), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(2), totalNonTransientWarnings(results));
            ASSERT_EQ(static_cast<size_t>(0), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), results.getMissingIndexEntries().size());
            ASSERT_EQ(0, results.getNumRemovedExtraIndexEntries());
            ASSERT_EQ(2, results.getNumInsertedMissingIndexEntries());

            dumpOnErrorGuard.dismiss();
        }

        // Confirm index updated as multikey and missing index entries are inserted such that valid
        // is true.
        {
            ValidateResults results;

            // Validate in repair mode can only be used in standalone mode, so ignore timestamp
            // assertions.
            shard_role_details::getRecoveryUnit(&_opCtx)->allowAllUntimestampedWrites();
            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{CollectionValidation::ValidateMode::kForeground,
                                  CollectionValidation::RepairMode::kFixErrors,
                                  kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });
            StorageDebugUtil::printValidateResults(results);
            ASSERT_EQ(true, results.isValid());
            ASSERT_EQ(false, results.getRepaired());
            ASSERT_EQ(static_cast<size_t>(0), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(0), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), results.getMissingIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), totalNonTransientWarnings(results));
            ASSERT_EQ(0, results.getNumRemovedExtraIndexEntries());
            ASSERT_EQ(0, results.getNumInsertedMissingIndexEntries());

            dumpOnErrorGuard.dismiss();
        }
    }
};

class ValidateDuplicateDocumentIndexKeySet : public ValidateBase {
public:
    ValidateDuplicateDocumentIndexKeySet() : ValidateBase(/*full=*/false, /*background=*/false) {}

    void run() {
        SharedBufferFragmentBuilder pooledBuilder(
            key_string::HeapBuilder::kHeapAllocatorDefaultBytes);

        // Create a new collection.
        lockDb(MODE_X);

        {
            beginTransaction();
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);
            commitTransaction();
        }

        // Create two identical indexes only differing by key pattern and name.
        {
            const auto indexName = "a";
            const auto indexKey = BSON("a" << 1);
            auto status = createIndexFromSpec(BSON("name" << indexName << "key" << indexKey << "v"
                                                          << static_cast<int>(kIndexVersion)));
            ASSERT_OK(status);
        }

        {
            const auto indexName = "b";
            const auto indexKey = BSON("b" << 1);
            auto status = createIndexFromSpec(BSON("name" << indexName << "key" << indexKey << "v"
                                                          << static_cast<int>(kIndexVersion)));
            ASSERT_OK(status);
        }

        // Insert a document.
        OpDebug* const nullOpDebug = nullptr;
        RecordId rid = RecordId::minLong();
        lockDb(MODE_X);
        {
            beginTransaction();
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx,
                coll(),
                InsertStatement(BSON("_id" << 1 << "a" << 1 << "b" << 1)),
                nullOpDebug,
                true));
            rid = coll()->getCursor(&_opCtx)->next()->id;
            commitTransaction();
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

            beginTransaction();
            int64_t numDeleted;
            const BSONObj actualKey = BSON("a" << 1);
            InsertDeleteOptions options;
            options.dupsAllowed = true;

            KeyStringSet keys;
            iam->getKeys(
                &_opCtx,
                coll(),
                descriptor->getEntry(),
                pooledBuilder,
                actualKey,
                InsertDeleteOptions::ConstraintEnforcementMode::kRelaxConstraintsUnfiltered,
                SortedDataIndexAccessMethod::GetKeysContext::kRemovingKeys,
                &keys,
                nullptr,
                nullptr,
                rid);
            auto removeStatus = iam->removeKeys(
                &_opCtx, descriptor->getEntry(), {keys.begin(), keys.end()}, options, &numDeleted);

            ASSERT_EQUALS(numDeleted, 1);
            ASSERT_OK(removeStatus);
            commitTransaction();

            releaseDb();
        }

        // Remove the index entry for index "b".
        {
            lockDb(MODE_X);

            const IndexCatalog* indexCatalog = coll()->getIndexCatalog();
            const std::string indexName = "b";
            auto descriptor = indexCatalog->findIndexByName(&_opCtx, indexName);
            auto iam = indexCatalog->getEntry(descriptor)->accessMethod()->asSortedData();

            beginTransaction();
            int64_t numDeleted;
            const BSONObj actualKey = BSON("b" << 1);
            InsertDeleteOptions options;
            options.dupsAllowed = true;

            KeyStringSet keys;
            iam->getKeys(
                &_opCtx,
                coll(),
                descriptor->getEntry(),
                pooledBuilder,
                actualKey,
                InsertDeleteOptions::ConstraintEnforcementMode::kRelaxConstraintsUnfiltered,
                SortedDataIndexAccessMethod::GetKeysContext::kRemovingKeys,
                &keys,
                nullptr,
                nullptr,
                rid);
            auto removeStatus = iam->removeKeys(
                &_opCtx, descriptor->getEntry(), {keys.begin(), keys.end()}, options, &numDeleted);

            ASSERT_EQUALS(numDeleted, 1);
            ASSERT_OK(removeStatus);
            commitTransaction();

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
        SharedBufferFragmentBuilder pooledBuilder(
            key_string::HeapBuilder::kHeapAllocatorDefaultBytes);

        // Create a new collection.
        lockDb(MODE_X);

        {
            beginTransaction();
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);
            commitTransaction();
        }

        // Create a unique index.
        const auto indexName = "a";
        {
            const auto indexKey = BSON("a" << 1);
            auto status = createIndexFromSpec(BSON("name" << indexName << "key" << indexKey << "v"
                                                          << static_cast<int>(kIndexVersion)
                                                          << "unique" << true));
            ASSERT_OK(status);
        }

        // Insert a document.
        OpDebug* const nullOpDebug = nullptr;
        lockDb(MODE_X);
        {
            beginTransaction();
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(BSON("_id" << 1 << "a" << 1)), nullOpDebug, true));
            commitTransaction();
        }

        // Confirm that inserting a document with the same value for "a" fails, verifying the
        // uniqueness constraint.
        BSONObj dupObj = BSON("_id" << 2 << "a" << 1);
        {
            beginTransaction();
            ASSERT_NOT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(dupObj), nullOpDebug, true));
            abortTransaction();
        }
        releaseDb();
        ensureValidateWorked();

        // Insert a document with a duplicate key for "a".
        {
            lockDb(MODE_X);

            const IndexCatalog* indexCatalog = coll()->getIndexCatalog();

            InsertDeleteOptions options;
            options.dupsAllowed = true;

            beginTransaction();

            // Insert a record and its keys separately. We do this to bypass duplicate constraint
            // checking. Inserting a record and all of its keys ensures that validation fails
            // because there are duplicate keys, and not just because there are keys without
            // corresponding records.
            auto swRecordId = coll()->getRecordStore()->insertRecord(
                &_opCtx, dupObj.objdata(), dupObj.objsize(), timestampToUse);
            ASSERT_OK(swRecordId);

            commitTransaction();

            // Insert the key on _id.
            {
                auto descriptor = indexCatalog->findIdIndex(&_opCtx);
                auto entry = const_cast<IndexCatalogEntry*>(indexCatalog->getEntry(descriptor));
                auto iam = entry->accessMethod()->asSortedData();
                auto interceptor = std::make_unique<IndexBuildInterceptor>(&_opCtx, entry);

                KeyStringSet keys;
                iam->getKeys(&_opCtx,
                             coll(),
                             entry,
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
                    beginTransaction();

                    int64_t numInserted;
                    auto insertStatus = iam->insertKeysAndUpdateMultikeyPaths(
                        &_opCtx,
                        coll(),
                        entry,
                        {keys.begin(), keys.end()},
                        {},
                        MultikeyPaths{},
                        options,
                        [this, &entry, &interceptor](const key_string::Value& duplicateKey) {
                            return interceptor->recordDuplicateKey(&_opCtx, entry, duplicateKey);
                        },
                        &numInserted);

                    ASSERT_EQUALS(numInserted, 1);
                    ASSERT_OK(insertStatus);

                    commitTransaction();
                }

                ASSERT_OK(interceptor->checkDuplicateKeyConstraints(&_opCtx, coll(), entry));
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
                             entry,
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
                    beginTransaction();

                    int64_t numInserted;
                    auto insertStatus = iam->insertKeysAndUpdateMultikeyPaths(
                        &_opCtx,
                        coll(),
                        entry,
                        {keys.begin(), keys.end()},
                        {},
                        MultikeyPaths{},
                        options,
                        [this, &entry, &interceptor](const key_string::Value& duplicateKey) {
                            return interceptor->recordDuplicateKey(&_opCtx, entry, duplicateKey);
                        },
                        &numInserted);

                    ASSERT_EQUALS(numInserted, 1);
                    ASSERT_OK(insertStatus);

                    commitTransaction();
                }

                ASSERT_NOT_OK(interceptor->checkDuplicateKeyConstraints(&_opCtx, coll(), entry));
            }

            releaseDb();
        }

        ValidateResults results = runValidate();

        ScopeGuard dumpOnErrorGuard([&] {
            StorageDebugUtil::printValidateResults(results);
            StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
        });

        ASSERT_FALSE(results.isValid()) << "Validation worked when it should have failed.";
        ASSERT_EQ(static_cast<size_t>(1), totalErrors(results));
        ASSERT_EQ(static_cast<size_t>(0), totalNonTransientWarnings(results));
        ASSERT_EQ(static_cast<size_t>(0), results.getExtraIndexEntries().size());
        ASSERT_EQ(static_cast<size_t>(0), results.getMissingIndexEntries().size());

        dumpOnErrorGuard.dismiss();
    }
};

template <bool full, bool background>
class ValidateInvalidBSONResults : public ValidateBase {
public:
    ValidateInvalidBSONResults() : ValidateBase(full, background) {}

    void run() {
        // Create a new collection.
        lockDb(MODE_X);

        {
            beginTransaction();
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);
            commitTransaction();
        }

        // Encode an invalid BSON Object with an invalid type, x90 and insert record
        const char* buffer = "\x0c\x00\x00\x00\x90\x41\x00\x10\x00\x00\x00\x00";
        BSONObj obj(buffer);
        lockDb(MODE_X);
        RecordStore* rs = coll()->getRecordStore();
        RecordId rid;
        {
            beginTransaction();
            auto swRecordId =
                rs->insertRecord(&_opCtx, obj.objdata(), obj.objsize(), timestampToUse);
            ASSERT_OK(swRecordId);
            rid = swRecordId.getValue();
            commitTransaction();
        }
        releaseDb();

        {
            auto mode = _background ? CollectionValidation::ValidateMode::kBackground
                                    : CollectionValidation::ValidateMode::kForeground;

            ValidateResults results;

            // validate() will set a kProvided read source. Callers continue to do operations
            // after running validate, so we must reset the read source back to normal before
            // returning.
            auto originalReadSource =
                shard_role_details::getRecoveryUnit(&_opCtx)->getTimestampReadSource();
            ON_BLOCK_EXIT([&] {
                shard_role_details::getRecoveryUnit(&_opCtx)->abandonSnapshot();
                shard_role_details::getRecoveryUnit(&_opCtx)->setTimestampReadSource(
                    originalReadSource);
            });
            forceCheckpoint(_background);
            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{mode, CollectionValidation::RepairMode::kNone, kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(false, results.isValid());
            ASSERT_EQ(static_cast<size_t>(1), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(0), totalNonTransientWarnings(results));
            ASSERT_EQ(static_cast<size_t>(0), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), results.getMissingIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(1), results.getCorruptRecords().size());
            ASSERT_EQ(rid, results.getCorruptRecords()[0]);

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
            beginTransaction();
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);
            commitTransaction();
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
            beginTransaction();
            ASSERT_OK(rs->insertRecord(&_opCtx, obj1.objdata(), 12ULL, timestampToUse));
            ASSERT_OK(rs->insertRecord(&_opCtx, obj2.objdata(), 12ULL, timestampToUse));
            ASSERT_OK(rs->insertRecord(&_opCtx, obj3.objdata(), 12ULL, timestampToUse));
            commitTransaction();
        }
        releaseDb();

        // Confirm all of the different corrupt records previously inserted are detected by
        // validate.
        {
            ValidateResults results;

            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{CollectionValidation::ValidateMode::kForeground,
                                  CollectionValidation::RepairMode::kNone,
                                  kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(false, results.isValid());
            ASSERT_EQ(false, results.getRepaired());
            ASSERT_EQ(static_cast<size_t>(1), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(0), totalNonTransientWarnings(results));
            ASSERT_EQ(static_cast<size_t>(0), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), results.getMissingIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(3), results.getCorruptRecords().size());
            ASSERT_EQ(0, results.getNumRemovedCorruptRecords());

            dumpOnErrorGuard.dismiss();
        }

        // Run validate with repair, expect corrupted records are removed.
        {
            ValidateResults results;

            // Validate in repair mode can only be used in standalone mode, so ignore timestamp
            // assertions.
            shard_role_details::getRecoveryUnit(&_opCtx)->allowAllUntimestampedWrites();
            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{CollectionValidation::ValidateMode::kForeground,
                                  CollectionValidation::RepairMode::kFixErrors,
                                  kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.isValid());
            ASSERT_EQ(true, results.getRepaired());
            ASSERT_EQ(static_cast<size_t>(0), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(1), totalNonTransientWarnings(results));
            ASSERT_EQ(static_cast<size_t>(0), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), results.getMissingIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), results.getCorruptRecords().size());
            ASSERT_EQ(3, results.getNumRemovedCorruptRecords());

            // Check that the corrupted records have been removed from the record store.
            ASSERT_EQ(0, rs->numRecords(&_opCtx));

            dumpOnErrorGuard.dismiss();
        }

        // Confirm corrupt records have been removed such that repair does not need to run and
        // results are valid.
        {
            ValidateResults results;

            // Validate in repair mode can only be used in standalone mode, so ignore timestamp
            // assertions.
            shard_role_details::getRecoveryUnit(&_opCtx)->allowAllUntimestampedWrites();
            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{CollectionValidation::ValidateMode::kForeground,
                                  CollectionValidation::RepairMode::kFixErrors,
                                  kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.isValid());
            ASSERT_EQ(false, results.getRepaired());
            ASSERT_EQ(static_cast<size_t>(0), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(0), totalNonTransientWarnings(results));
            ASSERT_EQ(static_cast<size_t>(0), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), results.getMissingIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), results.getCorruptRecords().size());
            ASSERT_EQ(0, results.getNumRemovedCorruptRecords());

            dumpOnErrorGuard.dismiss();
        }

        // Confirm repair mode does not silently suppress validation errors.
        {
            ValidateResults results;

            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{CollectionValidation::ValidateMode::kForeground,
                                  CollectionValidation::RepairMode::kNone,
                                  kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.isValid());
            ASSERT_EQ(false, results.getRepaired());
            ASSERT_EQ(static_cast<size_t>(0), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(0), totalNonTransientWarnings(results));
            ASSERT_EQ(static_cast<size_t>(0), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), results.getMissingIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), results.getCorruptRecords().size());
            ASSERT_EQ(0, results.getNumRemovedCorruptRecords());

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
            key_string::HeapBuilder::kHeapAllocatorDefaultBytes);

        // Create a new collection and insert non-multikey document.
        lockDb(MODE_X);

        RecordId id1;
        BSONObj doc = BSON("_id" << 1 << "a" << 1);
        {
            OpDebug* const nullOpDebug = nullptr;
            beginTransaction();
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);

            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(doc), nullOpDebug, true));
            id1 = coll()->getCursor(&_opCtx)->next()->id;
            commitTransaction();
        }

        // Create non-multikey index.
        const auto indexName = "non_mk_index";
        auto status = createIndexFromSpec(BSON("name" << indexName << "key" << BSON("a" << 1) << "v"
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

            // Remove non-multikey index entry.
            {
                beginTransaction();
                KeyStringSet keys;
                iam->getKeys(
                    &_opCtx,
                    coll(),
                    descriptor->getEntry(),
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
                auto removeStatus = iam->removeKeys(&_opCtx,
                                                    descriptor->getEntry(),
                                                    {keys.begin(), keys.end()},
                                                    options,
                                                    &numDeleted);
                ASSERT_OK(removeStatus);
                ASSERT_EQUALS(numDeleted, 1);
                commitTransaction();
            }

            // Update non-multikey document with multikey document.   {a: 1}   ->   {a: [2, 3]}
            BSONObj mkDoc = BSON("_id" << 1 << "a" << BSON_ARRAY(2 << 3));
            {
                beginTransaction();
                auto updateStatus = coll()->getRecordStore()->updateRecord(
                    &_opCtx, id1, mkDoc.objdata(), mkDoc.objsize());
                ASSERT_OK(updateStatus);
                commitTransaction();
            }

            // Insert index entries which satisfy the new multikey document.
            {
                beginTransaction();
                KeyStringSet keys;
                MultikeyPaths multikeyPaths;
                iam->getKeys(
                    &_opCtx,
                    coll(),
                    descriptor->getEntry(),
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
                                                                          descriptor->getEntry(),
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
                                                                     descriptor->getEntry(),
                                                                     {*keysIterator},
                                                                     {},
                                                                     MultikeyPaths{},
                                                                     options,
                                                                     nullptr,
                                                                     &numInserted);
                ASSERT_EQUALS(numInserted, 1);
                ASSERT_OK(insertStatus);
                commitTransaction();
            }
            releaseDb();
        }

        // Confirm multikey document found on non-multikey index error detected by validate.
        {
            ValidateResults results;
            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{CollectionValidation::ValidateMode::kForeground,
                                  CollectionValidation::RepairMode::kNone,
                                  kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(false, results.isValid());
            ASSERT_EQ(false, results.getRepaired());
            ASSERT_EQ(static_cast<size_t>(1), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(0), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), results.getMissingIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), totalNonTransientWarnings(results));

            dumpOnErrorGuard.dismiss();
        }

        // Run validate in repair mode.
        {
            ValidateResults results;

            // Validate in repair mode can only be used in standalone mode, so ignore timestamp
            // assertions.
            shard_role_details::getRecoveryUnit(&_opCtx)->allowAllUntimestampedWrites();
            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{CollectionValidation::ValidateMode::kForeground,
                                  CollectionValidation::RepairMode::kFixErrors,
                                  kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.isValid());
            ASSERT_EQ(true, results.getRepaired());
            ASSERT_EQ(static_cast<size_t>(0), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(0), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), results.getMissingIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(1), totalNonTransientWarnings(results));

            dumpOnErrorGuard.dismiss();
        }

        // Confirm index updated as multikey and multikey paths added such that index does not have
        // to be rebuilt.
        {
            ValidateResults results;

            // Validate in repair mode can only be used in standalone mode, so ignore timestamp
            // assertions.
            shard_role_details::getRecoveryUnit(&_opCtx)->allowAllUntimestampedWrites();
            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{CollectionValidation::ValidateMode::kForeground,
                                  CollectionValidation::RepairMode::kFixErrors,
                                  kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.isValid());
            ASSERT_EQ(false, results.getRepaired());
            ASSERT_EQ(static_cast<size_t>(0), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(0), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), results.getMissingIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), totalNonTransientWarnings(results));

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
            key_string::HeapBuilder::kHeapAllocatorDefaultBytes);

        // Create a new collection and insert multikey document.
        lockDb(MODE_X);

        RecordId id1;
        BSONObj doc1 = BSON("_id" << 1 << "a" << BSON_ARRAY(1 << 2) << "b" << 1);
        {
            OpDebug* const nullOpDebug = nullptr;

            beginTransaction();
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);

            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(doc1), nullOpDebug, true));
            id1 = coll()->getCursor(&_opCtx)->next()->id;
            commitTransaction();
        }

        // Create a multikey index.
        const auto indexName = "mk_index";
        auto status =
            createIndexFromSpec(BSON("name" << indexName << "key" << BSON("a" << 1 << "b" << 1)
                                            << "v" << static_cast<int>(kIndexVersion)));
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

            // Remove index keys for original document.
            MultikeyPaths oldMultikeyPaths;
            {
                beginTransaction();
                KeyStringSet keys;
                iam->getKeys(
                    &_opCtx,
                    coll(),
                    descriptor->getEntry(),
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
                auto removeStatus = iam->removeKeys(&_opCtx,
                                                    descriptor->getEntry(),
                                                    {keys.begin(), keys.end()},
                                                    options,
                                                    &numDeleted);
                ASSERT_OK(removeStatus);
                ASSERT_EQ(numDeleted, 2);
                commitTransaction();
            }

            // Update multikey document with a different multikey documents (not covered by multikey
            // paths).   {a: [1, 2], b: 1}   ->   {a: 1, b: [4, 5]}
            BSONObj doc2 = BSON("_id" << 1 << "a" << 1 << "b" << BSON_ARRAY(4 << 5));
            {
                beginTransaction();
                auto updateStatus = coll()->getRecordStore()->updateRecord(
                    &_opCtx, id1, doc2.objdata(), doc2.objsize());
                ASSERT_OK(updateStatus);
                commitTransaction();
            }

            // We are using the multikeyPaths of the old document and passing them to this insert
            // call (to avoid changing the multikey state).
            {
                beginTransaction();
                KeyStringSet keys;
                iam->getKeys(
                    &_opCtx,
                    coll(),
                    descriptor->getEntry(),
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
                auto insertStatus = iam->insertKeysAndUpdateMultikeyPaths(&_opCtx,
                                                                          coll(),
                                                                          descriptor->getEntry(),
                                                                          keys,
                                                                          {},
                                                                          oldMultikeyPaths,
                                                                          options,
                                                                          nullptr,
                                                                          &numInserted);

                ASSERT_EQUALS(numInserted, 2);
                ASSERT_OK(insertStatus);
                commitTransaction();
            }
            releaseDb();
        }

        // Confirm multikey paths' insufficient coverage of multikey document detected by validate.
        {
            ValidateResults results;

            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{CollectionValidation::ValidateMode::kForeground,
                                  CollectionValidation::RepairMode::kNone,
                                  kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(false, results.isValid());
            ASSERT_EQ(false, results.getRepaired());
            ASSERT_EQ(static_cast<size_t>(1), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(0), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), results.getMissingIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), totalNonTransientWarnings(results));

            dumpOnErrorGuard.dismiss();
        }

        // Run validate in repair mode.
        {
            ValidateResults results;

            // Validate in repair mode can only be used in standalone mode, so ignore timestamp
            // assertions.
            shard_role_details::getRecoveryUnit(&_opCtx)->allowAllUntimestampedWrites();
            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{CollectionValidation::ValidateMode::kForeground,
                                  CollectionValidation::RepairMode::kFixErrors,
                                  kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.isValid());
            ASSERT_EQ(true, results.getRepaired());
            ASSERT_EQ(static_cast<size_t>(0), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(0), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), results.getMissingIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(2), totalNonTransientWarnings(results));

            dumpOnErrorGuard.dismiss();
        }

        // Confirm repair mode does not silently suppress validation errors.
        {
            ValidateResults results;

            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{CollectionValidation::ValidateMode::kForeground,
                                  CollectionValidation::RepairMode::kNone,
                                  kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.isValid());
            ASSERT_EQ(false, results.getRepaired());
            ASSERT_EQ(static_cast<size_t>(0), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(0), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), results.getMissingIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), totalNonTransientWarnings(results));

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
            beginTransaction();
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            _db->createCollection(&_opCtx, _nss);
            commitTransaction();
        }
        CollectionWriter writer(&_opCtx, coll()->ns());

        const auto indexName = "mk_index";
        auto status =
            createIndexFromSpec(BSON("name" << indexName << "key" << BSON("a" << 1 << "b" << 1)
                                            << "v" << static_cast<int>(kIndexVersion)));
        ASSERT_OK(status);

        // Remove the multikeyPaths from the index catalog entry. This simulates the catalog state
        // of a pre-3.4 index.
        {
            beginTransaction();
            auto collMetadata = DurableCatalog::get(&_opCtx)
                                    ->getParsedCatalogEntry(&_opCtx, coll()->getCatalogId())
                                    ->metadata;
            int offset = collMetadata->findIndexOffset(indexName);
            ASSERT_GTE(offset, 0);

            auto& indexMetadata = collMetadata->indexes[offset];
            indexMetadata.multikeyPaths = {};
            writer.getWritableCollection(&_opCtx)->replaceMetadata(&_opCtx,
                                                                   std::move(collMetadata));
            commitTransaction();
        }

        // Reload the index from the modified catalog.
        const IndexDescriptor* descriptor = nullptr;
        {
            beginTransaction();
            auto writableCatalog = writer.getWritableCollection(&_opCtx)->getIndexCatalog();
            descriptor = writableCatalog->findIndexByName(&_opCtx, indexName);
            descriptor = writableCatalog->refreshEntry(&_opCtx,
                                                       writer.getWritableCollection(&_opCtx),
                                                       descriptor,
                                                       CreateIndexEntryFlags::kIsReady);
            commitTransaction();
        }

        // Insert a multikey document. The multikeyPaths should not get updated in this old
        // state.
        RecordId id1;
        BSONObj doc1 = BSON("_id" << 0 << "a" << BSON_ARRAY(1 << 2) << "b" << 1);
        OpDebug* const nullOpDebug = nullptr;
        {
            beginTransaction();
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(doc1), nullOpDebug, true));
            id1 = coll()->getCursor(&_opCtx)->next()->id;
            commitTransaction();
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

            // Validate in repair mode can only be used in standalone mode, so ignore timestamp
            // assertions.
            shard_role_details::getRecoveryUnit(&_opCtx)->allowAllUntimestampedWrites();
            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{CollectionValidation::ValidateMode::kForeground,
                                  CollectionValidation::RepairMode::kAdjustMultikey,
                                  kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.isValid());
            ASSERT_EQ(true, results.getRepaired());
            ASSERT_EQ(static_cast<size_t>(0), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(0), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), results.getMissingIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(1), totalNonTransientWarnings(results));

            dumpOnErrorGuard.dismiss();
        }

        auto expectedPathsAfter = MultikeyPaths{{0}, {}};
        ASSERT(catalogEntry->isMultikey(&_opCtx, coll()));
        ASSERT(catalogEntry->getMultikeyPaths(&_opCtx, coll()) == expectedPathsAfter);

        // Confirm validate does not make changes when run a second time.
        {
            ValidateResults results;

            // Validate in repair mode can only be used in standalone mode, so ignore timestamp
            // assertions.
            shard_role_details::getRecoveryUnit(&_opCtx)->allowAllUntimestampedWrites();
            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{CollectionValidation::ValidateMode::kForeground,
                                  CollectionValidation::RepairMode::kAdjustMultikey,
                                  kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(true, results.isValid());
            ASSERT_EQ(false, results.getRepaired());
            ASSERT_EQ(static_cast<size_t>(0), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(0), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), results.getMissingIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), totalNonTransientWarnings(results));

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
        lockDb(MODE_X);
        ASSERT(coll());

        // Encode an invalid BSON Object with an invalid type, x90 and insert record
        const char* buffer = "\x0c\x00\x00\x00\x90\x41\x00\x10\x00\x00\x00\x00";
        BSONObj obj(buffer);

        RecordStore* rs = coll()->getRecordStore();
        RecordId rid(OID::gen().view().view(), OID::kOIDSize);
        {
            beginTransaction();
            ASSERT_OK(rs->insertRecord(&_opCtx, rid, obj.objdata(), obj.objsize(), timestampToUse));
            commitTransaction();
        }
        releaseDb();

        {
            auto mode = _background ? CollectionValidation::ValidateMode::kBackground
                                    : CollectionValidation::ValidateMode::kForeground;

            ValidateResults results;

            // validate() will set a kProvided read source. Callers continue to do operations
            // after running validate, so we must reset the read source back to normal before
            // returning.
            auto originalReadSource =
                shard_role_details::getRecoveryUnit(&_opCtx)->getTimestampReadSource();
            ON_BLOCK_EXIT([&] {
                shard_role_details::getRecoveryUnit(&_opCtx)->abandonSnapshot();
                shard_role_details::getRecoveryUnit(&_opCtx)->setTimestampReadSource(
                    originalReadSource);
            });
            forceCheckpoint(_background);
            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{mode, CollectionValidation::RepairMode::kNone, kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(false, results.isValid());
            ASSERT_EQ(static_cast<size_t>(1), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(0), totalNonTransientWarnings(results));
            ASSERT_EQ(static_cast<size_t>(0), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), results.getMissingIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(1), results.getCorruptRecords().size());
            ASSERT_EQ(rid, results.getCorruptRecords()[0]);

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
        lockDb(MODE_X);
        ASSERT(coll());

        // Create an index.
        const auto indexName = "a";
        const auto indexKey = BSON("a" << 1);
        auto status = createIndexFromSpec(BSON("name" << indexName << "key" << indexKey << "v"
                                                      << static_cast<int>(kIndexVersion)));
        ASSERT_OK(status);

        // Insert documents.
        OpDebug* const nullOpDebug = nullptr;
        RecordId rid = RecordId::minLong();
        lockDb(MODE_X);

        const OID firstRecordId = OID::gen();
        {
            beginTransaction();
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
            commitTransaction();
        }
        releaseDb();
        ensureValidateWorked();
        lockDb(MODE_X);

        RecordStore* rs = coll()->getRecordStore();

        // Updating a document without updating the index entry will cause validation to detect a
        // missing index entry and an extra index entry.
        {
            beginTransaction();
            auto doc = BSON("_id" << firstRecordId << "a" << 5);
            auto updateStatus = rs->updateRecord(&_opCtx, rid, doc.objdata(), doc.objsize());
            ASSERT_OK(updateStatus);
            commitTransaction();
        }
        releaseDb();

        {
            auto mode = _background ? CollectionValidation::ValidateMode::kBackground
                                    : CollectionValidation::ValidateMode::kForeground;

            ValidateResults results;

            // validate() will set a kProvided read source. Callers continue to do operations
            // after running validate, so we must reset the read source back to normal before
            // returning.
            auto originalReadSource =
                shard_role_details::getRecoveryUnit(&_opCtx)->getTimestampReadSource();
            ON_BLOCK_EXIT([&] {
                shard_role_details::getRecoveryUnit(&_opCtx)->abandonSnapshot();
                shard_role_details::getRecoveryUnit(&_opCtx)->setTimestampReadSource(
                    originalReadSource);
            });
            forceCheckpoint(_background);
            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{mode, CollectionValidation::RepairMode::kNone, kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(false, results.isValid());
            ASSERT_EQ(static_cast<size_t>(1), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(2), totalNonTransientWarnings(results));
            ASSERT_EQ(static_cast<size_t>(1), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(1), results.getMissingIndexEntries().size());

            dumpOnErrorGuard.dismiss();
        }
    }
};

/**
 * Validate detects duplicate keys in a secondary unique index {a: 1} when the index is
 * on a clustered collection.
 * Two cases are tested:
 * 1. The false negative case: when validate says there isn't a uniqueness
 * violation even though there is one.
 * 2. The false positive case: when validate says there is a uniqueness
 * violation even though there isn't one.
 *
 * False negative case:
 * Suppose we have two documents {_id: "1000000000", a: 1} and {_id: "1000000000", a: 1}
 * that live in a collection. Since they have the same value for field 'a', they violate
 * the uniqueness constraint of the index.
 * The key strings for index {a: 1} for the two docs look something like this.
 * They map from the value of 'a' in the document to the recordId.
 * Buffer for keystring1: 1,1000000000
 * Buffer for keystring2: 1,2000000000
 *
 * When we compareWithoutRecordIdLong(), we chop off only the number of
 * bytes used in a long before making the comparison in the buffer. Since a long
 * is 8 bytes, we cut 8 characters off.
 * Truncated buffer 1: 1,10
 * Truncated buffer 2: 1,20
 *
 * And we can see that the two truncated buffers above still aren't equal. But instead,
 * if we used compareWithoutRecordIdStr(), we first figure out how many bytes we need
 * to chop to exclude the recordId, and that way only the index entry value is compared.
 * Now the unique index violation can be detected, as both the truncated buffers are
 * equal.
 * Truncated buffer 1: 1
 * Truncated buffer 2: 1
 *
 * False positive case:
 * Suppose we have two documents {_id: "1", a: 10000001} and {_id: "2", a: 10000002}.
 * Clearly they don't violate any constraints. However it is possible, if we truncate
 * more bytes than necessary, that we will end up truncating some of the bytes of the
 * field 'a'. For example,
 * Pre-truncation:
 * Buffer for keystring1: 10000001,1
 * Buffer for keystring2: 10000002,2
 * Post-truncation:
 * Buffer for keystring1: 10000
 * Buffer for keystring2: 10000
 * This can lead to a false positive uniqueness violation.
 */
template <bool falsePositiveCase>
class ValidateDuplicateKeyOnClusteredCollection : public ValidateBase {
public:
    ValidateDuplicateKeyOnClusteredCollection()
        : ValidateBase(/*full=*/true, /*background=*/false, /*clustered=*/true) {}

    void run() {
        SharedBufferFragmentBuilder pooledBuilder(
            key_string::HeapBuilder::kHeapAllocatorDefaultBytes);

        lockDb(MODE_X);
        ASSERT(coll());

        // Create a unique index on {a: 1}
        const auto indexName = "a";
        const auto indexKey = BSON("a" << 1);
        auto status = createIndexFromSpec(BSON("name" << indexName << "key" << indexKey << "v"
                                                      << static_cast<int>(kIndexVersion) << "unique"
                                                      << true));
        ASSERT_OK(status);


        // Insert documents.
        auto firstDoc = BSON("_id"
                             << "1000000000000"
                             << "a" << 1);
        auto secondDoc = BSON("_id"
                              << "2000000000000"
                              << "a" << 1);
        if (falsePositiveCase) {
            firstDoc = BSON("_id"
                            << "1"
                            << "a" << 10000001);
            secondDoc = BSON("_id"
                             << "2"
                             << "a" << 10000002);
        }
        OpDebug* const nullOpDebug = nullptr;
        lockDb(MODE_X);
        {
            beginTransaction();
            ASSERT_OK(collection_internal::insertDocument(
                &_opCtx, coll(), InsertStatement(firstDoc), nullOpDebug, true));
            if (falsePositiveCase) {
                ASSERT_OK(collection_internal::insertDocument(
                    &_opCtx, coll(), InsertStatement(secondDoc), nullOpDebug, true));
            }
            commitTransaction();
        }
        releaseDb();
        ensureValidateWorked();

        // Insert a document with a duplicate key for "a".
        if (!falsePositiveCase) {
            lockDb(MODE_X);

            const IndexCatalog* indexCatalog = coll()->getIndexCatalog();

            InsertDeleteOptions options;
            options.dupsAllowed = true;

            beginTransaction();

            // Insert a record and its keys separately. We do this to bypass duplicate constraint
            // checking. Inserting a record and all of its keys ensures that validation fails
            // because there are duplicate keys, and not just because there are keys without
            // corresponding records.
            auto swRecordId =
                coll()->getRecordStore()->insertRecord(&_opCtx,
                                                       record_id_helpers::keyForObj(secondDoc),
                                                       secondDoc.objdata(),
                                                       secondDoc.objsize(),
                                                       timestampToUse);
            ASSERT_OK(swRecordId);
            commitTransaction();

            // Insert the key on "a".
            {
                auto descriptor = indexCatalog->findIndexByName(&_opCtx, indexName);
                auto entry = const_cast<IndexCatalogEntry*>(indexCatalog->getEntry(descriptor));
                auto iam = entry->accessMethod()->asSortedData();
                auto interceptor = std::make_unique<IndexBuildInterceptor>(&_opCtx, entry);

                KeyStringSet keys;
                iam->getKeys(&_opCtx,
                             coll(),
                             entry,
                             pooledBuilder,
                             secondDoc,
                             InsertDeleteOptions::ConstraintEnforcementMode::kRelaxConstraints,
                             SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                             &keys,
                             nullptr,
                             nullptr,
                             swRecordId.getValue());
                ASSERT_EQ(1, keys.size());

                {
                    beginTransaction();

                    int64_t numInserted;
                    auto insertStatus = iam->insertKeysAndUpdateMultikeyPaths(
                        &_opCtx,
                        coll(),
                        entry,
                        {keys.begin(), keys.end()},
                        {},
                        MultikeyPaths{},
                        options,
                        [this, &entry, &interceptor](const key_string::Value& duplicateKey) {
                            return interceptor->recordDuplicateKey(&_opCtx, entry, duplicateKey);
                        },
                        &numInserted);

                    ASSERT_EQUALS(numInserted, 1);
                    ASSERT_OK(insertStatus);

                    commitTransaction();
                }

                ASSERT_NOT_OK(interceptor->checkDuplicateKeyConstraints(&_opCtx, coll(), entry));
            }

            releaseDb();
        }

        ValidateResults results = runValidate();

        ScopeGuard dumpOnErrorGuard([&] {
            StorageDebugUtil::printValidateResults(results);
            StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
        });

        if (falsePositiveCase) {
            ASSERT(results.isValid()) << "Validation failed when it should have worked.";
            ASSERT_EQ(static_cast<size_t>(0), totalErrors(results));
        } else {
            ASSERT_FALSE(results.isValid()) << "Validation worked when it should have failed.";
            ASSERT_EQ(static_cast<size_t>(1), totalErrors(results));
        }
        ASSERT_EQ(static_cast<size_t>(0), totalNonTransientWarnings(results));
        ASSERT_EQ(static_cast<size_t>(0), results.getExtraIndexEntries().size());
        ASSERT_EQ(static_cast<size_t>(0), results.getMissingIndexEntries().size());

        dumpOnErrorGuard.dismiss();
    }
};

class ValidateRepairOnClusteredCollection : public ValidateBase {
public:
    ValidateRepairOnClusteredCollection()
        : ValidateBase(/*full=*/false, /*background=*/false, /*clustered=*/true) {}

    void run() {
        lockDb(MODE_X);
        ASSERT(coll());

        // Create an index.
        const auto indexName = "a";
        const auto indexKey = BSON("a" << 1);
        auto status = createIndexFromSpec(BSON("name" << indexName << "key" << indexKey << "v"
                                                      << static_cast<int>(kIndexVersion)));
        ASSERT_OK(status);

        // Insert documents.
        OpDebug* const nullOpDebug = nullptr;
        RecordId rid = RecordId::minLong();
        lockDb(MODE_X);

        const OID firstRecordId = OID::gen();
        {
            beginTransaction();
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
            commitTransaction();
        }
        releaseDb();
        ensureValidateWorked();
        lockDb(MODE_X);

        RecordStore* rs = coll()->getRecordStore();

        // Updating a document without updating the index entry will cause validation to detect a
        // missing index entry and an extra index entry.
        {
            beginTransaction();
            auto doc = BSON("_id" << firstRecordId << "a" << 5);
            auto updateStatus = rs->updateRecord(&_opCtx, rid, doc.objdata(), doc.objsize());
            ASSERT_OK(updateStatus);
            commitTransaction();
        }
        releaseDb();

        {
            auto mode = _background ? CollectionValidation::ValidateMode::kBackground
                                    : CollectionValidation::ValidateMode::kForeground;

            ValidateResults results;

            // validate() will set a kProvided read source. Callers continue to do operations
            // after running validate, so we must reset the read source back to normal before
            // returning.
            auto originalReadSource =
                shard_role_details::getRecoveryUnit(&_opCtx)->getTimestampReadSource();
            ON_BLOCK_EXIT([&] {
                shard_role_details::getRecoveryUnit(&_opCtx)->abandonSnapshot();
                shard_role_details::getRecoveryUnit(&_opCtx)->setTimestampReadSource(
                    originalReadSource);
            });
            forceCheckpoint(_background);
            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{mode, CollectionValidation::RepairMode::kNone, kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(false, results.isValid());
            ASSERT_EQ(static_cast<size_t>(1), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(2), totalNonTransientWarnings(results));
            ASSERT_EQ(static_cast<size_t>(1), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(1), results.getMissingIndexEntries().size());

            dumpOnErrorGuard.dismiss();
        }

        // Run validate with repair, expect that extra index entries are removed and missing index
        // entries are inserted.
        {
            auto mode = _background ? CollectionValidation::ValidateMode::kBackground
                                    : CollectionValidation::ValidateMode::kForeground;

            ValidateResults results;

            // validate() will set a kProvided read source. Callers continue to do operations
            // after running validate, so we must reset the read source back to normal before
            // returning.
            auto originalReadSource =
                shard_role_details::getRecoveryUnit(&_opCtx)->getTimestampReadSource();
            ON_BLOCK_EXIT([&] {
                shard_role_details::getRecoveryUnit(&_opCtx)->abandonSnapshot();
                shard_role_details::getRecoveryUnit(&_opCtx)->setTimestampReadSource(
                    originalReadSource);
            });
            forceCheckpoint(_background);
            // Validate in repair mode can only be used in standalone mode, so ignore timestamp
            // assertions.
            shard_role_details::getRecoveryUnit(&_opCtx)->allowAllUntimestampedWrites();
            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{
                    mode, CollectionValidation::RepairMode::kFixErrors, kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });


            ASSERT_EQ(true, results.isValid());
            ASSERT_EQ(true, results.getRepaired());
            ASSERT_EQ(static_cast<size_t>(0), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(2), totalNonTransientWarnings(results));
            ASSERT_EQ(static_cast<size_t>(0), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), results.getMissingIndexEntries().size());
            ASSERT_EQ(1, results.getNumRemovedExtraIndexEntries());
            ASSERT_EQ(1, results.getNumInsertedMissingIndexEntries());

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
        lockDb(MODE_X);
        ASSERT(coll());

        if (_withSecondaryIndex) {
            // Create index on {a: 1}
            const auto indexName = "a";
            const auto indexKey = BSON("a" << 1);
            auto status = createIndexFromSpec(BSON("name" << indexName << "key" << indexKey << "v"
                                                          << static_cast<int>(kIndexVersion)));
            ASSERT_OK(status);
        }

        // Insert documents
        OpDebug* const nullOpDebug = nullptr;
        lockDb(MODE_X);

        const OID firstRecordId = OID::gen();
        {
            beginTransaction();
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
            commitTransaction();
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
            beginTransaction();
            auto doc = BSON("a" << 1);
            auto updateStatus =
                rs->updateRecord(&_opCtx, ridMissingId, doc.objdata(), doc.objsize());
            ASSERT_OK(updateStatus);
            commitTransaction();
        }
        const auto ridMismatchedId = cursor->next()->id;
        {
            beginTransaction();
            auto doc = BSON("_id" << OID::gen() << "a" << 2);
            auto updateStatus =
                rs->updateRecord(&_opCtx, ridMismatchedId, doc.objdata(), doc.objsize());
            ASSERT_OK(updateStatus);
            commitTransaction();
        }
        cursor.reset();

        releaseDb();

        // Verify that validate() detects the two corrupt records.
        {
            ValidateResults results;

            ASSERT_OK(CollectionValidation::validate(
                &_opCtx,
                _nss,
                ValidationOptions{CollectionValidation::ValidateMode::kForegroundFull,
                                  CollectionValidation::RepairMode::kNone,
                                  kLogDiagnostics},
                &results));

            ScopeGuard dumpOnErrorGuard([&] {
                StorageDebugUtil::printValidateResults(results);
                StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, coll()->ns());
            });

            ASSERT_EQ(false, results.isValid());
            ASSERT_EQ(static_cast<size_t>(2), totalErrors(results));
            ASSERT_EQ(static_cast<size_t>(0), totalNonTransientWarnings(results));
            ASSERT_EQ(static_cast<size_t>(0), results.getExtraIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(0), results.getMissingIndexEntries().size());
            ASSERT_EQ(static_cast<size_t>(2), results.getCorruptRecords().size());
            ASSERT_EQ(ridMissingId, results.getCorruptRecords()[0]);
            ASSERT_EQ(ridMismatchedId, results.getCorruptRecords()[1]);

            dumpOnErrorGuard.dismiss();
        }
    }

private:
    const bool _withSecondaryIndex;
};

class ValidateTests : public unittest::OldStyleSuiteSpecification {
public:
    ValidateTests() : OldStyleSuiteSpecification("validate_tests") {}

    void setupTests() override {
        add<ValidateDuplicateKeyOnClusteredCollection<true /*falsePositiveCase*/>>();
        add<ValidateDuplicateKeyOnClusteredCollection<false /*falsePositiveCase*/>>();

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

unittest::OldStyleSuiteInitializer<ValidateTests> validateTests;

}  // namespace ValidateTests
}  // namespace mongo
