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
#include "mongo/db/catalog/collection_validation.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/service_context.h"
#include "mongo/dbtests/dbtests.h"

namespace ValidateTests {

using std::unique_ptr;

namespace {
const auto kIndexVersion = IndexDescriptor::IndexVersion::kV2;
}  // namespace

static const char* const _ns = "unittests.validate_tests";

/**
 * Test fixture for a write locked test using collection _ns.  Includes functionality to
 * partially construct a new IndexDetails in a manner that supports proper cleanup in
 * dropCollection().
 */
class ValidateBase {
public:
    explicit ValidateBase(bool full, bool background)
        : _client(&_opCtx),
          _full(full),
          _background(background),
          _nss(_ns),
          _autoDb(nullptr),
          _db(nullptr) {
        _client.createCollection(_ns);
        {
            AutoGetCollection autoGetCollection(&_opCtx, _nss, MODE_X);
            _isInRecordIdOrder =
                autoGetCollection.getCollection()->getRecordStore()->isInRecordIdOrder();
        }
    }

    ~ValidateBase() {
        _client.dropCollection(_ns);
        getGlobalServiceContext()->unsetKillAllOperations();
    }

protected:
    bool checkValid() {
        ValidateResults results;
        BSONObjBuilder output;

        lockDb(MODE_IX);
        invariant(_opCtx.lockState()->isDbLockedForMode(_nss.db(), MODE_IX));
        std::unique_ptr<Lock::CollectionLock> lock =
            std::make_unique<Lock::CollectionLock>(&_opCtx, _nss, MODE_X);
        invariant(_opCtx.lockState()->isCollectionLockedForMode(_nss, MODE_X));

        Database* db = _autoDb.get()->getDb();
        ASSERT_OK(CollectionValidation::validate(&_opCtx,
                                                 db->getCollection(&_opCtx, _nss),
                                                 _full ? kValidateFull : kValidateNormal,
                                                 _background,
                                                 &results,
                                                 &output));

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

        return results.valid;
    }

    void lockDb(LockMode mode) {
        _autoDb.reset();
        invariant(_opCtx.lockState()->isDbLockedForMode(_nss.db(), MODE_NONE));
        _autoDb.reset(new AutoGetDb(&_opCtx, _nss.db().toString(), mode));
        invariant(_opCtx.lockState()->isDbLockedForMode(_nss.db(), mode));
        _db = _autoDb.get()->getDb();
    }

    void releaseDb() {
        _autoDb.reset();
        _db = nullptr;
        invariant(_opCtx.lockState()->isDbLockedForMode(_nss.db(), MODE_NONE));
    }

    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_txnPtr;
    DBDirectClient _client;
    bool _full;
    bool _background;
    const NamespaceString _nss;
    unique_ptr<AutoGetDb> _autoDb;
    Database* _db;
    bool _isInRecordIdOrder;
};

template <bool full, bool background>
class ValidateIdIndexCount : public ValidateBase {
public:
    ValidateIdIndexCount() : ValidateBase(full, background) {}

    void run() {

        // Can't do it in background if the RecordStore is not in RecordId order.
        if (_background && !_isInRecordIdOrder) {
            return;
        }

        // Create a new collection, insert records {_id: 1} and {_id: 2} and check it's valid.
        lockDb(MODE_X);
        Collection* coll;
        RecordId id1;
        {
            OpDebug* const nullOpDebug = nullptr;
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            coll = _db->createCollection(&_opCtx, _nss);

            ASSERT_OK(coll->insertDocument(
                &_opCtx, InsertStatement(BSON("_id" << 1)), nullOpDebug, true));
            id1 = coll->getCursor(&_opCtx)->next()->id;
            ASSERT_OK(coll->insertDocument(
                &_opCtx, InsertStatement(BSON("_id" << 2)), nullOpDebug, true));
            wunit.commit();
        }

        ASSERT_TRUE(checkValid());

        lockDb(MODE_X);
        RecordStore* rs = coll->getRecordStore();

        // Remove {_id: 1} from the record store, so we get more _id entries than records.
        {
            WriteUnitOfWork wunit(&_opCtx);
            rs->deleteRecord(&_opCtx, id1);
            wunit.commit();
        }

        ASSERT_FALSE(checkValid());

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

        ASSERT_FALSE(checkValid());
        releaseDb();
    }
};

template <bool full, bool background>
class ValidateSecondaryIndexCount : public ValidateBase {
public:
    ValidateSecondaryIndexCount() : ValidateBase(full, background) {}
    void run() {

        // Can't do it in background if the RecordStore is not in RecordId order.
        if (_background && !_isInRecordIdOrder) {
            return;
        }

        // Create a new collection, insert two documents.
        lockDb(MODE_X);
        Collection* coll;
        RecordId id1;
        {
            OpDebug* const nullOpDebug = nullptr;
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            coll = _db->createCollection(&_opCtx, _nss);
            ASSERT_OK(coll->insertDocument(
                &_opCtx, InsertStatement(BSON("_id" << 1 << "a" << 1)), nullOpDebug, true));
            id1 = coll->getCursor(&_opCtx)->next()->id;
            ASSERT_OK(coll->insertDocument(
                &_opCtx, InsertStatement(BSON("_id" << 2 << "a" << 2)), nullOpDebug, true));
            wunit.commit();
        }

        auto status = dbtests::createIndexFromSpec(&_opCtx,
                                                   coll->ns().ns(),
                                                   BSON("name"
                                                        << "a"
                                                        << "key" << BSON("a" << 1) << "v"
                                                        << static_cast<int>(kIndexVersion)
                                                        << "background" << false));

        ASSERT_OK(status);
        ASSERT_TRUE(checkValid());

        lockDb(MODE_X);
        RecordStore* rs = coll->getRecordStore();

        // Remove a record, so we get more _id entries than records, and verify validate fails.
        {
            WriteUnitOfWork wunit(&_opCtx);
            rs->deleteRecord(&_opCtx, id1);
            wunit.commit();
        }

        ASSERT_FALSE(checkValid());

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

        ASSERT_FALSE(checkValid());
        releaseDb();
    }
};

template <bool full, bool background>
class ValidateSecondaryIndex : public ValidateBase {
public:
    ValidateSecondaryIndex() : ValidateBase(full, background) {}
    void run() {

        // Can't do it in background if the RecordStore is not in RecordId order.
        if (_background && !_isInRecordIdOrder) {
            return;
        }

        // Create a new collection, insert three records.
        lockDb(MODE_X);
        OpDebug* const nullOpDebug = nullptr;
        Collection* coll;
        RecordId id1;
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            coll = _db->createCollection(&_opCtx, _nss);
            ASSERT_OK(coll->insertDocument(
                &_opCtx, InsertStatement(BSON("_id" << 1 << "a" << 1)), nullOpDebug, true));
            id1 = coll->getCursor(&_opCtx)->next()->id;
            ASSERT_OK(coll->insertDocument(
                &_opCtx, InsertStatement(BSON("_id" << 2 << "a" << 2)), nullOpDebug, true));
            ASSERT_OK(coll->insertDocument(
                &_opCtx, InsertStatement(BSON("_id" << 3 << "b" << 3)), nullOpDebug, true));
            wunit.commit();
        }

