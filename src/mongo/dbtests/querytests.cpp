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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/ordering.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/client/index_spec.h"
#include "mongo/db/client.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index_builds/index_build_interceptor.h"
#include "mongo/db/index_builds/multi_index_block.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/database.h"
#include "mongo/db/local_catalog/database_holder.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/client_cursor/clientcursor.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/cursor_manager.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/query_settings/query_settings_service.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/tenant_id.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/timer.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {

void insertOplogDocument(OperationContext* opCtx, Timestamp ts, StringData ns) {
    AutoGetCollection coll(opCtx, NamespaceString::createNamespaceString_forTest(ns), MODE_IX);
    WriteUnitOfWork wuow(opCtx);
    auto doc = BSON("ts" << ts);
    InsertStatement stmt;
    stmt.doc = doc;
    stmt.oplogSlot = OplogSlot{ts, OplogSlot::kInitialTerm};

    if (coll->needsCappedLock()) {
        // TODO SERVER-106004: Revisit this when cleaning up code around reserving oplog slots for
        // inserts into capped collections.
        Lock::ResourceLock heldUntilEndOfWUOW{
            opCtx, ResourceId(RESOURCE_METADATA, coll->ns()), MODE_X};
    }
    auto status = collection_internal::insertDocument(opCtx, *coll, stmt, nullptr);
    if (!status.isOK()) {
        std::cout << "Failed to insert oplog document: " << status.toString() << std::endl;
    }
    wuow.commit();
}

void deleteAll(OperationContext& opCtx, const NamespaceString& ns) {
    // Delete one-at-a-time because deleting all at once may used a batched delete which will fail
    // upon encountering a document that does not contain an _id.
    DBDirectClient client{&opCtx};
    while (client.count(ns)) {
        write_ops::checkWriteErrors(client.remove({ns, {{{}, false}}}));
    }
}

Database* getDbOrCreate(OperationContext* opCtx, const NamespaceString& nss) {
    auto db = DatabaseHolder::get(opCtx)->getDb(opCtx, nss.dbName());

    if (!db) {
        return DatabaseHolder::get(opCtx)->openDb(opCtx, nss.dbName(), nullptr);
    }
    return db;
}

using std::endl;
using std::string;
using std::unique_ptr;
using std::vector;

class Base {
public:
    Base() : _lk(&_opCtx) {
        {
            _database = getDbOrCreate(&_opCtx, nss());
            WriteUnitOfWork wunit(&_opCtx);
            // TODO(SERVER-103403): Investigate usage validity of
            // CollectionPtr::CollectionPtr_UNSAFE
            CollectionPtr collection = CollectionPtr::CollectionPtr_UNSAFE(
                CollectionCatalog::get(&_opCtx)->lookupCollectionByNamespace(&_opCtx, nss()));
            if (collection) {
                _database->dropCollection(&_opCtx, nss()).transitional_ignore();
            }
            _database->createCollection(&_opCtx, nss());
            wunit.commit();
        }
        _collection = acquireCollection(&_opCtx,
                                        CollectionAcquisitionRequest::fromOpCtx(
                                            &_opCtx, nss(), AcquisitionPrerequisites::kWrite),
                                        MODE_IX);
        ASSERT(_collection->exists());
        addIndex(IndexSpec().addKey("a").unique(false));
    }

    ~Base() {
        try {
            WriteUnitOfWork wunit(&_opCtx);
            uassertStatusOK(_database->dropCollection(&_opCtx, nss()));
            wunit.commit();
        } catch (...) {
            FAIL("Exception while cleaning up collection");
        }
    }

protected:
    const NamespaceString& nss() {
        return _nss;
    }

    void addIndex(const IndexSpec& spec) {
        BSONObjBuilder builder(spec.toBSON());
        builder.append("v", int(IndexConfig::kLatestIndexVersion));
        auto specObj = builder.obj();

        CollectionWriter collection(&_opCtx, &(*_collection));
        MultiIndexBlock indexer;
        ScopeGuard abortOnExit([&] {
            indexer.abortIndexBuild(&_opCtx, collection, MultiIndexBlock::kNoopOnCleanUpFn);
        });
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT_OK(dbtests::initializeMultiIndexBlock(&_opCtx, collection, indexer, specObj));
            wunit.commit();
        }
        uassertStatusOK(indexer.insertAllDocumentsInCollection(&_opCtx, _collection->nss()));
        uassertStatusOK(
            indexer.drainBackgroundWrites(&_opCtx,
                                          RecoveryUnit::ReadSource::kNoTimestamp,
                                          IndexBuildInterceptor::DrainYieldPolicy::kNoYield));
        uassertStatusOK(indexer.checkConstraints(&_opCtx, collection.get()));
        {
            WriteUnitOfWork wunit(&_opCtx);
            uassertStatusOK(indexer.commit(&_opCtx,
                                           collection.getWritableCollection(&_opCtx),
                                           MultiIndexBlock::kNoopOnCreateEachFn,
                                           MultiIndexBlock::kNoopOnCommitFn));
            wunit.commit();
        }
        abortOnExit.dismiss();
    }

    void insert(const char* s) {
        insert(fromjson(s));
    }

    void insert(const BSONObj& o) {
        WriteUnitOfWork wunit(&_opCtx);
        OpDebug* const nullOpDebug = nullptr;
        if (o["_id"].eoo()) {
            BSONObjBuilder b;
            OID oid;
            oid.init();
            b.appendOID("_id", &oid);
            b.appendElements(o);
            collection_internal::insertDocument(&_opCtx,
                                                _collection->getCollectionPtr(),
                                                InsertStatement(b.obj()),
                                                nullOpDebug,
                                                false)
                .transitional_ignore();
        } else {
            collection_internal::insertDocument(
                &_opCtx, _collection->getCollectionPtr(), InsertStatement(o), nullOpDebug, false)
                .transitional_ignore();
        }
        wunit.commit();
    }

    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("unittests.querytests");
    const ServiceContext::UniqueOperationContext _opCtxPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_opCtxPtr;
    Lock::GlobalWrite _lk;

    Database* _database;
    boost::optional<CollectionAcquisition> _collection;
};

class FindOneOr : public Base {
public:
    void run() {
        addIndex(IndexSpec().addKey("b").unique(false));
        addIndex(IndexSpec().addKey("c").unique(false));

        insert(BSON("b" << 2 << "_id" << 0));
        insert(BSON("c" << 3 << "_id" << 1));
        BSONObj query = fromjson("{$or:[{b:2},{c:3}]}");
        BSONObj ret;
        // Check findOne() returning object.
        ASSERT(Helpers::findOne(&_opCtx, *_collection, query, ret));
        ASSERT_EQUALS(string("b"), ret.firstElement().fieldName());
        // Cross check with findOne() returning location.
        ASSERT_BSONOBJ_EQ(ret,
                          _collection->getCollectionPtr()
                              ->docFor(&_opCtx, Helpers::findOne(&_opCtx, *_collection, query))
                              .value());
    }
};

class FindOneEmptyObj : public Base {
public:
    void run() {
        // We don't normally allow empty objects in the database, but test that we can find
        // an empty object (one might be allowed inside a reserved namespace at some point).
        Lock::GlobalWrite lk(&_opCtx);
        {
            WriteUnitOfWork wunit(&_opCtx);
            Database* db = getDbOrCreate(&_opCtx, nss());
            if (CollectionCatalog::get(&_opCtx)->lookupCollectionByNamespace(&_opCtx, nss())) {
                _collection.reset();
                db->dropCollection(&_opCtx, nss()).transitional_ignore();
            }
            db->createCollection(&_opCtx, nss(), CollectionOptions(), false);
            wunit.commit();
        }

        DBDirectClient cl(&_opCtx);
        BSONObj info;
        bool ok = cl.runCommand(nss().dbName(),
                                BSON("godinsert" << "querytests"
                                                 << "obj" << BSONObj()),
                                info);
        ASSERT(ok);
        _collection = acquireCollection(&_opCtx,
                                        CollectionAcquisitionRequest::fromOpCtx(
                                            &_opCtx, nss(), AcquisitionPrerequisites::kRead),
                                        MODE_IS);
        ASSERT(_collection->exists());
        insert(BSONObj());
        BSONObj query;
        BSONObj ret;
        ASSERT(Helpers::findOne(&_opCtx, *_collection, query, ret));
        ASSERT(ret.isEmpty());
        ASSERT_BSONOBJ_EQ(ret,
                          _collection->getCollectionPtr()
                              ->docFor(&_opCtx, Helpers::findOne(&_opCtx, *_collection, query))
                              .value());
    }
};

class ClientBase {
public:
    ClientBase() : _client(&_opCtx) {
        // Initialize the query settings.
        query_settings::QuerySettingsService::initializeForTest(_opCtx.getServiceContext());
    }

protected:
    void insert(const NamespaceString& nss, BSONObj o) {
        _client.insert(nss, o);
    }

    const ServiceContext::UniqueOperationContext _opCtxPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_opCtxPtr;
    DBDirectClient _client;
};

class BoundedKey : public ClientBase {
public:
    ~BoundedKey() {
        _client.dropCollection(_nss);
    }
    void run() {
        insert(_nss, BSON("a" << 1));
        BSONObjBuilder a;
        a.appendMaxKey("$lt");
        BSONObj limit = a.done();
        ASSERT(!_client.findOne(_nss, BSON("a" << limit)).isEmpty());
        ASSERT_OK(dbtests::createIndex(&_opCtx, _nss.ns_forTest(), BSON("a" << 1)));
        FindCommandRequest findCmd{_nss};
        findCmd.setFilter(BSON("a" << limit));
        findCmd.setHint(BSON("a" << 1));
        ASSERT(!_client.findOne(std::move(findCmd)).isEmpty());
    }

private:
    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("unittests.querytests.BoundedKey");
};

class GetMore : public ClientBase {
public:
    ~GetMore() {
        _client.dropCollection(_nss);
    }
    void run() {
        insert(_nss, BSON("a" << 1));
        insert(_nss, BSON("a" << 2));
        insert(_nss, BSON("a" << 3));
        FindCommandRequest findRequest{_nss};
        findRequest.setBatchSize(2);
        std::unique_ptr<DBClientCursor> cursor = _client.find(findRequest);
        long long cursorId = cursor->getCursorId();

        {
            // Check that a cursor has been registered with the global cursor manager, and has
            // already returned its first batch of results.
            auto pinnedCursor = unittest::assertGet(
                CursorManager::get(&_opCtx)->pinCursor(&_opCtx, cursorId, "getMore"));
            ASSERT_EQUALS(1ull, pinnedCursor.getCursor()->getNBatches());
        }

        int counter = 0;
        while (cursor->more()) {
            ASSERT_EQUALS(++counter, cursor->next().getIntField("a"));
        }
        ASSERT_EQ(counter, 3);
    }

private:
    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("unittests.querytests.GetMore");
};

/**
 * Setting killAllOperations causes further getmores to fail.
 */
class GetMoreKillOp : public ClientBase {
public:
    ~GetMoreKillOp() {
        _opCtx.getServiceContext()->unsetKillAllOperations();
        _client.dropCollection(_nss);
    }
    void run() {
        // Create a collection with some data.
        for (int i = 0; i < 1000; ++i) {
            insert(_nss, BSON("a" << i));
        }

        // Create a cursor on the collection, with a batch size of 200.
        FindCommandRequest findRequest{_nss};
        findRequest.setBatchSize(200);
        auto cursor = _client.find(std::move(findRequest));

        // Count 500 results, spanning a few batches of documents.
        for (int i = 0; i < 500; ++i) {
            ASSERT(cursor->more());
            cursor->next();
        }

        // Set the killop kill all flag, forcing the next get more to fail with a kill op
        // exception.
        _opCtx.getServiceContext()->setKillAllOperations();
        ASSERT_THROWS_CODE(([&] {
                               while (cursor->more()) {
                                   cursor->next();
                               }
                           }()),
                           AssertionException,
                           ErrorCodes::InterruptedAtShutdown);

        // Revert the killop kill all flag.
        _opCtx.getServiceContext()->unsetKillAllOperations();
    }

private:
    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("unittests.querytests.GetMoreKillOp");
};

class PositiveLimit : public ClientBase {
public:
    PositiveLimit() {}
    ~PositiveLimit() {
        _client.dropCollection(_nss);
    }

    void testLimit(int limit, int expectedCount) {
        FindCommandRequest findRequest{_nss};
        findRequest.setLimit(limit);
        ASSERT_EQUALS(_client.find(std::move(findRequest))->itcount(), expectedCount);
    }

    void run() {
        const int collSize = 1000;
        for (int i = 0; i < collSize; i++)
            insert(_nss, BSON("_id" << OID::gen() << "i" << i));

        testLimit(1, 1);
        testLimit(10, 10);
        testLimit(101, 101);
        testLimit(collSize - 1, collSize - 1);
        testLimit(collSize, collSize);
        testLimit(collSize + 1, collSize);
        testLimit(collSize + 10, collSize);
    }

private:
    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("unittests.querytests.PositiveLimit");
};

class TailNotAtEnd : public ClientBase {
public:
    ~TailNotAtEnd() {
        _client.dropCollection(_nss);
    }

    void run() {
        // Skip the test if the storage engine doesn't support capped collections.
        if (!_opCtx.getServiceContext()->getStorageEngine()->supportsCappedCollections()) {
            return;
        }

        _client.createCollection(_nss, 2047, true);
        insert(_nss, BSON("a" << 0));
        insert(_nss, BSON("a" << 1));
        insert(_nss, BSON("a" << 2));

        FindCommandRequest findRequest{_nss};
        findRequest.setHint(BSON("$natural" << 1));
        findRequest.setTailable(true);
        findRequest.setBatchSize(2);
        std::unique_ptr<DBClientCursor> c = _client.find(std::move(findRequest));

        ASSERT(0 != c->getCursorId());
        while (c->more())
            c->next();
        ASSERT(0 != c->getCursorId());
        insert(_nss, BSON("a" << 3));
        insert(_nss, BSON("a" << 4));
        insert(_nss, BSON("a" << 5));
        insert(_nss, BSON("a" << 6));
        ASSERT(c->more());
        ASSERT_EQUALS(3, c->next().getIntField("a"));
    }

private:
    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("unittests.querytests.TailNotAtEnd");
};

class EmptyTail : public ClientBase {
public:
    ~EmptyTail() {
        _client.dropCollection(_nss);
    }
    void run() {
        // Skip the test if the storage engine doesn't support capped collections.
        if (!_opCtx.getServiceContext()->getStorageEngine()->supportsCappedCollections()) {
            return;
        }

        _client.createCollection(_nss, 1900, true);

        FindCommandRequest findRequest{_nss};
        findRequest.setHint(BSON("$natural" << 1));
        findRequest.setTailable(true);
        findRequest.setBatchSize(2);

        std::unique_ptr<DBClientCursor> c = _client.find(findRequest);
        ASSERT_EQUALS(0, c->getCursorId());
        ASSERT(c->isDead());

        insert(_nss, BSON("a" << 0));
        findRequest.setFilter(BSON("a" << 1));
        c = _client.find(findRequest);
        ASSERT(0 != c->getCursorId());
        ASSERT(!c->isDead());
    }

private:
    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("unittests.querytests.EmptyTail");
};

class TailableDelete : public ClientBase {
public:
    ~TailableDelete() {
        _client.dropCollection(_nss);
    }
    void run() {
        // Skip the test if the storage engine doesn't support capped collections.
        if (!_opCtx.getServiceContext()->getStorageEngine()->supportsCappedCollections()) {
            return;
        }

        _client.createCollection(_nss, 8192, true, 2);
        insert(_nss, BSON("a" << 0));
        insert(_nss, BSON("a" << 1));

        FindCommandRequest findRequest{_nss};
        findRequest.setHint(BSON("$natural" << 1));
        findRequest.setTailable(true);
        findRequest.setBatchSize(2);
        std::unique_ptr<DBClientCursor> c = _client.find(std::move(findRequest));
        c->next();
        c->next();
        ASSERT(!c->more());
        insert(_nss, BSON("a" << 2));
        insert(_nss, BSON("a" << 3));

        // We have overwritten the previous cursor position and should encounter a dead cursor.
        ASSERT_THROWS(c->more() ? c->nextSafe() : BSONObj(), AssertionException);
    }

private:
    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("unittests.querytests.TailableDelete");
};