        auto status = dbtests::createIndexFromSpec(&_opCtx,
                                                   coll->ns().ns(),
                                                   BSON("name"
                                                        << "a"
                                                        << "key" << BSON("a" << 1) << "v"
                                                        << static_cast<int>(kIndexVersion)
                                                        << "background" << false));

        ASSERT_OK(status);
        ASSERT_TRUE(checkValid());

        lockDb(MODE_X);
        RecordStore* rs = coll->getRecordStore();

        // Update {a: 1} to {a: 9} without updating the index, so we get inconsistent values
        // between the index and the document. Verify validate fails.
        {
            WriteUnitOfWork wunit(&_opCtx);
            auto doc = BSON("_id" << 1 << "a" << 9);
            auto updateStatus = rs->updateRecord(&_opCtx, id1, doc.objdata(), doc.objsize());

            ASSERT_OK(updateStatus);
            wunit.commit();
        }

        ASSERT_FALSE(checkValid());
        releaseDb();
    }
};

template <bool full, bool background>
class ValidateIdIndex : public ValidateBase {
public:
    ValidateIdIndex() : ValidateBase(full, background) {}

    void run() {

        // Can't do it in background if the RecordStore is not in RecordId order.
        if (_background && !_isInRecordIdOrder) {
            return;
        }

        // Create a new collection, insert records {_id: 1} and {_id: 2} and check it's valid.
        lockDb(MODE_X);
        OpDebug* const nullOpDebug = nullptr;
        Collection* coll;
        RecordId id1;
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            coll = _db->createCollection(&_opCtx, _nss);

            ASSERT_OK(coll->insertDocument(
                &_opCtx, InsertStatement(BSON("_id" << 1)), nullOpDebug, true));
            id1 = coll->getCursor(&_opCtx)->next()->id;
            ASSERT_OK(coll->insertDocument(
                &_opCtx, InsertStatement(BSON("_id" << 2)), nullOpDebug, true));
            wunit.commit();
        }

        ASSERT_TRUE(checkValid());

        lockDb(MODE_X);
        RecordStore* rs = coll->getRecordStore();

        // Update {_id: 1} to {_id: 9} without updating the index, so we get inconsistent values
        // between the index and the document. Verify validate fails.
        {
            WriteUnitOfWork wunit(&_opCtx);
            auto doc = BSON("_id" << 9);
            auto updateStatus = rs->updateRecord(&_opCtx, id1, doc.objdata(), doc.objsize());
            ASSERT_OK(updateStatus);
            wunit.commit();
        }

        ASSERT_FALSE(checkValid());

        lockDb(MODE_X);

        // Revert {_id: 9} to {_id: 1} and verify that validate succeeds.
        {
            WriteUnitOfWork wunit(&_opCtx);
            auto doc = BSON("_id" << 1);
            auto updateStatus = rs->updateRecord(&_opCtx, id1, doc.objdata(), doc.objsize());
            ASSERT_OK(updateStatus);
            wunit.commit();
        }

        ASSERT_TRUE(checkValid());

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

        ASSERT_FALSE(checkValid());
        releaseDb();
    }
};

template <bool full, bool background>
class ValidateMultiKeyIndex : public ValidateBase {
public:
    ValidateMultiKeyIndex() : ValidateBase(full, background) {}

    void run() {

        // Can't do it in background if the RecordStore is not in RecordId order.
        if (_background && !_isInRecordIdOrder) {
            return;
        }

        // Create a new collection, insert three records and check it's valid.
        lockDb(MODE_X);
        OpDebug* const nullOpDebug = nullptr;
        Collection* coll;
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
            coll = _db->createCollection(&_opCtx, _nss);


            ASSERT_OK(coll->insertDocument(&_opCtx, InsertStatement(doc1), nullOpDebug, true));
            id1 = coll->getCursor(&_opCtx)->next()->id;
            ASSERT_OK(coll->insertDocument(&_opCtx, InsertStatement(doc2), nullOpDebug, true));
            ASSERT_OK(coll->insertDocument(&_opCtx, InsertStatement(doc3), nullOpDebug, true));
            wunit.commit();
        }