class TailableDelete2 : public ClientBase {
public:
    ~TailableDelete2() {
        _client.dropCollection(_nss);
    }
    void run() {
        // Skip the test if the storage engine doesn't support capped collections.
        if (!_opCtx.getServiceContext()->getStorageEngine()->supportsCappedCollections()) {
            return;
        }

        _client.createCollection(_nss, 8192, true, 2);
        insert(_nss, BSON("a" << 0));
        insert(_nss, BSON("a" << 1));

        FindCommandRequest findRequest{_nss};
        findRequest.setHint(BSON("$natural" << 1));
        findRequest.setTailable(true);
        findRequest.setBatchSize(2);
        std::unique_ptr<DBClientCursor> c = _client.find(std::move(findRequest));
        c->next();
        c->next();
        ASSERT(!c->more());
        insert(_nss, BSON("a" << 2));
        insert(_nss, BSON("a" << 3));
        insert(_nss, BSON("a" << 4));

        // We have overwritten the previous cursor position and should encounter a dead cursor.
        ASSERT_THROWS(c->more() ? c->nextSafe() : BSONObj(), AssertionException);
    }

private:
    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("unittests.querytests.TailableDelete");
};


class TailableInsertDelete : public ClientBase {
public:
    ~TailableInsertDelete() {
        _client.dropCollection(_nss);
    }

    void run() {
        // Skip the test if the storage engine doesn't support capped collections.
        if (!_opCtx.getServiceContext()->getStorageEngine()->supportsCappedCollections()) {
            return;
        }

        _client.createCollection(_nss, 1330, true);
        insert(_nss, BSON("a" << 0));
        insert(_nss, BSON("a" << 1));

        FindCommandRequest findRequest{_nss};
        findRequest.setHint(BSON("$natural" << 1));
        findRequest.setTailable(true);
        findRequest.setBatchSize(2);
        std::unique_ptr<DBClientCursor> c = _client.find(std::move(findRequest));

        c->next();
        c->next();
        ASSERT(!c->more());
        insert(_nss, BSON("a" << 2));
        ASSERT(c->more());
        ASSERT_EQUALS(2, c->next().getIntField("a"));
        ASSERT(!c->more());
    }

private:
    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("unittests.querytests.TailableInsertDelete");
};

class TailCappedOnly : public ClientBase {
public:
    ~TailCappedOnly() {
        _client.dropCollection(_nss);
    }
    void run() {
        _client.insert(_nss, BSONObj());
        FindCommandRequest findRequest{_nss};
        findRequest.setTailable(true);
        ASSERT_THROWS(_client.find(std::move(findRequest)), AssertionException);
    }

private:
    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("unittest.querytests.TailCappedOnly");
};

class TailableQueryOnId : public ClientBase {
public:
    ~TailableQueryOnId() {
        _client.dropCollection(_nss);
    }

    void insertA(const NamespaceString& nss, int a) {
        BSONObjBuilder b;
        b.appendOID("_id", nullptr, true);
        b.appendOID("value", nullptr, true);
        b.append("a", a);
        insert(nss, b.obj());
    }

    void run() {
        // Skip the test if the storage engine doesn't support capped collections.
        if (!_opCtx.getServiceContext()->getStorageEngine()->supportsCappedCollections()) {
            return;
        }

        BSONObj info;
        _client.runCommand(_nss.dbName(),
                           BSON("create" << "querytests.TailableQueryOnId"
                                         << "capped" << true << "size" << 8192),
                           info);
        insertA(_nss, 0);
        insertA(_nss, 1);
        FindCommandRequest findRequest{_nss};
        findRequest.setFilter(BSON("a" << GT << -1));
        findRequest.setTailable(true);
        std::unique_ptr<DBClientCursor> c1 = _client.find(findRequest);
        OID id;
        id.init("000000000000000000000000");
        findRequest.setFilter(BSON("value" << GT << id));
        std::unique_ptr<DBClientCursor> c2 = _client.find(findRequest);
        c1->next();
        c1->next();
        ASSERT(!c1->more());
        c2->next();
        c2->next();
        ASSERT(!c2->more());
        insertA(_nss, 2);
        ASSERT(c1->more());
        ASSERT_EQUALS(2, c1->next().getIntField("a"));
        ASSERT(!c1->more());
        ASSERT(c2->more());
        ASSERT_EQUALS(2, c2->next().getIntField("a"));  // SERVER-645
        ASSERT(!c2->more());
        ASSERT(!c2->isDead());
    }

private:
    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("unittests.querytests.TailableQueryOnId");
};

class OplogScanWithGtTimstampPred : public ClientBase {
public:
    ~OplogScanWithGtTimstampPred() {
        _client.dropCollection(_nss);
    }

    void run() {
        // Skip the test if the storage engine doesn't support capped collections.
        if (!_opCtx.getServiceContext()->getStorageEngine()->supportsCappedCollections()) {
            return;
        }

        // Create a capped collection of size 10.
        _client.dropCollection(_nss);
        _client.createCollection(_nss, 10, true);
        // WiredTiger storage engines forbid dropping of the oplog. Evergreen reuses nodes for
        // testing, so the oplog may already exist on the test node; in this case, trying to create
        // the oplog once again would fail.
        //
        // To ensure we are working with a clean oplog (an oplog without entries), we resort
        // to truncating the oplog instead.
        if (_opCtx.getServiceContext()->getStorageEngine()->supportsRecoveryTimestamp()) {
            deleteAll(_opCtx, _nss);
        }
        const auto ns = _nss.ns_forTest();
        insertOplogDocument(&_opCtx, Timestamp(1000, 0), ns);
        insertOplogDocument(&_opCtx, Timestamp(1000, 1), ns);
        insertOplogDocument(&_opCtx, Timestamp(1000, 2), ns);
        FindCommandRequest findRequest{_nss};
        findRequest.setFilter(BSON("ts" << GT << Timestamp(1000, 1)));
        findRequest.setHint(BSON("$natural" << 1));
        std::unique_ptr<DBClientCursor> c = _client.find(findRequest);
        ASSERT(c->more());
        ASSERT_EQUALS(2u, c->next()["ts"].timestamp().getInc());
        ASSERT(!c->more());

        insert(_nss, BSON("ts" << Timestamp(1000, 3)));
        c = _client.find(findRequest);
        ASSERT(c->more());
        ASSERT_EQUALS(2u, c->next()["ts"].timestamp().getInc());
        ASSERT(c->more());
    }

private:
    const NamespaceString _nss = NamespaceString::createNamespaceString_forTest(
        "local.oplog.querytests.OplogScanWithGtTimstampPred");
};

class OplogScanGtTsExplain : public ClientBase {
public:
    ~OplogScanGtTsExplain() {
        _client.dropCollection(_nss);
    }

    void run() {
        // Skip the test if the storage engine doesn't support capped collections.
        if (!_opCtx.getServiceContext()->getStorageEngine()->supportsCappedCollections()) {
            return;
        }

        // Create a capped collection of size 10.
        _client.dropCollection(_nss);
        _client.createCollection(_nss, 10, true);
        // WiredTiger storage engines forbid dropping of the oplog. Evergreen reuses nodes for
        // testing, so the oplog may already exist on the test node; in this case, trying to create
        // the oplog once again would fail.
        //
        // To ensure we are working with a clean oplog (an oplog without entries), we resort
        // to truncating the oplog instead.
        if (_opCtx.getServiceContext()->getStorageEngine()->supportsRecoveryTimestamp()) {
            deleteAll(_opCtx, _nss);
        }

        const auto ns = _nss.ns_forTest();
        insertOplogDocument(&_opCtx, Timestamp(1000, 0), ns);
        insertOplogDocument(&_opCtx, Timestamp(1000, 1), ns);
        insertOplogDocument(&_opCtx, Timestamp(1000, 2), ns);

        BSONObj explainCmdObj =
            BSON("explain" << BSON("find" << "oplog.querytests.OplogScanGtTsExplain"
                                          << "filter" << BSON("ts" << GT << Timestamp(1000, 1))
                                          << "hint" << BSON("$natural" << 1))
                           << "verbosity"
                           << "executionStats");

        auto reply = _client.runCommand(OpMsgRequestBuilder::create(
            auth::ValidatedTenancyScope::kNotRequired, DatabaseName::kLocal, explainCmdObj));
        BSONObj explainCmdReplyBody = reply->getCommandReply();
        ASSERT_OK(getStatusFromCommandResult(explainCmdReplyBody));

        ASSERT(explainCmdReplyBody.hasField("executionStats")) << explainCmdReplyBody;
        BSONObj execStats = explainCmdReplyBody["executionStats"].Obj();
        ASSERT_EQUALS(1, execStats.getIntField("nReturned")) << explainCmdReplyBody;
    }

private:
    const NamespaceString _nss = NamespaceString::createNamespaceString_forTest(
        "local.oplog.querytests.OplogScanGtTsExplain");
};

class BasicCount : public ClientBase {
public:
    ~BasicCount() {
        _client.dropCollection(_nss);
    }
    void run() {
        ASSERT_OK(dbtests::createIndex(&_opCtx, _nss.ns_forTest(), BSON("a" << 1)));
        count(0);
        insert(_nss, BSON("a" << 3));
        count(0);
        insert(_nss, BSON("a" << 4));
        count(1);
        insert(_nss, BSON("a" << 5));
        count(1);
        insert(_nss, BSON("a" << 4));
        count(2);
    }

private:
    void count(unsigned long long c) {
        ASSERT_EQUALS(c, _client.count(_nss, BSON("a" << 4)));
    }

    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("unittests.querytests.BasicCount");
};

class ArrayId : public ClientBase {
public:
    ~ArrayId() {
        _client.dropCollection(_nss);
    }
    void run() {
        ASSERT_OK(dbtests::createIndex(&_opCtx, _nss.ns_forTest(), BSON("_id" << 1)));

        auto response = _client.insertAcknowledged(_nss, {fromjson("{'_id':[1,2]}")});
        ASSERT_NOT_OK(getStatusFromWriteCommandReply(response));
    }

private:
    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("unittests.querytests.ArrayId");
};

class UnderscoreNs : public ClientBase {
public:
    ~UnderscoreNs() {
        _client.dropCollection(_nss);
    }
    void run() {
        ASSERT(_client.findOne(_nss, BSONObj{}).isEmpty());
        auto response = _client.insertAcknowledged(_nss, {BSON("a" << 1)});
        ASSERT_OK(getStatusFromWriteCommandReply(response));
        ASSERT_EQ(1, response["n"].Int());
        ASSERT_EQUALS(1, _client.findOne(_nss, BSONObj{}).getIntField("a"));
    }

private:
    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("unittests.querytests._UnderscoreNs");
};

class EmptyFieldSpec : public ClientBase {
public:
    ~EmptyFieldSpec() {
        _client.dropCollection(_nss);
    }
    void run() {
        _client.insert(_nss, BSON("a" << 1));
        ASSERT(!_client.findOne(_nss, BSONObj{}).isEmpty());
        FindCommandRequest findCmd{_nss};
        findCmd.setProjection(BSONObj{});
        ASSERT(!_client.findOne(std::move(findCmd)).isEmpty());
    }

private:
    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("unittests.querytests.EmptyFieldSpec");
};

class MultiNe : public ClientBase {
public:
    ~MultiNe() {
        _client.dropCollection(_nss);
    }
    void run() {
        _client.insert(_nss, fromjson("{a:[1,2]}"));
        ASSERT(_client.findOne(_nss, fromjson("{a:{$ne:1}}")).isEmpty());
        BSONObj spec = fromjson("{a:{$ne:1,$ne:2}}");
        ASSERT(_client.findOne(_nss, spec).isEmpty());
    }

private:
    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("unittests.querytests.Ne");
};

class EmbeddedNe : public ClientBase {
public:
    ~EmbeddedNe() {
        _client.dropCollection(_nss);
    }
    void run() {
        _client.insert(_nss, fromjson("{a:[{b:1},{b:2}]}"));
        ASSERT(_client.findOne(_nss, fromjson("{'a.b':{$ne:1}}")).isEmpty());
    }

private:
    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("unittests.querytests.NestedNe");
};

class EmbeddedNumericTypes : public ClientBase {
public:
    ~EmbeddedNumericTypes() {
        _client.dropCollection(_nss);
    }
    void run() {
        _client.insert(_nss, BSON("a" << BSON("b" << 1)));
        ASSERT(!_client.findOne(_nss, BSON("a" << BSON("b" << 1.0))).isEmpty());
        ASSERT_OK(dbtests::createIndex(&_opCtx, _nss.ns_forTest(), BSON("a" << 1)));
        ASSERT(!_client.findOne(_nss, BSON("a" << BSON("b" << 1.0))).isEmpty());
    }

private:
    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("unittests.querytests.NumericEmbedded");
};

class AutoResetIndexCache : public ClientBase {
public:
    ~AutoResetIndexCache() {
        _client.dropCollection(_nss);
    }

    void index() {
        const bool includeBuildUUIDs = false;
        const int options = 0;
        ASSERT_EQUALS(2u, _client.getIndexSpecs(_nss, includeBuildUUIDs, options).size());
    }
    void noIndex() {
        const bool includeBuildUUIDs = false;
        const int options = 0;
        ASSERT_EQUALS(0u, _client.getIndexSpecs(_nss, includeBuildUUIDs, options).size());
    }
    void checkIndex() {
        ASSERT_OK(dbtests::createIndex(&_opCtx, _nss.ns_forTest(), BSON("a" << 1)));
        index();
    }
    void run() {
        _client.dropDatabase(DatabaseName::createDatabaseName_forTest(boost::none, "unittests"));
        noIndex();
        checkIndex();
        _client.dropCollection(_nss);
        noIndex();
        checkIndex();
        _client.dropDatabase(DatabaseName::createDatabaseName_forTest(boost::none, "unittests"));
        noIndex();
        checkIndex();
    }

private:
    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("unittests.querytests.AutoResetIndexCache");
};

class UniqueIndex : public ClientBase {
public:
    ~UniqueIndex() {
        _client.dropCollection(_nss);
    }
    void run() {
        ASSERT_OK(dbtests::createIndex(&_opCtx, _nss.ns_forTest(), BSON("a" << 1), true));
        _client.insert(_nss, BSON("a" << 4 << "b" << 2));
        _client.insert(_nss, BSON("a" << 4 << "b" << 3));
        ASSERT_EQUALS(1U, _client.count(_nss, BSONObj()));
        _client.dropCollection(_nss);
        ASSERT_OK(dbtests::createIndex(&_opCtx, _nss.ns_forTest(), BSON("b" << 1), true));
        _client.insert(_nss, BSON("a" << 4 << "b" << 2));
        _client.insert(_nss, BSON("a" << 4 << "b" << 3));
        ASSERT_EQUALS(2U, _client.count(_nss, BSONObj()));
    }

private:
    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("unittests.querytests.UniqueIndex");
};

class UniqueIndexPreexistingData : public ClientBase {
public:
    ~UniqueIndexPreexistingData() {
        _client.dropCollection(_nss);
    }
    void run() {
        _client.insert(_nss, BSON("a" << 4 << "b" << 2));
        _client.insert(_nss, BSON("a" << 4 << "b" << 3));
        ASSERT_EQUALS(ErrorCodes::DuplicateKey,
                      dbtests::createIndex(&_opCtx, _nss.ns_forTest(), BSON("a" << 1), true));
    }

private:
    const NamespaceString _nss = NamespaceString::createNamespaceString_forTest(
        "unittests.querytests.UniqueIndexPreexistingData");
};

class SubobjectInArray : public ClientBase {
public:
    ~SubobjectInArray() {
        _client.dropCollection(_nss);
    }
    void run() {
        _client.insert(_nss, fromjson("{a:[{b:{c:1}}]}"));
        ASSERT(!_client.findOne(_nss, BSON("a.b.c" << 1)).isEmpty());
        ASSERT(!_client.findOne(_nss, fromjson("{'a.c':null}")).isEmpty());
    }

private:
    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("unittests.querytests.SubobjectInArray");
};

class Size : public ClientBase {
public:
    ~Size() {
        _client.dropCollection(_nss);
    }
    void run() {
        _client.insert(_nss, fromjson("{a:[1,2,3]}"));
        ASSERT_OK(dbtests::createIndex(&_opCtx, _nss.ns_forTest(), BSON("a" << 1)));
        FindCommandRequest findRequest{_nss};
        findRequest.setFilter(BSON("a" << mongo::BSIZE << 3));
        findRequest.setHint(BSON("a" << 1));
        ASSERT(_client.find(std::move(findRequest))->more());
    }

private:
    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("unittests.querytests.Size");
};