        ASSERT_TRUE(checkValid());

        lockDb(MODE_X);

        // Create multi-key index.
        auto status = dbtests::createIndexFromSpec(&_opCtx,
                                                   coll->ns().ns(),
                                                   BSON("name"
                                                        << "multikey_index"
                                                        << "key" << BSON("a.b" << 1) << "v"
                                                        << static_cast<int>(kIndexVersion)
                                                        << "background" << false));

        ASSERT_OK(status);
        ASSERT_TRUE(checkValid());

        lockDb(MODE_X);
        RecordStore* rs = coll->getRecordStore();

        // Update a document's indexed field without updating the index.
        {
            WriteUnitOfWork wunit(&_opCtx);
            auto updateStatus = rs->updateRecord(&_opCtx, id1, doc1_b.objdata(), doc1_b.objsize());
            ASSERT_OK(updateStatus);
            wunit.commit();
        }

        ASSERT_FALSE(checkValid());

        lockDb(MODE_X);

        // Update a document's non-indexed field without updating the index.
        // Index validation should still be valid.
        {
            WriteUnitOfWork wunit(&_opCtx);
            auto updateStatus = rs->updateRecord(&_opCtx, id1, doc1_c.objdata(), doc1_c.objsize());
            ASSERT_OK(updateStatus);
            wunit.commit();
        }

        ASSERT_TRUE(checkValid());
        releaseDb();
    }
};

template <bool full, bool background>
class ValidateSparseIndex : public ValidateBase {
public:
    ValidateSparseIndex() : ValidateBase(full, background) {}

    void run() {

        // Can't do it in background if the RecordStore is not in RecordId order.
        if (_background && !_isInRecordIdOrder) {
            return;
        }

        // Create a new collection, insert three records and check it's valid.
        lockDb(MODE_X);
        OpDebug* const nullOpDebug = nullptr;
        Collection* coll;
        RecordId id1;
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            coll = _db->createCollection(&_opCtx, _nss);

            ASSERT_OK(coll->insertDocument(
                &_opCtx, InsertStatement(BSON("_id" << 1 << "a" << 1)), nullOpDebug, true));
            id1 = coll->getCursor(&_opCtx)->next()->id;
            ASSERT_OK(coll->insertDocument(
                &_opCtx, InsertStatement(BSON("_id" << 2 << "a" << 2)), nullOpDebug, true));
            ASSERT_OK(coll->insertDocument(
                &_opCtx, InsertStatement(BSON("_id" << 3 << "b" << 1)), nullOpDebug, true));
            wunit.commit();
        }

        // Create a sparse index.
        auto status =
            dbtests::createIndexFromSpec(&_opCtx,
                                         coll->ns().ns(),
                                         BSON("name"
                                              << "sparse_index"
                                              << "key" << BSON("a" << 1) << "v"
                                              << static_cast<int>(kIndexVersion) << "background"
                                              << false << "sparse" << true));

        ASSERT_OK(status);
        ASSERT_TRUE(checkValid());

        lockDb(MODE_X);
        RecordStore* rs = coll->getRecordStore();

        // Update a document's indexed field without updating the index.
        {
            WriteUnitOfWork wunit(&_opCtx);
            auto doc = BSON("_id" << 2 << "a" << 3);
            auto updateStatus = rs->updateRecord(&_opCtx, id1, doc.objdata(), doc.objsize());
            ASSERT_OK(updateStatus);
            wunit.commit();
        }

        ASSERT_FALSE(checkValid());
        releaseDb();
    }
};

template <bool full, bool background>
class ValidatePartialIndex : public ValidateBase {
public:
    ValidatePartialIndex() : ValidateBase(full, background) {}

    void run() {

        // Can't do it in background if the RecordStore is not in RecordId order.
        if (_background && !_isInRecordIdOrder) {
            return;
        }

        // Create a new collection, insert three records and check it's valid.
        lockDb(MODE_X);
        OpDebug* const nullOpDebug = nullptr;
        Collection* coll;
        RecordId id1;
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            coll = _db->createCollection(&_opCtx, _nss);

            ASSERT_OK(coll->insertDocument(
                &_opCtx, InsertStatement(BSON("_id" << 1 << "a" << 1)), nullOpDebug, true));
            id1 = coll->getCursor(&_opCtx)->next()->id;
            ASSERT_OK(coll->insertDocument(
                &_opCtx, InsertStatement(BSON("_id" << 2 << "a" << 2)), nullOpDebug, true));
            // Explicitly test that multi-key partial indexes containing documents that
            // don't match the filter expression are handled correctly.
            ASSERT_OK(coll->insertDocument(
                &_opCtx,
                InsertStatement(BSON("_id" << 3 << "a" << BSON_ARRAY(-1 << -2 << -3))),
                nullOpDebug,
                true));
            wunit.commit();
        }

        // Create a partial index.
        auto status =
            dbtests::createIndexFromSpec(&_opCtx,
                                         coll->ns().ns(),
                                         BSON("name"
                                              << "partial_index"
                                              << "key" << BSON("a" << 1) << "v"
                                              << static_cast<int>(kIndexVersion) << "background"
                                              << false << "partialFilterExpression"
                                              << BSON("a" << BSON("$gt" << 1))));

        ASSERT_OK(status);
        ASSERT_TRUE(checkValid());

        lockDb(MODE_X);
        RecordStore* rs = coll->getRecordStore();

        // Update an unindexed document without updating the index.
        {
            WriteUnitOfWork wunit(&_opCtx);
            auto doc = BSON("_id" << 1);
            auto updateStatus = rs->updateRecord(&_opCtx, id1, doc.objdata(), doc.objsize());
            ASSERT_OK(updateStatus);
            wunit.commit();
        }

        ASSERT_TRUE(checkValid());
        releaseDb();
    }
};

template <bool full, bool background>
class ValidatePartialIndexOnCollectionWithNonIndexableFields : public ValidateBase {
public:
    ValidatePartialIndexOnCollectionWithNonIndexableFields() : ValidateBase(full, background) {}

    void run() {

        // Can't do it in background if the RecordStore is not in RecordId order.
        if (_background && !_isInRecordIdOrder) {
            return;
        }

        // Create a new collection and insert a record that has a non-indexable value on the indexed
        // field.
        lockDb(MODE_X);
        OpDebug* const nullOpDebug = nullptr;
        Collection* coll;
        RecordId id1;
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            coll = _db->createCollection(&_opCtx, _nss);
            ASSERT_OK(
                coll->insertDocument(&_opCtx,
                                     InsertStatement(BSON("_id" << 1 << "x" << 1 << "a" << 2)),
                                     nullOpDebug,
                                     true));
            wunit.commit();
        }

        // Create a partial geo index that indexes the document. This should return an error.
        ASSERT_NOT_OK(
            dbtests::createIndexFromSpec(&_opCtx,
                                         coll->ns().ns(),
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
                                         coll->ns().ns(),
                                         BSON("name"
                                              << "partial_index"
                                              << "key"
                                              << BSON("x"
                                                      << "2dsphere")
                                              << "v" << static_cast<int>(kIndexVersion)
                                              << "background" << false << "partialFilterExpression"
                                              << BSON("a" << BSON("$eq" << 1))));
        ASSERT_OK(status);
        ASSERT_TRUE(checkValid());
        releaseDb();
    }
};

template <bool full, bool background>
class ValidateCompoundIndex : public ValidateBase {
public:
    ValidateCompoundIndex() : ValidateBase(full, background) {}

    void run() {

        // Can't do it in background if the RecordStore is not in RecordId order.
        if (_background && !_isInRecordIdOrder) {
            return;
        }

        // Create a new collection, insert five records and check it's valid.
        lockDb(MODE_X);
        OpDebug* const nullOpDebug = nullptr;
        Collection* coll;
        RecordId id1;
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            coll = _db->createCollection(&_opCtx, _nss);

            ASSERT_OK(
                coll->insertDocument(&_opCtx,
                                     InsertStatement(BSON("_id" << 1 << "a" << 1 << "b" << 4)),
                                     nullOpDebug,
                                     true));
            id1 = coll->getCursor(&_opCtx)->next()->id;
            ASSERT_OK(
                coll->insertDocument(&_opCtx,
                                     InsertStatement(BSON("_id" << 2 << "a" << 2 << "b" << 5)),
                                     nullOpDebug,
                                     true));
            ASSERT_OK(coll->insertDocument(
                &_opCtx, InsertStatement(BSON("_id" << 3 << "a" << 3)), nullOpDebug, true));
            ASSERT_OK(coll->insertDocument(
                &_opCtx, InsertStatement(BSON("_id" << 4 << "b" << 6)), nullOpDebug, true));
            ASSERT_OK(coll->insertDocument(
                &_opCtx, InsertStatement(BSON("_id" << 5 << "c" << 7)), nullOpDebug, true));
            wunit.commit();
        }

        // Create two compound indexes, one forward and one reverse, to test
        // validate()'s index direction parsing.
        auto status = dbtests::createIndexFromSpec(&_opCtx,
                                                   coll->ns().ns(),
                                                   BSON("name"
                                                        << "compound_index_1"
                                                        << "key" << BSON("a" << 1 << "b" << -1)
                                                        << "v" << static_cast<int>(kIndexVersion)
                                                        << "background" << false));
        ASSERT_OK(status);

        status = dbtests::createIndexFromSpec(&_opCtx,
                                              coll->ns().ns(),
                                              BSON("name"
                                                   << "compound_index_2"
                                                   << "key" << BSON("a" << -1 << "b" << 1) << "v"
                                                   << static_cast<int>(kIndexVersion)
                                                   << "background" << false));

        ASSERT_OK(status);
        ASSERT_TRUE(checkValid());

        lockDb(MODE_X);
        RecordStore* rs = coll->getRecordStore();

        // Update a document's indexed field without updating the index.
        {
            WriteUnitOfWork wunit(&_opCtx);
            auto doc = BSON("_id" << 1 << "a" << 1 << "b" << 3);
            auto updateStatus = rs->updateRecord(&_opCtx, id1, doc.objdata(), doc.objsize());
            ASSERT_OK(updateStatus);
            wunit.commit();
        }

        ASSERT_FALSE(checkValid());
        releaseDb();
    }
};

template <bool full, bool background>
class ValidateIndexEntry : public ValidateBase {
public:
    ValidateIndexEntry() : ValidateBase(full, background) {}