class FullArray : public ClientBase {
public:
    ~FullArray() {
        _client.dropCollection(_nss);
    }
    void run() {
        _client.insert(_nss, fromjson("{a:[1,2,3]}"));
        FindCommandRequest findRequest{_nss};
        findRequest.setFilter(fromjson("{a:[1,2,3]}"));
        ASSERT(_client.find(findRequest)->more());
        ASSERT_OK(dbtests::createIndex(&_opCtx, _nss.ns_forTest(), BSON("a" << 1)));
        findRequest.setFilter(fromjson("{a:{$in:[1,[1,2,3]]}}"));
        findRequest.setHint(BSON("a" << 1));
        ASSERT(_client.find(findRequest)->more());
        findRequest.setFilter(fromjson("{a:[1,2,3]}"));
        ASSERT(_client.find(findRequest)->more());
    }

private:
    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("unittests.querytests.IndexedArray");
};

class InsideArray : public ClientBase {
public:
    ~InsideArray() {
        _client.dropCollection(_nss);
    }
    void run() {
        _client.insert(_nss, fromjson("{a:[[1],2]}"));
        check("$natural");
        ASSERT_OK(dbtests::createIndex(&_opCtx, _nss.ns_forTest(), BSON("a" << 1)));
        check("a");
    }

private:
    void check(const string& hintField) {
        FindCommandRequest findRequest{_nss};
        findRequest.setHint(BSON(hintField << 1));
        findRequest.setFilter(fromjson("{a:[[1],2]}"));
        ASSERT(_client.find(findRequest)->more());
        findRequest.setFilter(fromjson("{a:[1]}"));
        ASSERT(_client.find(findRequest)->more());
        findRequest.setFilter(fromjson("{a:2}"));
        ASSERT(_client.find(findRequest)->more());
        findRequest.setFilter(fromjson("{a:1}"));
        ASSERT(!_client.find(findRequest)->more());
    }

    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("unittests.querytests.InsideArray");
};

class IndexInsideArrayCorrect : public ClientBase {
public:
    ~IndexInsideArrayCorrect() {
        _client.dropCollection(_nss);
    }
    void run() {
        _client.insert(_nss, fromjson("{'_id':1,a:[1]}"));
        _client.insert(_nss, fromjson("{'_id':2,a:[[1]]}"));
        ASSERT_OK(dbtests::createIndex(&_opCtx, _nss.ns_forTest(), BSON("a" << 1)));
        FindCommandRequest findRequest{_nss};
        findRequest.setFilter(fromjson("{a:[1]}"));
        findRequest.setHint(BSON("a" << 1));
        ASSERT_EQUALS(1, _client.find(std::move(findRequest))->next().getIntField("_id"));
    }

private:
    const NamespaceString _nss = NamespaceString::createNamespaceString_forTest(
        "unittests.querytests.IndexInsideArrayCorrect");
};

class SubobjArr : public ClientBase {
public:
    ~SubobjArr() {
        _client.dropCollection(_nss);
    }
    void run() {
        _client.insert(_nss, fromjson("{a:[{b:[1]}]}"));
        check("$natural");
        ASSERT_OK(dbtests::createIndex(&_opCtx, _nss.ns_forTest(), BSON("a" << 1)));
        check("a");
    }

private:
    void check(const string& hintField) {
        FindCommandRequest findRequest{_nss};
        findRequest.setFilter(fromjson("{'a.b':1}"));
        findRequest.setHint(BSON(hintField << 1));
        ASSERT(_client.find(findRequest)->more());
        findRequest.setFilter(fromjson("{'a.b':[1]}"));
        ASSERT(_client.find(findRequest)->more());
    }

    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("unittests.querytests.SubobjArr");
};

class MatchCodeCodeWScope : public ClientBase {
public:
    MatchCodeCodeWScope() {}
    ~MatchCodeCodeWScope() {
        _client.dropCollection(_nss);
    }
    void run() {
        checkMatch();
        ASSERT_OK(dbtests::createIndex(&_opCtx, _nss.ns_forTest(), BSON("a" << 1)));
        checkMatch();
    }

private:
    void checkMatch() {
        _client.remove(_nss, BSONObj());

        _client.insert(_nss, code());
        _client.insert(_nss, codeWScope());

        ASSERT_EQUALS(1U, _client.count(_nss, code()));
        ASSERT_EQUALS(1U, _client.count(_nss, codeWScope()));

        ASSERT_EQUALS(
            1U,
            _client.count(_nss,
                          BSON("a" << BSON("$type" << int(stdx::to_underlying(BSONType::code))))));
        ASSERT_EQUALS(
            1U,
            _client.count(
                _nss,
                BSON("a" << BSON("$type" << int(stdx::to_underlying(BSONType::codeWScope))))));
    }
    BSONObj code() const {
        BSONObjBuilder codeBuilder;
        codeBuilder.appendCode("a", "return 1;");
        return codeBuilder.obj();
    }
    BSONObj codeWScope() const {
        BSONObjBuilder codeWScopeBuilder;
        codeWScopeBuilder.appendCodeWScope("a", "return 1;", BSONObj());
        return codeWScopeBuilder.obj();
    }
    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("unittests.querytests.MatchCodeCodeWScope");
};

class MatchDBRefType : public ClientBase {
public:
    MatchDBRefType() {}
    ~MatchDBRefType() {
        _client.dropCollection(_nss);
    }
    void run() {
        checkMatch();
        ASSERT_OK(dbtests::createIndex(&_opCtx, _nss.ns_forTest(), BSON("a" << 1)));
        checkMatch();
    }

private:
    void checkMatch() {
        _client.remove(_nss, BSONObj());
        _client.insert(_nss, dbref());
        ASSERT_EQUALS(1U, _client.count(_nss, dbref()));
        ASSERT_EQUALS(
            1U,
            _client.count(_nss,
                          BSON("a" << BSON("$type" << int(stdx::to_underlying(BSONType::dbRef))))));
    }
    BSONObj dbref() const {
        BSONObjBuilder b;
        OID oid;
        b.appendDBRef("a", "ns", oid);
        return b.obj();
    }
    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("unittests.querytests.MatchDBRefType");
};

class DirectLocking : public ClientBase {
public:
    void run() {
        Lock::GlobalWrite lk(&_opCtx);
        _client.remove(NamespaceString::createNamespaceString_forTest("a.b"), BSONObj());
        auto db = getDbOrCreate(
            &_opCtx, NamespaceString::createNamespaceString_forTest("unittests.DirectLocking"));
        ASSERT_EQUALS("unittests", db->name().toString_forTest());
    }
    const char* ns;
};

class FastCountIn : public ClientBase {
public:
    ~FastCountIn() {
        _client.dropCollection(_nss);
    }
    void run() {
        _client.insert(_nss, BSON("i" << "a"));
        ASSERT_OK(dbtests::createIndex(&_opCtx, _nss.ns_forTest(), BSON("i" << 1)));
        ASSERT_EQUALS(1U, _client.count(_nss, fromjson("{i:{$in:['a']}}")));
    }

private:
    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("unittests.querytests.FastCountIn");
};

class EmbeddedArray : public ClientBase {
public:
    ~EmbeddedArray() {
        _client.dropCollection(_nss);
    }
    void run() {
        _client.insert(_nss, fromjson("{foo:{bar:['spam']}}"));
        _client.insert(_nss, fromjson("{foo:{bar:['spam','eggs']}}"));
        _client.insert(_nss, fromjson("{bar:['spam']}"));
        _client.insert(_nss, fromjson("{bar:['spam','eggs']}"));
        ASSERT_EQUALS(2U, _client.count(_nss, BSON("bar" << "spam")));
        ASSERT_EQUALS(2U, _client.count(_nss, BSON("foo.bar" << "spam")));
    }

private:
    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("unittests.querytests.EmbeddedArray");
};