    void run() {

        // Can't do it in background if the RecordStore is not in RecordId order.
        if (_background && !_isInRecordIdOrder) {
            return;
        }

        // Create a new collection, insert three records and check it's valid.
        lockDb(MODE_X);
        OpDebug* const nullOpDebug = nullptr;
        Collection* coll;
        RecordId id1;
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            coll = _db->createCollection(&_opCtx, _nss);

            ASSERT_OK(coll->insertDocument(
                &_opCtx, InsertStatement(BSON("_id" << 1 << "a" << 1)), nullOpDebug, true));
            id1 = coll->getCursor(&_opCtx)->next()->id;
            ASSERT_OK(coll->insertDocument(
                &_opCtx, InsertStatement(BSON("_id" << 2 << "a" << 2)), nullOpDebug, true));
            ASSERT_OK(coll->insertDocument(
                &_opCtx, InsertStatement(BSON("_id" << 3 << "b" << 1)), nullOpDebug, true));
            wunit.commit();
        }

        const std::string indexName = "bad_index";
        auto status = dbtests::createIndexFromSpec(
            &_opCtx,
            coll->ns().ns(),
            BSON("name" << indexName << "key" << BSON("a" << 1) << "v"
                        << static_cast<int>(kIndexVersion) << "background" << false));

        ASSERT_OK(status);
        ASSERT_TRUE(checkValid());

        lockDb(MODE_X);

        // Replace a correct index entry with a bad one and check it's invalid.
        IndexCatalog* indexCatalog = coll->getIndexCatalog();
        auto descriptor = indexCatalog->findIndexByName(&_opCtx, indexName);
        auto iam =
            const_cast<IndexAccessMethod*>(indexCatalog->getEntry(descriptor)->accessMethod());

        {
            WriteUnitOfWork wunit(&_opCtx);
            int64_t numDeleted;
            InsertResult insertResult;
            const BSONObj actualKey = BSON("a" << 1);
            const BSONObj badKey = BSON("a" << -1);
            InsertDeleteOptions options;
            options.dupsAllowed = true;
            options.logIfError = true;

            KeyStringSet keys;
            iam->getKeys(actualKey,
                         IndexAccessMethod::GetKeysMode::kRelaxConstraintsUnfiltered,
                         &keys,
                         nullptr,
                         nullptr,
                         id1);
            auto removeStatus =
                iam->removeKeys(&_opCtx, {keys.begin(), keys.end()}, id1, options, &numDeleted);
            auto insertStatus = iam->insert(&_opCtx, badKey, id1, options, &insertResult);

            ASSERT_EQUALS(numDeleted, 1);
            ASSERT_EQUALS(insertResult.numInserted, 1);
            ASSERT_OK(removeStatus);
            ASSERT_OK(insertStatus);
            wunit.commit();
        }

        ASSERT_FALSE(checkValid());
        releaseDb();
    }
};

template <bool full, bool background>
class ValidateIndexOrdering : public ValidateBase {
public:
    ValidateIndexOrdering() : ValidateBase(full, background) {}

    void run() {

        // Can't do it in background if the RecordStore is not in RecordId order.
        if (_background && !_isInRecordIdOrder) {
            return;
        }

        // Create a new collection, insert three records and check it's valid.
        lockDb(MODE_X);
        OpDebug* const nullOpDebug = nullptr;
        Collection* coll;
        RecordId id1;
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            coll = _db->createCollection(&_opCtx, _nss);

            ASSERT_OK(coll->insertDocument(
                &_opCtx, InsertStatement(BSON("_id" << 1 << "a" << 1)), nullOpDebug, true));
            id1 = coll->getCursor(&_opCtx)->next()->id;
            ASSERT_OK(coll->insertDocument(
                &_opCtx, InsertStatement(BSON("_id" << 2 << "a" << 2)), nullOpDebug, true));
            ASSERT_OK(coll->insertDocument(
                &_opCtx, InsertStatement(BSON("_id" << 3 << "b" << 1)), nullOpDebug, true));
            wunit.commit();
        }

        const std::string indexName = "bad_index";
        auto status = dbtests::createIndexFromSpec(
            &_opCtx,
            coll->ns().ns(),
            BSON("name" << indexName << "key" << BSON("a" << 1) << "v"
                        << static_cast<int>(kIndexVersion) << "background" << false));

        ASSERT_OK(status);
        ASSERT_TRUE(checkValid());

        lockDb(MODE_X);

        // Change the IndexDescriptor's keyPattern to descending so the index ordering
        // appears wrong.
        IndexCatalog* indexCatalog = coll->getIndexCatalog();
        IndexDescriptor* descriptor =
            const_cast<IndexDescriptor*>(indexCatalog->findIndexByName(&_opCtx, indexName));
        descriptor->setKeyPatternForTest(BSON("a" << -1));

        ASSERT_FALSE(checkValid());
        releaseDb();
    }
};

template <bool full, bool background>
class ValidateWildCardIndex : public ValidateBase {
public:
    ValidateWildCardIndex() : ValidateBase(full, background) {}

    void run() {
        // Can't perform background validation if the RecordStore is not in RecordId order.
        if (_background && !_isInRecordIdOrder) {
            return;
        }

        // Create a new collection.
        lockDb(MODE_X);
        Collection* coll;
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            coll = _db->createCollection(&_opCtx, _nss);
            wunit.commit();
        }

        // Create a $** index.
        const auto indexName = "wildcardIndex";
        const auto indexKey = BSON("$**" << 1);
        auto status = dbtests::createIndexFromSpec(
            &_opCtx,
            coll->ns().ns(),
            BSON("name" << indexName << "key" << indexKey << "v" << static_cast<int>(kIndexVersion)
                        << "background" << false));
        ASSERT_OK(status);

        // Insert non-multikey documents.
        OpDebug* const nullOpDebug = nullptr;
        lockDb(MODE_X);
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(
                coll->insertDocument(&_opCtx,
                                     InsertStatement(BSON("_id" << 1 << "a" << 1 << "b" << 1)),
                                     nullOpDebug,
                                     true));
            ASSERT_OK(
                coll->insertDocument(&_opCtx,
                                     InsertStatement(BSON("_id" << 2 << "b" << BSON("0" << 1))),
                                     nullOpDebug,
                                     true));
            wunit.commit();
        }
        ASSERT_TRUE(checkValid());

        // Insert multikey documents.
        lockDb(MODE_X);
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(coll->insertDocument(
                &_opCtx,
                InsertStatement(BSON("_id" << 3 << "mk_1" << BSON_ARRAY(1 << 2 << 3))),
                nullOpDebug,
                true));
            ASSERT_OK(coll->insertDocument(
                &_opCtx,
                InsertStatement(BSON("_id" << 4 << "mk_2" << BSON_ARRAY(BSON("e" << 1)))),
                nullOpDebug,
                true));
            wunit.commit();
        }
        ASSERT_TRUE(checkValid());

        // Insert additional multikey path metadata index keys.
        lockDb(MODE_X);
        const RecordId recordId(RecordId::ReservedId::kWildcardMultikeyMetadataId);
        IndexCatalog* indexCatalog = coll->getIndexCatalog();
        auto descriptor = indexCatalog->findIndexByName(&_opCtx, indexName);
        auto accessMethod =
            const_cast<IndexAccessMethod*>(indexCatalog->getEntry(descriptor)->accessMethod());
        auto sortedDataInterface = accessMethod->getSortedDataInterface();
        {
            WriteUnitOfWork wunit(&_opCtx);
            const BSONObj indexKey = BSON("" << 1 << ""
                                             << "non_existent_path");
            auto insertStatus =
                sortedDataInterface->insert(&_opCtx, indexKey, recordId, true /* dupsAllowed */);
            ASSERT_OK(insertStatus);
            wunit.commit();
        }

        // An index whose set of multikey metadata paths is a superset of collection multikey
        // metadata paths is valid.
        ASSERT_TRUE(checkValid());

        // Remove the multikey path metadata index key for a path that exists and is multikey in the
        // collection.
        lockDb(MODE_X);
        {
            WriteUnitOfWork wunit(&_opCtx);
            const BSONObj indexKey = BSON("" << 1 << ""
                                             << "mk_1");
            sortedDataInterface->unindex(&_opCtx, indexKey, recordId, true /* dupsAllowed */);
            wunit.commit();
        }

        // An index that is missing one or more multikey metadata fields that exist in the
        // collection is not valid.
        ASSERT_FALSE(checkValid());

        releaseDb();
    }
};

template <bool full, bool background>
class ValidateWildCardIndexWithProjection : public ValidateBase {
public:
    ValidateWildCardIndexWithProjection() : ValidateBase(full, background) {}

    void run() {
        // Can't perform background validation if the RecordStore is not in RecordId order.
        if (_background && !_isInRecordIdOrder) {
            return;
        }

        // Create a new collection.
        lockDb(MODE_X);
        Collection* coll;
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            coll = _db->createCollection(&_opCtx, _nss);
            wunit.commit();
        }

        // Create a $** index with a projection on "a".
        const auto indexName = "wildcardIndex";
        const auto indexKey = BSON("a.$**" << 1);
        auto status = dbtests::createIndexFromSpec(
            &_opCtx,
            coll->ns().ns(),
            BSON("name" << indexName << "key" << indexKey << "v" << static_cast<int>(kIndexVersion)
                        << "background" << false));
        ASSERT_OK(status);

        // Insert documents with indexed and not-indexed paths.
        OpDebug* const nullOpDebug = nullptr;
        lockDb(MODE_X);
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(
                coll->insertDocument(&_opCtx,
                                     InsertStatement(BSON("_id" << 1 << "a" << 1 << "b" << 1)),
                                     nullOpDebug,
                                     true));
            ASSERT_OK(
                coll->insertDocument(&_opCtx,
                                     InsertStatement(BSON("_id" << 2 << "a" << BSON("w" << 1))),
                                     nullOpDebug,
                                     true));
            ASSERT_OK(coll->insertDocument(
                &_opCtx,
                InsertStatement(BSON("_id" << 3 << "a" << BSON_ARRAY("x" << 1))),
                nullOpDebug,
                true));
            ASSERT_OK(coll->insertDocument(
                &_opCtx, InsertStatement(BSON("_id" << 4 << "b" << 2)), nullOpDebug, true));
            ASSERT_OK(
                coll->insertDocument(&_opCtx,
                                     InsertStatement(BSON("_id" << 5 << "b" << BSON("y" << 1))),
                                     nullOpDebug,
                                     true));
            ASSERT_OK(coll->insertDocument(
                &_opCtx,
                InsertStatement(BSON("_id" << 6 << "b" << BSON_ARRAY("z" << 1))),
                nullOpDebug,
                true));
            wunit.commit();
        }
        ASSERT_TRUE(checkValid());

        lockDb(MODE_X);
        IndexCatalog* indexCatalog = coll->getIndexCatalog();
        auto descriptor = indexCatalog->findIndexByName(&_opCtx, indexName);
        auto accessMethod =
            const_cast<IndexAccessMethod*>(indexCatalog->getEntry(descriptor)->accessMethod());
        auto sortedDataInterface = accessMethod->getSortedDataInterface();

        // Removing a multikey metadata path for a path included in the projection causes validate
        // to fail.
        lockDb(MODE_X);
        {
            WriteUnitOfWork wunit(&_opCtx);
            const BSONObj indexKey = BSON("" << 1 << ""
                                             << "a");
            RecordId recordId(RecordId::ReservedId::kWildcardMultikeyMetadataId);
            sortedDataInterface->unindex(&_opCtx, indexKey, recordId, true /* dupsAllowed */);
            wunit.commit();
        }
        ASSERT_FALSE(checkValid());

        releaseDb();
    }
};