class DifferentNumbers : public ClientBase {
public:
    ~DifferentNumbers() {
        _client.dropCollection(_nss);
    }
    void t(const NamespaceString& nss) {
        FindCommandRequest findRequest{nss};
        findRequest.setSort(BSON("7" << 1));
        std::unique_ptr<DBClientCursor> cursor = _client.find(std::move(findRequest));
        while (cursor->more()) {
            BSONObj o = cursor->next();
            MONGO_verify(validateBSON(o).isOK());
        }
    }
    void run() {
        {
            BSONObjBuilder b;
            b.append("7", (int)4);
            _client.insert(_nss, b.obj());
        }
        {
            BSONObjBuilder b;
            b.append("7", (long long)2);
            _client.insert(_nss, b.obj());
        }
        {
            BSONObjBuilder b;
            b.appendNull("7");
            _client.insert(_nss, b.obj());
        }
        {
            BSONObjBuilder b;
            b.append("7", "b");
            _client.insert(_nss, b.obj());
        }
        {
            BSONObjBuilder b;
            b.appendNull("8");
            _client.insert(_nss, b.obj());
        }
        {
            BSONObjBuilder b;
            b.append("7", (double)3.7);
            _client.insert(_nss, b.obj());
        }

        t(_nss);
        ASSERT_OK(dbtests::createIndex(&_opCtx, _nss.ns_forTest(), BSON("7" << 1)));
        t(_nss);
    }

private:
    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("unittests.querytests.DifferentNumbers");
};

class CollectionBase : public ClientBase {
public:
    CollectionBase(string leaf)
        : _nss(NamespaceString::createNamespaceString_forTest("unittests.querytests." + leaf)) {
        _client.dropCollection(nss());
    }

    virtual ~CollectionBase() {
        _client.dropCollection(nss());
    }

    int count() {
        return (int)_client.count(nss());
    }

    size_t numCursorsOpen() {
        return CursorManager::get(&_opCtx)->numCursors();
    }

    StringData ns() {
        return _nss.ns_forTest();
    }
    const NamespaceString& nss() {
        return _nss;
    }

private:
    const NamespaceString _nss;
};

class SymbolStringSame : public CollectionBase {
public:
    SymbolStringSame() : CollectionBase("symbolstringsame") {}

    void run() {
        {
            BSONObjBuilder b;
            b.appendSymbol("x", "eliot");
            b.append("z", 17);
            _client.insert(nss(), b.obj());
        }
        ASSERT_EQUALS(17, _client.findOne(nss(), BSONObj{})["z"].number());
        {
            BSONObjBuilder b;
            b.appendSymbol("x", "eliot");
            ASSERT_EQUALS(17, _client.findOne(nss(), b.obj())["z"].number());
        }
        ASSERT_EQUALS(17, _client.findOne(nss(), BSON("x" << "eliot"))["z"].number());
        ASSERT_OK(dbtests::createIndex(&_opCtx, ns(), BSON("x" << 1)));
        ASSERT_EQUALS(17, _client.findOne(nss(), BSON("x" << "eliot"))["z"].number());
    }
};

class TailableCappedRaceCondition : public CollectionBase {
public:
    TailableCappedRaceCondition() : CollectionBase("tailablecappedrace") {
        _client.dropCollection(nss());
        _n = 0;
    }
    void run() {
        // Skip the test if the storage engine doesn't support capped collections.
        if (!_opCtx.getServiceContext()->getStorageEngine()->supportsCappedCollections()) {
            return;
        }

        string err;
        dbtests::WriteContextForTests ctx(&_opCtx, ns());
        auto coll = ctx.getCollection();

        // note that extents are always at least 4KB now - so this will get rounded up
        // a bit.
        {
            WriteUnitOfWork wunit(&_opCtx);

            CollectionOptions collectionOptions = unittest::assertGet(
                CollectionOptions::parse(fromjson("{ capped : true, size : 2000, max: 10000 }"),
                                         CollectionOptions::parseForCommand));
            NamespaceString nss = NamespaceString::createNamespaceString_forTest(ns());
            ASSERT(ctx.db()->userCreateNS(&_opCtx, nss, collectionOptions, false).isOK());
            wunit.commit();
        }

        for (int i = 0; i < 200; i++) {
            insertNext();
            ASSERT(count() < 90);
        }

        int a = count();

        FindCommandRequest findRequest{nss()};
        findRequest.setFilter(BSON("i" << GT << 0));
        findRequest.setHint(BSON("$natural" << 1));
        findRequest.setTailable(true);
        std::unique_ptr<DBClientCursor> c = _client.find(std::move(findRequest));
        int n = 0;
        while (c->more()) {
            BSONObj z = c->next();
            n++;
        }

        ASSERT_EQUALS(a, n);

        insertNext();
        ASSERT(c->more());

        for (int i = 0; i < 90; i++) {
            insertNext();
        }

        ASSERT_THROWS(([&] {
                          while (c->more()) {
                              c->nextSafe();
                          }
                      }()),
                      AssertionException);
    }

    void insertNext() {
        BSONObjBuilder b;
        b.appendOID("_id", nullptr, true);
        b.append("i", _n++);
        insert(nss(), b.obj());
    }

    int _n;
};

class HelperTest : public CollectionBase {
public:
    HelperTest() : CollectionBase("helpertest") {}

    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns());

        for (int i = 0; i < 50; i++) {
            insert(nss(), BSON("_id" << i << "x" << i * 2));
        }

        ASSERT_EQUALS(50, count());

        BSONObj res;
        ASSERT(Helpers::findOne(&_opCtx, ctx.getCollection(), BSON("_id" << 20), res));
        ASSERT_EQUALS(40, res["x"].numberInt());

        ASSERT(Helpers::findById(&_opCtx, nss(), BSON("_id" << 20), res));
        ASSERT_EQUALS(40, res["x"].numberInt());

        ASSERT(!Helpers::findById(&_opCtx, nss(), BSON("_id" << 200), res));

        long long slow;
        long long fast;

        const int n = kDebugBuild ? 1000 : 10000;
        {
            Timer t;
            for (int i = 0; i < n; i++) {
                ASSERT(Helpers::findOne(&_opCtx, ctx.getCollection(), BSON("_id" << 20), res));
            }
            slow = t.micros();
        }
        {
            Timer t;
            for (int i = 0; i < n; i++) {
                ASSERT(Helpers::findById(&_opCtx, nss(), BSON("_id" << 20), res));
            }
            fast = t.micros();
        }

        std::cout << "HelperTest  slow:" << slow << " fast:" << fast << endl;
    }
};

class HelperByIdTest : public CollectionBase {
public:
    HelperByIdTest() : CollectionBase("helpertestbyid") {}

    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns());

        for (int i = 0; i < 1000; i++) {
            insert(nss(), BSON("_id" << i << "x" << i * 2));
        }
        for (int i = 0; i < 1000; i += 2) {
            _client.remove(nss(), BSON("_id" << i));
        }

        BSONObj res;
        for (int i = 0; i < 1000; i++) {
            bool found = Helpers::findById(&_opCtx, nss(), BSON("_id" << i), res);
            ASSERT_EQUALS(i % 2, int(found));
        }
    }
};

class ClientCursorTest : public CollectionBase {
    ClientCursorTest() : CollectionBase("clientcursortest") {}

    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns());

        for (int i = 0; i < 1000; i++) {
            insert(nss(), BSON("_id" << i << "x" << i * 2));
        }
    }
};

class FindingStart : public CollectionBase {
public:
    FindingStart() : CollectionBase("findingstart") {}
    static const char* ns() {
        return "local.oplog.querytests.findingstart";
    }

    void run() {
        // Skip the test if the storage engine doesn't support capped collections.
        if (!_opCtx.getServiceContext()->getStorageEngine()->supportsCappedCollections()) {
            return;
        }

        BSONObj info;
        _client.runCommand(DatabaseName::kLocal,
                           BSON("create" << "oplog.querytests.findingstart"
                                         << "capped" << true << "size" << 4096),
                           info);
        // WiredTiger storage engines forbid dropping of the oplog. Evergreen reuses nodes for
        // testing, so the oplog may already exist on the test node; in this case, trying to create
        // the oplog once again would fail.
        //
        // To ensure we are working with a clean oplog (an oplog without entries), we resort
        // to truncating the oplog instead.
        if (_opCtx.getServiceContext()->getStorageEngine()->supportsRecoveryTimestamp()) {
            deleteAll(_opCtx, NamespaceString::createNamespaceString_forTest(ns()));
        }

        unsigned i = 0;
        int max = 1;

        while (1) {
            int oldCount = count();
            insertOplogDocument(&_opCtx, Timestamp(1000, i++), ns());
            int newCount = count();
            if (oldCount == newCount || newCount < max)
                break;

            if (newCount > max)
                max = newCount;
        }

        for (int k = 0; k < 5; ++k) {
            auto ts = Timestamp(1000, i++);
            insertOplogDocument(&_opCtx, ts, ns());
            FindCommandRequest findRequest{NamespaceString::createNamespaceString_forTest(ns())};
            findRequest.setSort(BSON("$natural" << 1));
            unsigned min = _client.find(findRequest)->next()["ts"].timestamp().getInc();
            for (unsigned j = -1; j < i; ++j) {
                FindCommandRequest findRequestInner{
                    NamespaceString::createNamespaceString_forTest(ns())};
                findRequestInner.setFilter(BSON("ts" << GTE << Timestamp(1000, j)));
                std::unique_ptr<DBClientCursor> c = _client.find(findRequestInner);
                ASSERT(c->more());
                BSONObj next = c->next();
                ASSERT(!next["ts"].eoo());
                ASSERT_EQUALS((j > min ? j : min), next["ts"].timestamp().getInc());
            }
        }
        _client.dropCollection(nss());
    }
};

class FindingStartPartiallyFull : public CollectionBase {
public:
    FindingStartPartiallyFull() : CollectionBase("findingstart") {}
    static const char* ns() {
        return "local.oplog.querytests.findingstart";
    }

    void run() {
        // Skip the test if the storage engine doesn't support capped collections.
        if (!_opCtx.getServiceContext()->getStorageEngine()->supportsCappedCollections()) {
            return;
        }

        size_t startNumCursors = numCursorsOpen();

        BSONObj info;
        _client.runCommand(DatabaseName::kLocal,
                           BSON("create" << "oplog.querytests.findingstart"
                                         << "capped" << true << "size" << 4096),
                           info);
        // WiredTiger storage engines forbid dropping of the oplog. Evergreen reuses nodes for
        // testing, so the oplog may already exist on the test node; in this case, trying to create
        // the oplog once again would fail.
        //
        // To ensure we are working with a clean oplog (an oplog without entries), we resort
        // to truncating the oplog instead.
        if (_opCtx.getServiceContext()->getStorageEngine()->supportsRecoveryTimestamp()) {
            deleteAll(_opCtx, NamespaceString::createNamespaceString_forTest(ns()));
        }

        unsigned i = 0;
        for (; i < 150; insertOplogDocument(&_opCtx, Timestamp(1000, i++), ns()))
            ;

        for (int k = 0; k < 5; ++k) {
            insertOplogDocument(&_opCtx, Timestamp(1000, i++), ns());
            FindCommandRequest findRequest{NamespaceString::createNamespaceString_forTest(ns())};
            findRequest.setSort(BSON("$natural" << 1));
            unsigned min = _client.find(findRequest)->next()["ts"].timestamp().getInc();
            for (unsigned j = -1; j < i; ++j) {
                FindCommandRequest findRequestInner{
                    NamespaceString::createNamespaceString_forTest(ns())};
                findRequestInner.setFilter(BSON("ts" << GTE << Timestamp(1000, j)));
                std::unique_ptr<DBClientCursor> c = _client.find(findRequestInner);
                ASSERT(c->more());
                BSONObj next = c->next();
                ASSERT(!next["ts"].eoo());
                ASSERT_EQUALS((j > min ? j : min), next["ts"].timestamp().getInc());
            }
        }
        ASSERT_EQUALS(startNumCursors, numCursorsOpen());
        _client.dropCollection(nss());
    }
};

/**
 * Check oplog replay mode where query timestamp is earlier than the earliest
 * entry in the collection.
 */
class FindingStartStale : public CollectionBase {
public:
    FindingStartStale() : CollectionBase("findingstart") {}
    static const char* ns() {
        return "local.oplog.querytests.findingstart";
    }

    void run() {
        // Skip the test if the storage engine doesn't support capped collections.
        if (!_opCtx.getServiceContext()->getStorageEngine()->supportsCappedCollections()) {
            return;
        }

        size_t startNumCursors = numCursorsOpen();

        // Check oplog replay mode with missing collection.
        FindCommandRequest findRequestMissingColl{
            NamespaceString::createNamespaceString_forTest("local.oplog.querytests.missing")};
        findRequestMissingColl.setFilter(BSON("ts" << GTE << Timestamp(1000, 50)));
        std::unique_ptr<DBClientCursor> c0 = _client.find(std::move(findRequestMissingColl));
        ASSERT(!c0->more());

        BSONObj info;
        _client.runCommand(DatabaseName::kLocal,
                           BSON("create" << "oplog.querytests.findingstart"
                                         << "capped" << true << "size" << 4096),
                           info);
        // WiredTiger storage engines forbid dropping of the oplog. Evergreen reuses nodes for
        // testing, so the oplog may already exist on the test node; in this case, trying to create
        // the oplog once again would fail.
        //
        // To ensure we are working with a clean oplog (an oplog without entries), we resort
        // to truncating the oplog instead.
        if (_opCtx.getServiceContext()->getStorageEngine()->supportsRecoveryTimestamp()) {
            deleteAll(_opCtx, NamespaceString::createNamespaceString_forTest(ns()));
        }

        // Check oplog replay mode with empty collection.
        FindCommandRequest findRequest{NamespaceString::createNamespaceString_forTest(ns())};
        findRequest.setFilter(BSON("ts" << GTE << Timestamp(1000, 50)));
        std::unique_ptr<DBClientCursor> c = _client.find(findRequest);
        ASSERT(!c->more());

        // Check with some docs in the collection.
        for (int i = 100; i < 150; insertOplogDocument(&_opCtx, Timestamp(1000, i++), ns()))
            ;
        c = _client.find(findRequest);
        ASSERT(c->more());
        ASSERT_EQUALS(100u, c->next()["ts"].timestamp().getInc());

        // Check that no persistent cursors outlast our queries above.
        ASSERT_EQUALS(startNumCursors, numCursorsOpen());

        _client.dropCollection(nss());
    }
};

class WhatsMyUri : public CollectionBase {
public:
    WhatsMyUri() : CollectionBase("whatsmyuri") {}
    void run() {
        BSONObj result;
        _client.runCommand(DatabaseName::kAdmin, BSON("whatsmyuri" << 1), result);
        ASSERT_EQUALS("", result["you"].str());
    }
};

class WhatsMySni : public CollectionBase {
public:
    WhatsMySni() : CollectionBase("whatsmysni") {}
    void run() {
        BSONObj result;
        _client.runCommand(DatabaseName::kAdmin, BSON("whatsmysni" << 1), result);
        ASSERT_EQUALS("", result["sni"].str());
    }
};

class QueryByUuid : public CollectionBase {
public:
    QueryByUuid() : CollectionBase("QueryByUuid") {}

    void run() {
        CollectionOptions coll_opts;
        coll_opts.uuid = UUID::gen();
        {
            Lock::GlobalWrite lk(&_opCtx);
            auto db = getDbOrCreate(&_opCtx, nss());
            WriteUnitOfWork wunit(&_opCtx);
            db->createCollection(&_opCtx, nss(), coll_opts, false);
            wunit.commit();
        }
        insert(nss(), BSON("a" << 1));
        insert(nss(), BSON("a" << 2));
        insert(nss(), BSON("a" << 3));
        std::unique_ptr<DBClientCursor> cursor = _client.find(
            FindCommandRequest{NamespaceStringOrUUID{nss().dbName(), *coll_opts.uuid}});
        ASSERT_EQUALS(nss(), cursor->getNamespaceString());
        for (int i = 1; i <= 3; ++i) {
            ASSERT(cursor->more());
            BSONObj obj(cursor->next());
            ASSERT_EQUALS(obj["a"].Int(), i);
        }
        ASSERT(!cursor->more());
    }
};

class CountByUUID : public CollectionBase {
public:
    CountByUUID() : CollectionBase("CountByUUID") {}

    void run() {
        CollectionOptions coll_opts;
        coll_opts.uuid = UUID::gen();
        {
            Lock::GlobalWrite lk(&_opCtx);
            auto db = getDbOrCreate(&_opCtx, nss());
            WriteUnitOfWork wunit(&_opCtx);
            db->createCollection(&_opCtx, nss(), coll_opts, false);
            wunit.commit();
        }
        insert(nss(), BSON("a" << 1));

        auto count =
            _client.count(NamespaceStringOrUUID(nss().dbName(), *coll_opts.uuid), BSONObj());
        ASSERT_EQUALS(1U, count);

        insert(nss(), BSON("a" << 2));
        insert(nss(), BSON("a" << 3));

        count = _client.count(NamespaceStringOrUUID(nss().dbName(), *coll_opts.uuid), BSONObj());
        ASSERT_EQUALS(3U, count);
    }
};