template <bool full, bool background>
class ValidateMissingAndExtraIndexEntryResults : public ValidateBase {
public:
    ValidateMissingAndExtraIndexEntryResults() : ValidateBase(full, background) {}

    void run() {
        // Can't perform background validation if the RecordStore is not in RecordId order.
        if (_background && !_isInRecordIdOrder) {
            return;
        }

        // Create a new collection.
        lockDb(MODE_X);
        Collection* coll;
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            coll = _db->createCollection(&_opCtx, _nss);
            wunit.commit();
        }

        // Create an index.
        const auto indexName = "a";
        const auto indexKey = BSON("a" << 1);
        auto status = dbtests::createIndexFromSpec(
            &_opCtx,
            coll->ns().ns(),
            BSON("name" << indexName << "key" << indexKey << "v" << static_cast<int>(kIndexVersion)
                        << "background" << false));
        ASSERT_OK(status);

        // Insert documents.
        OpDebug* const nullOpDebug = nullptr;
        RecordId rid = RecordId::min();
        lockDb(MODE_X);
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(coll->insertDocument(
                &_opCtx, InsertStatement(BSON("_id" << 1 << "a" << 1)), nullOpDebug, true));
            ASSERT_OK(coll->insertDocument(
                &_opCtx, InsertStatement(BSON("_id" << 2 << "a" << 2)), nullOpDebug, true));
            ASSERT_OK(coll->insertDocument(
                &_opCtx, InsertStatement(BSON("_id" << 3 << "a" << 3)), nullOpDebug, true));
            rid = coll->getCursor(&_opCtx)->next()->id;
            wunit.commit();
        }
        ASSERT_TRUE(checkValid());

        RecordStore* rs = coll->getRecordStore();

        // Updating a document without updating the index entry should cause us to have a missing
        // index entry and an extra index entry.
        {
            WriteUnitOfWork wunit(&_opCtx);
            auto doc = BSON("_id" << 1 << "a" << 5);
            auto updateStatus = rs->updateRecord(&_opCtx, rid, doc.objdata(), doc.objsize());
            ASSERT_OK(updateStatus);
            wunit.commit();
        }

        {
            ValidateResults results;
            BSONObjBuilder output;

            lockDb(MODE_IX);
            std::unique_ptr<Lock::CollectionLock> lock =
                std::make_unique<Lock::CollectionLock>(&_opCtx, _nss, MODE_X);

            Database* db = _autoDb.get()->getDb();
            ASSERT_OK(CollectionValidation::validate(&_opCtx,
                                                     db->getCollection(&_opCtx, _nss),
                                                     kValidateFull,
                                                     _background,
                                                     &results,
                                                     &output));

            ASSERT_EQ(false, results.valid);
            ASSERT_EQ(static_cast<size_t>(1), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(2), results.warnings.size());
            ASSERT_EQ(static_cast<size_t>(1), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(1), results.missingIndexEntries.size());
        }

        releaseDb();
    }
};

template <bool full, bool background>
class ValidateMissingIndexEntryResults : public ValidateBase {
public:
    ValidateMissingIndexEntryResults() : ValidateBase(full, background) {}

    void run() {
        // Can't perform background validation if the RecordStore is not in RecordId order.
        if (_background && !_isInRecordIdOrder) {
            return;
        }

        // Create a new collection.
        lockDb(MODE_X);
        Collection* coll;
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            coll = _db->createCollection(&_opCtx, _nss);
            wunit.commit();
        }

        // Create an index.
        const auto indexName = "a";
        const auto indexKey = BSON("a" << 1);
        auto status = dbtests::createIndexFromSpec(
            &_opCtx,
            coll->ns().ns(),
            BSON("name" << indexName << "key" << indexKey << "v" << static_cast<int>(kIndexVersion)
                        << "background" << false));
        ASSERT_OK(status);