class GetIndexSpecsByUUID : public CollectionBase {
public:
    GetIndexSpecsByUUID() : CollectionBase("GetIndexSpecsByUUID") {}

    void run() {
        CollectionOptions coll_opts;
        coll_opts.uuid = UUID::gen();
        {
            Lock::GlobalWrite lk(&_opCtx);
            auto db = getDbOrCreate(&_opCtx, nss());
            WriteUnitOfWork wunit(&_opCtx);
            db->createCollection(&_opCtx, nss(), coll_opts, true);
            wunit.commit();
        }
        insert(nss(), BSON("a" << 1));
        insert(nss(), BSON("a" << 2));
        insert(nss(), BSON("a" << 3));

        const bool includeBuildUUIDs = false;
        const int options = 0;

        auto specsWithIdIndexOnly = _client.getIndexSpecs(
            NamespaceStringOrUUID(nss().dbName(), *coll_opts.uuid), includeBuildUUIDs, options);
        ASSERT_EQUALS(1U, specsWithIdIndexOnly.size());

        ASSERT_OK(dbtests::createIndex(&_opCtx, ns(), BSON("a" << 1), true));

        auto specsWithBothIndexes = _client.getIndexSpecs(
            NamespaceStringOrUUID(nss().dbName(), *coll_opts.uuid), includeBuildUUIDs, options);
        ASSERT_EQUALS(2U, specsWithBothIndexes.size());
    }
};

class GetDatabaseInfosTest : public CollectionBase {
public:
    GetDatabaseInfosTest() : CollectionBase("GetDatabaseInfosTest") {}

    void run() {
        const auto nss1 =
            NamespaceString::createNamespaceString_forTest("unittestsdb1.querytests.coll1");
        {
            Lock::GlobalWrite lk(&_opCtx);
            auto db = getDbOrCreate(&_opCtx, nss1);

            WriteUnitOfWork wunit(&_opCtx);
            db->createCollection(&_opCtx, nss1);
            wunit.commit();
        }
        insert(nss1, BSON("a" << 1));
        auto dbInfos = _client.getDatabaseInfos(BSONObj(), true /*nameOnly*/);
        checkNewDBInResults(dbInfos, 1);


        const auto nss2 =
            NamespaceString::createNamespaceString_forTest("unittestsdb2.querytests.coll2");
        {
            Lock::GlobalWrite lk(&_opCtx);
            auto db = getDbOrCreate(&_opCtx, nss2);
            WriteUnitOfWork wunit(&_opCtx);
            db->createCollection(&_opCtx, nss2);
            wunit.commit();
        }
        insert(nss2, BSON("b" << 2));
        dbInfos = _client.getDatabaseInfos(BSONObj(), true /*nameOnly*/);
        checkNewDBInResults(dbInfos, 2);


        const auto nss3 =
            NamespaceString::createNamespaceString_forTest("unittestsdb3.querytests.coll3");
        {
            Lock::GlobalWrite lk(&_opCtx);
            auto db = getDbOrCreate(&_opCtx, nss3);
            WriteUnitOfWork wunit(&_opCtx);
            db->createCollection(&_opCtx, nss3);
            wunit.commit();
        }
        insert(nss3, BSON("c" << 3));
        dbInfos = _client.getDatabaseInfos(BSONObj(), true /*nameOnly*/);
        checkNewDBInResults(dbInfos, 3);
    }

    void checkNewDBInResults(const std::vector<BSONObj> results, const int dbNum) {
        std::string target = "unittestsdb" + std::to_string(dbNum);
        for (const auto& res : results) {
            if (res["name"].str() == target) {
                return;
            }
        }
        ASSERT(false);  // Should not hit this unless we failed to find the database.
    }
};

class CollectionInternalBase : public CollectionBase {
public:
    CollectionInternalBase(const char* nsLeaf)
        : CollectionBase(nsLeaf),
          _autodb(
              &_opCtx, DatabaseName::createDatabaseName_forTest(boost::none, "unittests"), MODE_X) {
    }

private:
    AutoGetDb _autodb;
};

class QueryReadsAll : public CollectionBase {
public:
    QueryReadsAll() : CollectionBase("queryreadsall") {}
    void run() {
        for (int i = 0; i < 5; ++i) {
            insert(nss(), BSONObj());
        }
        {
            // With five results and a batch size of 5, a cursor is created since we don't know
            // there are no more results.
            FindCommandRequest findRequest{NamespaceString::createNamespaceString_forTest(ns())};
            findRequest.setBatchSize(5);
            std::unique_ptr<DBClientCursor> c = _client.find(std::move(findRequest));
            ASSERT(c->more());
            ASSERT_NE(0, c->getCursorId());
            for (int i = 0; i < 5; ++i) {
                ASSERT(c->more());
                c->next();
            }
            ASSERT(!c->more());
        }
        {
            // With a batchsize of 6 we know there are no more results so we don't create a
            // cursor.
            FindCommandRequest findRequest{NamespaceString::createNamespaceString_forTest(ns())};
            findRequest.setBatchSize(6);
            std::unique_ptr<DBClientCursor> c = _client.find(std::move(findRequest));
            ASSERT(c->more());
            ASSERT_EQ(0, c->getCursorId());
        }
    }
};

class OrderingTest {
public:
    void run() {
        {
            Ordering o = Ordering::make(BSON("a" << 1 << "b" << -1 << "c" << 1));
            ASSERT_EQUALS(1, o.get(0));
            ASSERT_EQUALS(-1, o.get(1));
            ASSERT_EQUALS(1, o.get(2));

            ASSERT(!o.descending(1));
            ASSERT(o.descending(1 << 1));
            ASSERT(!o.descending(1 << 2));
        }

        {
            Ordering o = Ordering::make(BSON("a.d" << 1 << "a" << 1 << "e" << -1));
            ASSERT_EQUALS(1, o.get(0));
            ASSERT_EQUALS(1, o.get(1));
            ASSERT_EQUALS(-1, o.get(2));

            ASSERT(!o.descending(1));
            ASSERT(!o.descending(1 << 1));
            ASSERT(o.descending(1 << 2));
        }
    }
};

class All : public unittest::OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("query") {}

    void setupTests() override {
        add<FindingStart>();
        add<FindOneOr>();
        add<FindOneEmptyObj>();
        add<BoundedKey>();
        add<GetMore>();
        add<GetMoreKillOp>();
        add<PositiveLimit>();
        add<TailNotAtEnd>();
        add<EmptyTail>();
        add<TailableDelete>();
        add<TailableDelete2>();
        add<TailableInsertDelete>();
        add<TailCappedOnly>();
        add<TailableQueryOnId>();
        add<OplogScanWithGtTimstampPred>();
        add<OplogScanGtTsExplain>();
        add<ArrayId>();
        add<UnderscoreNs>();
        add<EmptyFieldSpec>();
        add<MultiNe>();
        add<EmbeddedNe>();
        add<EmbeddedNumericTypes>();
        add<AutoResetIndexCache>();
        add<UniqueIndex>();
        add<UniqueIndexPreexistingData>();
        add<SubobjectInArray>();
        add<Size>();
        add<FullArray>();
        add<InsideArray>();
        add<IndexInsideArrayCorrect>();
        add<SubobjArr>();
        add<MatchCodeCodeWScope>();
        add<MatchDBRefType>();
        add<DirectLocking>();
        add<FastCountIn>();
        add<EmbeddedArray>();
        add<DifferentNumbers>();
        add<SymbolStringSame>();
        add<TailableCappedRaceCondition>();
        add<HelperTest>();
        add<HelperByIdTest>();
        add<FindingStartPartiallyFull>();
        add<FindingStartStale>();
        add<WhatsMyUri>();
        add<QueryByUuid>();
        add<GetIndexSpecsByUUID>();
        add<CountByUUID>();
        add<GetDatabaseInfosTest>();
        add<QueryReadsAll>();
        add<OrderingTest>();
        add<WhatsMySni>();
    }
};

unittest::OldStyleSuiteInitializer<All> myall;

}  // namespace
}  // namespace mongo