        // Insert documents.
        OpDebug* const nullOpDebug = nullptr;
        RecordId rid = RecordId::min();
        lockDb(MODE_X);
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(coll->insertDocument(
                &_opCtx, InsertStatement(BSON("_id" << 1 << "a" << 1)), nullOpDebug, true));
            ASSERT_OK(coll->insertDocument(
                &_opCtx, InsertStatement(BSON("_id" << 2 << "a" << 2)), nullOpDebug, true));
            ASSERT_OK(coll->insertDocument(
                &_opCtx, InsertStatement(BSON("_id" << 3 << "a" << 3)), nullOpDebug, true));
            rid = coll->getCursor(&_opCtx)->next()->id;
            wunit.commit();
        }
        ASSERT_TRUE(checkValid());

        // Removing an index entry without removing the document should cause us to have a missing
        // index entry.
        {
            lockDb(MODE_X);

            IndexCatalog* indexCatalog = coll->getIndexCatalog();
            auto descriptor = indexCatalog->findIndexByName(&_opCtx, indexName);
            auto iam =
                const_cast<IndexAccessMethod*>(indexCatalog->getEntry(descriptor)->accessMethod());

            WriteUnitOfWork wunit(&_opCtx);
            int64_t numDeleted;
            const BSONObj actualKey = BSON("a" << 1);
            InsertDeleteOptions options;
            options.logIfError = true;
            options.dupsAllowed = true;

            KeyStringSet keys;
            iam->getKeys(actualKey,
                         IndexAccessMethod::GetKeysMode::kRelaxConstraintsUnfiltered,
                         &keys,
                         nullptr,
                         nullptr,
                         rid);
            auto removeStatus =
                iam->removeKeys(&_opCtx, {keys.begin(), keys.end()}, rid, options, &numDeleted);

            ASSERT_EQUALS(numDeleted, 1);
            ASSERT_OK(removeStatus);
            wunit.commit();
        }

        {
            ValidateResults results;
            BSONObjBuilder output;

            lockDb(MODE_IX);
            std::unique_ptr<Lock::CollectionLock> lock =
                std::make_unique<Lock::CollectionLock>(&_opCtx, _nss, MODE_X);

            Database* db = _autoDb.get()->getDb();
            ASSERT_OK(CollectionValidation::validate(&_opCtx,
                                                     db->getCollection(&_opCtx, _nss),
                                                     kValidateFull,
                                                     _background,
                                                     &results,
                                                     &output));

            ASSERT_EQ(false, results.valid);
            ASSERT_EQ(static_cast<size_t>(1), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(1), results.warnings.size());
            ASSERT_EQ(static_cast<size_t>(0), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(1), results.missingIndexEntries.size());
        }

        releaseDb();
    }
};

template <bool full, bool background>
class ValidateExtraIndexEntryResults : public ValidateBase {
public:
    ValidateExtraIndexEntryResults() : ValidateBase(full, background) {}

    void run() {
        // Can't perform background validation if the RecordStore is not in RecordId order.
        if (_background && !_isInRecordIdOrder) {
            return;
        }

        // Create a new collection.
        lockDb(MODE_X);
        Collection* coll;
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(_db->dropCollection(&_opCtx, _nss));
            coll = _db->createCollection(&_opCtx, _nss);
            wunit.commit();
        }

        // Create an index.
        const auto indexName = "a";
        const auto indexKey = BSON("a" << 1);
        auto status = dbtests::createIndexFromSpec(
            &_opCtx,
            coll->ns().ns(),
            BSON("name" << indexName << "key" << indexKey << "v" << static_cast<int>(kIndexVersion)
                        << "background" << false));
        ASSERT_OK(status);

        // Insert documents.
        OpDebug* const nullOpDebug = nullptr;
        RecordId rid = RecordId::min();
        lockDb(MODE_X);
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(coll->insertDocument(
                &_opCtx, InsertStatement(BSON("_id" << 1 << "a" << 1)), nullOpDebug, true));
            ASSERT_OK(coll->insertDocument(
                &_opCtx, InsertStatement(BSON("_id" << 2 << "a" << 2)), nullOpDebug, true));
            ASSERT_OK(coll->insertDocument(
                &_opCtx, InsertStatement(BSON("_id" << 3 << "a" << 3)), nullOpDebug, true));
            rid = coll->getCursor(&_opCtx)->next()->id;
            wunit.commit();
        }
        ASSERT_TRUE(checkValid());

        // Removing a document without removing the index entries should cause us to have extra
        // index entries.
        {
            lockDb(MODE_X);
            RecordStore* rs = coll->getRecordStore();

            WriteUnitOfWork wunit(&_opCtx);
            rs->deleteRecord(&_opCtx, rid);
            wunit.commit();
        }

        {
            ValidateResults results;
            BSONObjBuilder output;

            lockDb(MODE_IX);
            std::unique_ptr<Lock::CollectionLock> lock =
                std::make_unique<Lock::CollectionLock>(&_opCtx, _nss, MODE_X);

            Database* db = _autoDb.get()->getDb();
            ASSERT_OK(CollectionValidation::validate(&_opCtx,
                                                     db->getCollection(&_opCtx, _nss),
                                                     kValidateFull,
                                                     _background,
                                                     &results,
                                                     &output));

            ASSERT_EQ(false, results.valid);
            ASSERT_EQ(static_cast<size_t>(2), results.errors.size());
            ASSERT_EQ(static_cast<size_t>(1), results.warnings.size());
            ASSERT_EQ(static_cast<size_t>(2), results.extraIndexEntries.size());
            ASSERT_EQ(static_cast<size_t>(0), results.missingIndexEntries.size());
        }

        releaseDb();
    }
};

class ValidateTests : public Suite {
public:
    ValidateTests() : Suite("validate_tests") {}

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
        add<ValidateWildCardIndexWithProjection<false, false>>();

        // Tests for index validation.
        add<ValidateIndexEntry<false, false>>();
        add<ValidateIndexEntry<false, true>>();
        add<ValidateIndexOrdering<false, false>>();
        add<ValidateIndexOrdering<false, true>>();

        // Tests that the 'missingIndexEntries' and 'extraIndexEntries' field are populated
        // correctly.
        add<ValidateMissingAndExtraIndexEntryResults<false, false>>();
        add<ValidateMissingIndexEntryResults<false, false>>();
        add<ValidateExtraIndexEntryResults<false, false>>();
    }
} validateTests;
}  // namespace ValidateTests
