/**
 *    Copyright (C) 2008 10gen Inc.
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

#include <boost/optional.hpp>
#include <iostream>

#include "mongo/client/dbclientcursor.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/queued_data_stage.h"
#include "mongo/db/json.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/find.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/util/timer.h"

namespace {
namespace QueryTests {

using std::unique_ptr;
using std::endl;
using std::string;
using std::vector;

class Base {
public:
    Base() : _lk(&_opCtx), _context(&_opCtx, ns()) {
        {
            WriteUnitOfWork wunit(&_opCtx);
            _database = _context.db();
            _collection = _database->getCollection(&_opCtx, ns());
            if (_collection) {
                _database->dropCollection(&_opCtx, ns()).transitional_ignore();
            }
            _collection = _database->createCollection(&_opCtx, ns());
            wunit.commit();
        }

        addIndex(IndexSpec().addKey("a").unique(false));
    }

    ~Base() {
        try {
            WriteUnitOfWork wunit(&_opCtx);
            uassertStatusOK(_database->dropCollection(&_opCtx, ns()));
            wunit.commit();
        } catch (...) {
            FAIL("Exception while cleaning up collection");
        }
    }

protected:
    static const char* ns() {
        return "unittests.querytests";
    }

    void addIndex(const IndexSpec& spec) {
        DBDirectClient client(&_opCtx);
        client.createIndex(ns(), spec);
        client.getLastError();
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
            _collection->insertDocument(&_opCtx, InsertStatement(b.obj()), nullOpDebug, false)
                .transitional_ignore();
        } else {
            _collection->insertDocument(&_opCtx, InsertStatement(o), nullOpDebug, false)
                .transitional_ignore();
        }
        wunit.commit();
    }


    const ServiceContext::UniqueOperationContext _opCtxPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_opCtxPtr;
    Lock::GlobalWrite _lk;
    OldClientContext _context;

    Database* _database;
    Collection* _collection;
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
        ASSERT(Helpers::findOne(&_opCtx, _collection, query, ret, true));
        ASSERT_EQUALS(string("b"), ret.firstElement().fieldName());
        // Cross check with findOne() returning location.
        ASSERT_BSONOBJ_EQ(
            ret,
            _collection->docFor(&_opCtx, Helpers::findOne(&_opCtx, _collection, query, true))
                .value());
    }
};

class FindOneRequireIndex : public Base {
public:
    void run() {
        insert(BSON("b" << 2 << "_id" << 0));
        BSONObj query = fromjson("{b:2}");
        BSONObj ret;

        // Check findOne() returning object, allowing unindexed scan.
        ASSERT(Helpers::findOne(&_opCtx, _collection, query, ret, false));
        // Check findOne() returning location, allowing unindexed scan.
        ASSERT_BSONOBJ_EQ(
            ret,
            _collection->docFor(&_opCtx, Helpers::findOne(&_opCtx, _collection, query, false))
                .value());

        // Check findOne() returning object, requiring indexed scan without index.
        ASSERT_THROWS(Helpers::findOne(&_opCtx, _collection, query, ret, true), UserException);
        // Check findOne() returning location, requiring indexed scan without index.
        ASSERT_THROWS(Helpers::findOne(&_opCtx, _collection, query, true), UserException);

        addIndex(IndexSpec().addKey("b").unique(false));

        // Check findOne() returning object, requiring indexed scan with index.
        ASSERT(Helpers::findOne(&_opCtx, _collection, query, ret, true));
        // Check findOne() returning location, requiring indexed scan with index.
        ASSERT_BSONOBJ_EQ(
            ret,
            _collection->docFor(&_opCtx, Helpers::findOne(&_opCtx, _collection, query, true))
                .value());
    }
};

class FindOneEmptyObj : public Base {
public:
    void run() {
        // We don't normally allow empty objects in the database, but test that we can find
        // an empty object (one might be allowed inside a reserved namespace at some point).
        Lock::GlobalWrite lk(&_opCtx);
        OldClientContext ctx(&_opCtx, "unittests.querytests");

        {
            WriteUnitOfWork wunit(&_opCtx);
            Database* db = ctx.db();
            if (db->getCollection(&_opCtx, ns())) {
                _collection = NULL;
                db->dropCollection(&_opCtx, ns()).transitional_ignore();
            }
            _collection = db->createCollection(&_opCtx, ns(), CollectionOptions(), false);
            wunit.commit();
        }
        ASSERT(_collection);

        DBDirectClient cl(&_opCtx);
        BSONObj info;
        bool ok = cl.runCommand("unittests",
                                BSON("godinsert"
                                     << "querytests"
                                     << "obj"
                                     << BSONObj()),
                                info);
        ASSERT(ok);

        insert(BSONObj());
        BSONObj query;
        BSONObj ret;
        ASSERT(Helpers::findOne(&_opCtx, _collection, query, ret, false));
        ASSERT(ret.isEmpty());
        ASSERT_BSONOBJ_EQ(
            ret,
            _collection->docFor(&_opCtx, Helpers::findOne(&_opCtx, _collection, query, false))
                .value());
    }
};

class ClientBase {
public:
    ClientBase() : _client(&_opCtx) {
        mongo::LastError::get(_opCtx.getClient()).reset();
    }
    virtual ~ClientBase() {
        mongo::LastError::get(_opCtx.getClient()).reset();
    }

protected:
    void insert(const char* ns, BSONObj o) {
        _client.insert(ns, o);
    }
    void update(const char* ns, BSONObj q, BSONObj o, bool upsert = 0) {
        _client.update(ns, Query(q), o, upsert);
    }
    bool error() {
        return !_client.getPrevError().getField("err").isNull();
    }

    const ServiceContext::UniqueOperationContext _opCtxPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_opCtxPtr;
    DBDirectClient _client;
};

class BoundedKey : public ClientBase {
public:
    ~BoundedKey() {
        _client.dropCollection("unittests.querytests.BoundedKey");
    }
    void run() {
        const char* ns = "unittests.querytests.BoundedKey";
        insert(ns, BSON("a" << 1));
        BSONObjBuilder a;
        a.appendMaxKey("$lt");
        BSONObj limit = a.done();
        ASSERT(!_client.findOne(ns, QUERY("a" << limit)).isEmpty());
        ASSERT_OK(dbtests::createIndex(&_opCtx, ns, BSON("a" << 1)));
        ASSERT(!_client.findOne(ns, QUERY("a" << limit).hint(BSON("a" << 1))).isEmpty());
    }
};

class GetMore : public ClientBase {
public:
    ~GetMore() {
        _client.dropCollection("unittests.querytests.GetMore");
    }
    void run() {
        const char* ns = "unittests.querytests.GetMore";
        insert(ns, BSON("a" << 1));
        insert(ns, BSON("a" << 2));
        insert(ns, BSON("a" << 3));
        unique_ptr<DBClientCursor> cursor = _client.query(ns, BSONObj(), 2);
        long long cursorId = cursor->getCursorId();
        cursor->decouple();
        cursor.reset();

        {
            // Check internal server handoff to getmore.
            OldClientWriteContext ctx(&_opCtx, ns);
            auto pinnedCursor = unittest::assertGet(
                ctx.getCollection()->getCursorManager()->pinCursor(&_opCtx, cursorId));
            ASSERT_EQUALS(2, pinnedCursor.getCursor()->pos());
        }

        cursor = _client.getMore(ns, cursorId);
        ASSERT(cursor->more());
        ASSERT_EQUALS(3, cursor->next().getIntField("a"));
    }
};

/**
 * Setting killAllOperations causes further getmores to fail.
 */
class GetMoreKillOp : public ClientBase {
public:
    ~GetMoreKillOp() {
        getGlobalServiceContext()->unsetKillAllOperations();
        _client.dropCollection("unittests.querytests.GetMoreKillOp");
    }
    void run() {
        // Create a collection with some data.
        const char* ns = "unittests.querytests.GetMoreKillOp";
        for (int i = 0; i < 1000; ++i) {
            insert(ns, BSON("a" << i));
        }

        // Create a cursor on the collection, with a batch size of 200.
        unique_ptr<DBClientCursor> cursor = _client.query(ns, "", 0, 0, 0, 0, 200);

        // Count 500 results, spanning a few batches of documents.
        for (int i = 0; i < 500; ++i) {
            ASSERT(cursor->more());
            cursor->next();
        }

        // Set the killop kill all flag, forcing the next get more to fail with a kill op
        // exception.
        getGlobalServiceContext()->setKillAllOperations();
        ASSERT_THROWS_CODE(([&] {
                               while (cursor->more()) {
                                   cursor->next();
                               }
                           }()),
                           UserException,
                           ErrorCodes::InterruptedAtShutdown);

        // Revert the killop kill all flag.
        getGlobalServiceContext()->unsetKillAllOperations();
    }
};

/**
 * A get more exception caused by an invalid or unauthorized get more request does not cause
 * the get more's ClientCursor to be destroyed.  This prevents an unauthorized user from
 * improperly killing a cursor by issuing an invalid get more request.
 */
class GetMoreInvalidRequest : public ClientBase {
public:
    ~GetMoreInvalidRequest() {
        getGlobalServiceContext()->unsetKillAllOperations();
        _client.dropCollection("unittests.querytests.GetMoreInvalidRequest");
    }
    void run() {
        // Create a collection with some data.
        const char* ns = "unittests.querytests.GetMoreInvalidRequest";
        for (int i = 0; i < 1000; ++i) {
            insert(ns, BSON("a" << i));
        }

        // Create a cursor on the collection, with a batch size of 200.
        unique_ptr<DBClientCursor> cursor = _client.query(ns, "", 0, 0, 0, 0, 200);
        CursorId cursorId = cursor->getCursorId();

        // Count 500 results, spanning a few batches of documents.
        int count = 0;
        for (int i = 0; i < 500; ++i) {
            ASSERT(cursor->more());
            cursor->next();
            ++count;
        }

        // Send a get more with a namespace that is incorrect ('spoofed') for this cursor id.
        // This is the invalaid get more request described in the comment preceding this class.
        ASSERT_THROWS(
            _client.getMore("unittests.querytests.GetMoreInvalidRequest_WRONG_NAMESPACE_FOR_CURSOR",
                            cursor->getCursorId()),
            UserException);

        // Check that the cursor still exists
        {
            AutoGetCollectionForReadCommand ctx(&_opCtx, NamespaceString(ns));
            ASSERT(1 == ctx.getCollection()->getCursorManager()->numCursors());
            ASSERT_OK(
                ctx.getCollection()->getCursorManager()->pinCursor(&_opCtx, cursorId).getStatus());
        }

        // Check that the cursor can be iterated until all documents are returned.
        while (cursor->more()) {
            cursor->next();
            ++count;
        }
        ASSERT_EQUALS(1000, count);
    }
};

class PositiveLimit : public ClientBase {
public:
    const char* ns;
    PositiveLimit() : ns("unittests.querytests.PositiveLimit") {}
    ~PositiveLimit() {
        _client.dropCollection(ns);
    }

    void testLimit(int limit) {
        ASSERT_EQUALS(_client.query(ns, BSONObj(), limit)->itcount(), limit);
    }
    void run() {
        for (int i = 0; i < 1000; i++)
            insert(ns, BSON(GENOID << "i" << i));

        ASSERT_EQUALS(_client.query(ns, BSONObj(), 1)->itcount(), 1);
        ASSERT_EQUALS(_client.query(ns, BSONObj(), 10)->itcount(), 10);
        ASSERT_EQUALS(_client.query(ns, BSONObj(), 101)->itcount(), 101);
        ASSERT_EQUALS(_client.query(ns, BSONObj(), 999)->itcount(), 999);
        ASSERT_EQUALS(_client.query(ns, BSONObj(), 1000)->itcount(), 1000);
        ASSERT_EQUALS(_client.query(ns, BSONObj(), 1001)->itcount(), 1000);
        ASSERT_EQUALS(_client.query(ns, BSONObj(), 0)->itcount(), 1000);
    }
};

class TailNotAtEnd : public ClientBase {
public:
    ~TailNotAtEnd() {
        _client.dropCollection("unittests.querytests.TailNotAtEnd");
    }
    void run() {
        const char* ns = "unittests.querytests.TailNotAtEnd";
        _client.createCollection(ns, 2047, true);
        insert(ns, BSON("a" << 0));
        insert(ns, BSON("a" << 1));
        insert(ns, BSON("a" << 2));
        unique_ptr<DBClientCursor> c = _client.query(
            ns, Query().hint(BSON("$natural" << 1)), 2, 0, 0, QueryOption_CursorTailable);
        ASSERT(0 != c->getCursorId());
        while (c->more())
            c->next();
        ASSERT(0 != c->getCursorId());
        insert(ns, BSON("a" << 3));
        insert(ns, BSON("a" << 4));
        insert(ns, BSON("a" << 5));
        insert(ns, BSON("a" << 6));
        ASSERT(c->more());
        ASSERT_EQUALS(3, c->next().getIntField("a"));
    }
};

class EmptyTail : public ClientBase {
public:
    ~EmptyTail() {
        _client.dropCollection("unittests.querytests.EmptyTail");
    }
    void run() {
        const char* ns = "unittests.querytests.EmptyTail";
        _client.createCollection(ns, 1900, true);
        unique_ptr<DBClientCursor> c = _client.query(
            ns, Query().hint(BSON("$natural" << 1)), 2, 0, 0, QueryOption_CursorTailable);
        ASSERT_EQUALS(0, c->getCursorId());
        ASSERT(c->isDead());
        insert(ns, BSON("a" << 0));
        c = _client.query(
            ns, QUERY("a" << 1).hint(BSON("$natural" << 1)), 2, 0, 0, QueryOption_CursorTailable);
        ASSERT(0 != c->getCursorId());
        ASSERT(!c->isDead());
    }
};

class TailableDelete : public ClientBase {
public:
    ~TailableDelete() {
        _client.dropCollection("unittests.querytests.TailableDelete");
    }
    void run() {
        const char* ns = "unittests.querytests.TailableDelete";
        _client.createCollection(ns, 8192, true, 2);
        insert(ns, BSON("a" << 0));
        insert(ns, BSON("a" << 1));
        unique_ptr<DBClientCursor> c = _client.query(
            ns, Query().hint(BSON("$natural" << 1)), 2, 0, 0, QueryOption_CursorTailable);
        c->next();
        c->next();
        ASSERT(!c->more());
        insert(ns, BSON("a" << 2));
        insert(ns, BSON("a" << 3));

        // We have overwritten the previous cursor position and should encounter a dead cursor.
        ASSERT_THROWS(c->more() ? c->nextSafe() : BSONObj(), AssertionException);
    }
};

class TailableDelete2 : public ClientBase {
public:
    ~TailableDelete2() {
        _client.dropCollection("unittests.querytests.TailableDelete");
    }
    void run() {
        const char* ns = "unittests.querytests.TailableDelete";
        _client.createCollection(ns, 8192, true, 2);
        insert(ns, BSON("a" << 0));
        insert(ns, BSON("a" << 1));
        unique_ptr<DBClientCursor> c = _client.query(
            ns, Query().hint(BSON("$natural" << 1)), 2, 0, 0, QueryOption_CursorTailable);
        c->next();
        c->next();
        ASSERT(!c->more());
        insert(ns, BSON("a" << 2));
        insert(ns, BSON("a" << 3));
        insert(ns, BSON("a" << 4));

        // We have overwritten the previous cursor position and should encounter a dead cursor.
        ASSERT_THROWS(c->more() ? c->nextSafe() : BSONObj(), AssertionException);
    }
};


class TailableInsertDelete : public ClientBase {
public:
    ~TailableInsertDelete() {
        _client.dropCollection("unittests.querytests.TailableInsertDelete");
    }
    void run() {
        const char* ns = "unittests.querytests.TailableInsertDelete";
        _client.createCollection(ns, 1330, true);
        insert(ns, BSON("a" << 0));
        insert(ns, BSON("a" << 1));
        unique_ptr<DBClientCursor> c = _client.query(
            ns, Query().hint(BSON("$natural" << 1)), 2, 0, 0, QueryOption_CursorTailable);
        c->next();
        c->next();
        ASSERT(!c->more());
        insert(ns, BSON("a" << 2));
        _client.remove(ns, QUERY("a" << 1));
        ASSERT(c->more());
        ASSERT_EQUALS(2, c->next().getIntField("a"));
        ASSERT(!c->more());
    }
};

class TailCappedOnly : public ClientBase {
public:
    ~TailCappedOnly() {
        _client.dropCollection("unittest.querytests.TailCappedOnly");
    }
    void run() {
        const char* ns = "unittests.querytests.TailCappedOnly";
        _client.insert(ns, BSONObj());
        ASSERT_THROWS(_client.query(ns, BSONObj(), 0, 0, 0, QueryOption_CursorTailable),
                      UserException);
    }
};

class TailableQueryOnId : public ClientBase {
public:
    ~TailableQueryOnId() {
        _client.dropCollection("unittests.querytests.TailableQueryOnId");
    }

    void insertA(const char* ns, int a) {
        BSONObjBuilder b;
        b.appendOID("_id", 0, true);
        b.appendOID("value", 0, true);
        b.append("a", a);
        insert(ns, b.obj());
    }

    void run() {
        const char* ns = "unittests.querytests.TailableQueryOnId";
        BSONObj info;
        _client.runCommand("unittests",
                           BSON("create"
                                << "querytests.TailableQueryOnId"
                                << "capped"
                                << true
                                << "size"
                                << 8192
                                << "autoIndexId"
                                << true),
                           info);
        insertA(ns, 0);
        insertA(ns, 1);
        unique_ptr<DBClientCursor> c1 =
            _client.query(ns, QUERY("a" << GT << -1), 0, 0, 0, QueryOption_CursorTailable);
        OID id;
        id.init("000000000000000000000000");
        unique_ptr<DBClientCursor> c2 =
            _client.query(ns, QUERY("value" << GT << id), 0, 0, 0, QueryOption_CursorTailable);
        c1->next();
        c1->next();
        ASSERT(!c1->more());
        c2->next();
        c2->next();
        ASSERT(!c2->more());
        insertA(ns, 2);
        ASSERT(c1->more());
        ASSERT_EQUALS(2, c1->next().getIntField("a"));
        ASSERT(!c1->more());
        ASSERT(c2->more());
        ASSERT_EQUALS(2, c2->next().getIntField("a"));  // SERVER-645
        ASSERT(!c2->more());
        ASSERT(!c2->isDead());
    }
};

class OplogReplayMode : public ClientBase {
public:
    ~OplogReplayMode() {
        _client.dropCollection("unittests.querytests.OplogReplayMode");
    }
    void run() {
        const char* ns = "unittests.querytests.OplogReplayMode";

        // Create a capped collection of size 10.
        _client.dropCollection(ns);
        _client.createCollection(ns, 10, true);

        insert(ns, BSON("ts" << 0));
        insert(ns, BSON("ts" << 1));
        insert(ns, BSON("ts" << 2));
        unique_ptr<DBClientCursor> c =
            _client.query(ns,
                          QUERY("ts" << GT << 1).hint(BSON("$natural" << 1)),
                          0,
                          0,
                          0,
                          QueryOption_OplogReplay);
        ASSERT(c->more());
        ASSERT_EQUALS(2, c->next().getIntField("ts"));
        ASSERT(!c->more());

        insert(ns, BSON("ts" << 3));
        c = _client.query(ns,
                          QUERY("ts" << GT << 1).hint(BSON("$natural" << 1)),
                          0,
                          0,
                          0,
                          QueryOption_OplogReplay);
        ASSERT(c->more());
        ASSERT_EQUALS(2, c->next().getIntField("ts"));
        ASSERT(c->more());
    }
};

class OplogReplaySlaveReadTill : public ClientBase {
public:
    ~OplogReplaySlaveReadTill() {
        _client.dropCollection("unittests.querytests.OplogReplaySlaveReadTill");
    }
    void run() {
        const char* ns = "unittests.querytests.OplogReplaySlaveReadTill";

        // Create a capped collection of size 10.
        _client.dropCollection(ns);
        _client.createCollection(ns, 10, true);

        Lock::DBLock lk(&_opCtx, "unittests", MODE_X);
        OldClientContext ctx(&_opCtx, ns);

        BSONObj info;
        _client.runCommand("unittests",
                           BSON("create"
                                << "querytests.OplogReplaySlaveReadTill"
                                << "capped"
                                << true
                                << "size"
                                << 8192),
                           info);

        Date_t one = Date_t::fromMillisSinceEpoch(
            LogicalClock::get(&_opCtx)->reserveTicks(1).asTimestamp().asLL());
        Date_t two = Date_t::fromMillisSinceEpoch(
            LogicalClock::get(&_opCtx)->reserveTicks(1).asTimestamp().asLL());
        Date_t three = Date_t::fromMillisSinceEpoch(
            LogicalClock::get(&_opCtx)->reserveTicks(1).asTimestamp().asLL());
        insert(ns, BSON("ts" << one));
        insert(ns, BSON("ts" << two));
        insert(ns, BSON("ts" << three));
        unique_ptr<DBClientCursor> c =
            _client.query(ns,
                          QUERY("ts" << GTE << two).hint(BSON("$natural" << 1)),
                          0,
                          0,
                          0,
                          QueryOption_OplogReplay | QueryOption_CursorTailable |
                              DBClientCursor::QueryOptionLocal_forceOpQuery);
        ASSERT(c->more());
        ASSERT_EQUALS(two, c->next()["ts"].Date());
        long long cursorId = c->getCursorId();

        auto pinnedCursor = unittest::assertGet(
            ctx.db()->getCollection(&_opCtx, ns)->getCursorManager()->pinCursor(&_opCtx, cursorId));
        ASSERT_EQUALS(three.toULL(), pinnedCursor.getCursor()->getSlaveReadTill().asULL());
    }
};

class OplogReplayExplain : public ClientBase {
public:
    ~OplogReplayExplain() {
        _client.dropCollection("unittests.querytests.OplogReplayExplain");
    }
    void run() {
        const char* ns = "unittests.querytests.OplogReplayExplain";

        // Create a capped collection of size 10.
        _client.dropCollection(ns);
        _client.createCollection(ns, 10, true);

        insert(ns, BSON("ts" << 0));
        insert(ns, BSON("ts" << 1));
        insert(ns, BSON("ts" << 2));
        unique_ptr<DBClientCursor> c =
            _client.query(ns,
                          QUERY("ts" << GT << 1).hint(BSON("$natural" << 1)).explain(),
                          0,
                          0,
                          0,
                          QueryOption_OplogReplay);
        ASSERT(c->more());

        // Check number of results and filterSet flag in explain.
        // filterSet is not available in oplog replay mode.
        BSONObj explainObj = c->next();
        ASSERT(explainObj.hasField("executionStats"));
        BSONObj execStats = explainObj["executionStats"].Obj();
        ASSERT_EQUALS(1, execStats.getIntField("nReturned"));

        ASSERT(!c->more());
    }
};

class BasicCount : public ClientBase {
public:
    ~BasicCount() {
        _client.dropCollection("unittests.querytests.BasicCount");
    }
    void run() {
        const char* ns = "unittests.querytests.BasicCount";
        ASSERT_OK(dbtests::createIndex(&_opCtx, ns, BSON("a" << 1)));
        count(0);
        insert(ns, BSON("a" << 3));
        count(0);
        insert(ns, BSON("a" << 4));
        count(1);
        insert(ns, BSON("a" << 5));
        count(1);
        insert(ns, BSON("a" << 4));
        count(2);
    }

private:
    void count(unsigned long long c) {
        ASSERT_EQUALS(c, _client.count("unittests.querytests.BasicCount", BSON("a" << 4)));
    }
};

class ArrayId : public ClientBase {
public:
    ~ArrayId() {
        _client.dropCollection("unittests.querytests.ArrayId");
    }
    void run() {
        const char* ns = "unittests.querytests.ArrayId";
        ASSERT_OK(dbtests::createIndex(&_opCtx, ns, BSON("_id" << 1)));
        ASSERT(!error());
        _client.insert(ns, fromjson("{'_id':[1,2]}"));
        ASSERT(error());
    }
};

class UnderscoreNs : public ClientBase {
public:
    ~UnderscoreNs() {
        _client.dropCollection("unittests.querytests._UnderscoreNs");
    }
    void run() {
        ASSERT(!error());
        const char* ns = "unittests.querytests._UnderscoreNs";
        ASSERT(_client.findOne(ns, "{}").isEmpty());
        _client.insert(ns, BSON("a" << 1));
        ASSERT_EQUALS(1, _client.findOne(ns, "{}").getIntField("a"));
        ASSERT(!error());
    }
};

class EmptyFieldSpec : public ClientBase {
public:
    ~EmptyFieldSpec() {
        _client.dropCollection("unittests.querytests.EmptyFieldSpec");
    }
    void run() {
        const char* ns = "unittests.querytests.EmptyFieldSpec";
        _client.insert(ns, BSON("a" << 1));
        ASSERT(!_client.findOne(ns, "").isEmpty());
        BSONObj empty;
        ASSERT(!_client.findOne(ns, "", &empty).isEmpty());
    }
};

class MultiNe : public ClientBase {
public:
    ~MultiNe() {
        _client.dropCollection("unittests.querytests.Ne");
    }
    void run() {
        const char* ns = "unittests.querytests.Ne";
        _client.insert(ns, fromjson("{a:[1,2]}"));
        ASSERT(_client.findOne(ns, fromjson("{a:{$ne:1}}")).isEmpty());
        BSONObj spec = fromjson("{a:{$ne:1,$ne:2}}");
        ASSERT(_client.findOne(ns, spec).isEmpty());
    }
};

class EmbeddedNe : public ClientBase {
public:
    ~EmbeddedNe() {
        _client.dropCollection("unittests.querytests.NestedNe");
    }
    void run() {
        const char* ns = "unittests.querytests.NestedNe";
        _client.insert(ns, fromjson("{a:[{b:1},{b:2}]}"));
        ASSERT(_client.findOne(ns, fromjson("{'a.b':{$ne:1}}")).isEmpty());
    }
};

class EmbeddedNumericTypes : public ClientBase {
public:
    ~EmbeddedNumericTypes() {
        _client.dropCollection("unittests.querytests.NumericEmbedded");
    }
    void run() {
        const char* ns = "unittests.querytests.NumericEmbedded";
        _client.insert(ns, BSON("a" << BSON("b" << 1)));
        ASSERT(!_client.findOne(ns, BSON("a" << BSON("b" << 1.0))).isEmpty());
        ASSERT_OK(dbtests::createIndex(&_opCtx, ns, BSON("a" << 1)));
        ASSERT(!_client.findOne(ns, BSON("a" << BSON("b" << 1.0))).isEmpty());
    }
};

class AutoResetIndexCache : public ClientBase {
public:
    ~AutoResetIndexCache() {
        _client.dropCollection("unittests.querytests.AutoResetIndexCache");
    }
    static const char* ns() {
        return "unittests.querytests.AutoResetIndexCache";
    }
    void index() {
        ASSERT_EQUALS(2u, _client.getIndexSpecs(ns()).size());
    }
    void noIndex() {
        ASSERT_EQUALS(0u, _client.getIndexSpecs(ns()).size());
    }
    void checkIndex() {
        ASSERT_OK(dbtests::createIndex(&_opCtx, ns(), BSON("a" << 1)));
        index();
    }
    void run() {
        _client.dropDatabase("unittests");
        noIndex();
        checkIndex();
        _client.dropCollection(ns());
        noIndex();
        checkIndex();
        _client.dropDatabase("unittests");
        noIndex();
        checkIndex();
    }
};

class UniqueIndex : public ClientBase {
public:
    ~UniqueIndex() {
        _client.dropCollection("unittests.querytests.UniqueIndex");
    }
    void run() {
        const char* ns = "unittests.querytests.UniqueIndex";
        ASSERT_OK(dbtests::createIndex(&_opCtx, ns, BSON("a" << 1), true));
        _client.insert(ns, BSON("a" << 4 << "b" << 2));
        _client.insert(ns, BSON("a" << 4 << "b" << 3));
        ASSERT_EQUALS(1U, _client.count(ns, BSONObj()));
        _client.dropCollection(ns);
        ASSERT_OK(dbtests::createIndex(&_opCtx, ns, BSON("b" << 1), true));
        _client.insert(ns, BSON("a" << 4 << "b" << 2));
        _client.insert(ns, BSON("a" << 4 << "b" << 3));
        ASSERT_EQUALS(2U, _client.count(ns, BSONObj()));
    }
};

class UniqueIndexPreexistingData : public ClientBase {
public:
    ~UniqueIndexPreexistingData() {
        _client.dropCollection("unittests.querytests.UniqueIndexPreexistingData");
    }
    void run() {
        const char* ns = "unittests.querytests.UniqueIndexPreexistingData";
        _client.insert(ns, BSON("a" << 4 << "b" << 2));
        _client.insert(ns, BSON("a" << 4 << "b" << 3));
        ASSERT_EQUALS(ErrorCodes::DuplicateKey,
                      dbtests::createIndex(&_opCtx, ns, BSON("a" << 1), true));
        ASSERT_EQUALS(
            0U,
            _client.count("unittests.system.indexes", BSON("ns" << ns << "name" << NE << "_id_")));
    }
};

class SubobjectInArray : public ClientBase {
public:
    ~SubobjectInArray() {
        _client.dropCollection("unittests.querytests.SubobjectInArray");
    }
    void run() {
        const char* ns = "unittests.querytests.SubobjectInArray";
        _client.insert(ns, fromjson("{a:[{b:{c:1}}]}"));
        ASSERT(!_client.findOne(ns, BSON("a.b.c" << 1)).isEmpty());
        ASSERT(!_client.findOne(ns, fromjson("{'a.c':null}")).isEmpty());
    }
};

class Size : public ClientBase {
public:
    ~Size() {
        _client.dropCollection("unittests.querytests.Size");
    }
    void run() {
        const char* ns = "unittests.querytests.Size";
        _client.insert(ns, fromjson("{a:[1,2,3]}"));
        ASSERT_OK(dbtests::createIndex(&_opCtx, ns, BSON("a" << 1)));
        ASSERT(_client.query(ns, QUERY("a" << mongo::BSIZE << 3).hint(BSON("a" << 1)))->more());
    }
};

class FullArray : public ClientBase {
public:
    ~FullArray() {
        _client.dropCollection("unittests.querytests.IndexedArray");
    }
    void run() {
        const char* ns = "unittests.querytests.IndexedArray";
        _client.insert(ns, fromjson("{a:[1,2,3]}"));
        ASSERT(_client.query(ns, Query("{a:[1,2,3]}"))->more());
        ASSERT_OK(dbtests::createIndex(&_opCtx, ns, BSON("a" << 1)));
        ASSERT(_client.query(ns, Query("{a:{$in:[1,[1,2,3]]}}").hint(BSON("a" << 1)))->more());
        ASSERT(_client.query(ns, Query("{a:[1,2,3]}").hint(BSON("a" << 1)))->more());  // SERVER-146
    }
};

class InsideArray : public ClientBase {
public:
    ~InsideArray() {
        _client.dropCollection("unittests.querytests.InsideArray");
    }
    void run() {
        const char* ns = "unittests.querytests.InsideArray";
        _client.insert(ns, fromjson("{a:[[1],2]}"));
        check("$natural");
        ASSERT_OK(dbtests::createIndex(&_opCtx, ns, BSON("a" << 1)));
        check("a");  // SERVER-146
    }

private:
    void check(const string& hintField) {
        const char* ns = "unittests.querytests.InsideArray";
        ASSERT(_client.query(ns, Query("{a:[[1],2]}").hint(BSON(hintField << 1)))->more());
        ASSERT(_client.query(ns, Query("{a:[1]}").hint(BSON(hintField << 1)))->more());
        ASSERT(_client.query(ns, Query("{a:2}").hint(BSON(hintField << 1)))->more());
        ASSERT(!_client.query(ns, Query("{a:1}").hint(BSON(hintField << 1)))->more());
    }
};

class IndexInsideArrayCorrect : public ClientBase {
public:
    ~IndexInsideArrayCorrect() {
        _client.dropCollection("unittests.querytests.IndexInsideArrayCorrect");
    }
    void run() {
        const char* ns = "unittests.querytests.IndexInsideArrayCorrect";
        _client.insert(ns, fromjson("{'_id':1,a:[1]}"));
        _client.insert(ns, fromjson("{'_id':2,a:[[1]]}"));
        ASSERT_OK(dbtests::createIndex(&_opCtx, ns, BSON("a" << 1)));
        ASSERT_EQUALS(
            1, _client.query(ns, Query("{a:[1]}").hint(BSON("a" << 1)))->next().getIntField("_id"));
    }
};

class SubobjArr : public ClientBase {
public:
    ~SubobjArr() {
        _client.dropCollection("unittests.querytests.SubobjArr");
    }
    void run() {
        const char* ns = "unittests.querytests.SubobjArr";
        _client.insert(ns, fromjson("{a:[{b:[1]}]}"));
        check("$natural");
        ASSERT_OK(dbtests::createIndex(&_opCtx, ns, BSON("a" << 1)));
        check("a");
    }

private:
    void check(const string& hintField) {
        const char* ns = "unittests.querytests.SubobjArr";
        ASSERT(_client.query(ns, Query("{'a.b':1}").hint(BSON(hintField << 1)))->more());
        ASSERT(_client.query(ns, Query("{'a.b':[1]}").hint(BSON(hintField << 1)))->more());
    }
};

class MinMax : public ClientBase {
public:
    MinMax() : ns("unittests.querytests.MinMax") {}
    ~MinMax() {
        _client.dropCollection("unittests.querytests.MinMax");
    }
    void run() {
        ASSERT_OK(dbtests::createIndex(&_opCtx, ns, BSON("a" << 1 << "b" << 1)));
        _client.insert(ns, BSON("a" << 1 << "b" << 1));
        _client.insert(ns, BSON("a" << 1 << "b" << 2));
        _client.insert(ns, BSON("a" << 2 << "b" << 1));
        _client.insert(ns, BSON("a" << 2 << "b" << 2));

        ASSERT_EQUALS(4, count(_client.query(ns, BSONObj())));
        BSONObj hints[] = {BSONObj(), BSON("a" << 1 << "b" << 1)};
        for (int i = 0; i < 2; ++i) {
            check(0, 0, 3, 3, 4, hints[i]);
            check(1, 1, 2, 2, 3, hints[i]);
            check(1, 2, 2, 2, 2, hints[i]);
            check(1, 2, 2, 1, 1, hints[i]);

            unique_ptr<DBClientCursor> c = query(1, 2, 2, 2, hints[i]);
            BSONObj obj = c->next();
            ASSERT_EQUALS(1, obj.getIntField("a"));
            ASSERT_EQUALS(2, obj.getIntField("b"));
            obj = c->next();
            ASSERT_EQUALS(2, obj.getIntField("a"));
            ASSERT_EQUALS(1, obj.getIntField("b"));
            ASSERT(!c->more());
        }
    }

private:
    unique_ptr<DBClientCursor> query(int minA, int minB, int maxA, int maxB, const BSONObj& hint) {
        Query q;
        q = q.minKey(BSON("a" << minA << "b" << minB)).maxKey(BSON("a" << maxA << "b" << maxB));
        if (!hint.isEmpty())
            q.hint(hint);
        return _client.query(ns, q);
    }
    void check(
        int minA, int minB, int maxA, int maxB, int expectedCount, const BSONObj& hint = empty_) {
        ASSERT_EQUALS(expectedCount, count(query(minA, minB, maxA, maxB, hint)));
    }
    int count(unique_ptr<DBClientCursor> c) {
        int ret = 0;
        while (c->more()) {
            ++ret;
            c->next();
        }
        return ret;
    }
    const char* ns;
    static BSONObj empty_;
};
BSONObj MinMax::empty_;

class MatchCodeCodeWScope : public ClientBase {
public:
    MatchCodeCodeWScope() : _ns("unittests.querytests.MatchCodeCodeWScope") {}
    ~MatchCodeCodeWScope() {
        _client.dropCollection("unittests.querytests.MatchCodeCodeWScope");
    }
    void run() {
        checkMatch();
        ASSERT_OK(dbtests::createIndex(&_opCtx, _ns, BSON("a" << 1)));
        checkMatch();
    }

private:
    void checkMatch() {
        _client.remove(_ns, BSONObj());

        _client.insert(_ns, code());
        _client.insert(_ns, codeWScope());

        ASSERT_EQUALS(1U, _client.count(_ns, code()));
        ASSERT_EQUALS(1U, _client.count(_ns, codeWScope()));

        ASSERT_EQUALS(1U, _client.count(_ns, BSON("a" << BSON("$type" << (int)Code))));
        ASSERT_EQUALS(1U, _client.count(_ns, BSON("a" << BSON("$type" << (int)CodeWScope))));
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
    const char* _ns;
};

class MatchDBRefType : public ClientBase {
public:
    MatchDBRefType() : _ns("unittests.querytests.MatchDBRefType") {}
    ~MatchDBRefType() {
        _client.dropCollection("unittests.querytests.MatchDBRefType");
    }
    void run() {
        checkMatch();
        ASSERT_OK(dbtests::createIndex(&_opCtx, _ns, BSON("a" << 1)));
        checkMatch();
    }

private:
    void checkMatch() {
        _client.remove(_ns, BSONObj());
        _client.insert(_ns, dbref());
        ASSERT_EQUALS(1U, _client.count(_ns, dbref()));
        ASSERT_EQUALS(1U, _client.count(_ns, BSON("a" << BSON("$type" << (int)DBRef))));
    }
    BSONObj dbref() const {
        BSONObjBuilder b;
        OID oid;
        b.appendDBRef("a", "ns", oid);
        return b.obj();
    }
    const char* _ns;
};

class DirectLocking : public ClientBase {
public:
    void run() {
        Lock::GlobalWrite lk(&_opCtx);
        OldClientContext ctx(&_opCtx, "unittests.DirectLocking");
        _client.remove("a.b", BSONObj());
        ASSERT_EQUALS("unittests", ctx.db()->name());
    }
    const char* ns;
};

class FastCountIn : public ClientBase {
public:
    ~FastCountIn() {
        _client.dropCollection("unittests.querytests.FastCountIn");
    }
    void run() {
        const char* ns = "unittests.querytests.FastCountIn";
        _client.insert(ns,
                       BSON("i"
                            << "a"));
        ASSERT_OK(dbtests::createIndex(&_opCtx, ns, BSON("i" << 1)));
        ASSERT_EQUALS(1U, _client.count(ns, fromjson("{i:{$in:['a']}}")));
    }
};

class EmbeddedArray : public ClientBase {
public:
    ~EmbeddedArray() {
        _client.dropCollection("unittests.querytests.EmbeddedArray");
    }
    void run() {
        const char* ns = "unittests.querytests.EmbeddedArray";
        _client.insert(ns, fromjson("{foo:{bar:['spam']}}"));
        _client.insert(ns, fromjson("{foo:{bar:['spam','eggs']}}"));
        _client.insert(ns, fromjson("{bar:['spam']}"));
        _client.insert(ns, fromjson("{bar:['spam','eggs']}"));
        ASSERT_EQUALS(2U,
                      _client.count(ns,
                                    BSON("bar"
                                         << "spam")));
        ASSERT_EQUALS(2U,
                      _client.count(ns,
                                    BSON("foo.bar"
                                         << "spam")));
    }
};

class DifferentNumbers : public ClientBase {
public:
    ~DifferentNumbers() {
        _client.dropCollection("unittests.querytests.DifferentNumbers");
    }
    void t(const char* ns) {
        unique_ptr<DBClientCursor> cursor = _client.query(ns, Query().sort("7"));
        while (cursor->more()) {
            BSONObj o = cursor->next();
            verify(o.valid(BSONVersion::kLatest));
        }
    }
    void run() {
        const char* ns = "unittests.querytests.DifferentNumbers";
        {
            BSONObjBuilder b;
            b.append("7", (int)4);
            _client.insert(ns, b.obj());
        }
        {
            BSONObjBuilder b;
            b.append("7", (long long)2);
            _client.insert(ns, b.obj());
        }
        {
            BSONObjBuilder b;
            b.appendNull("7");
            _client.insert(ns, b.obj());
        }
        {
            BSONObjBuilder b;
            b.append("7", "b");
            _client.insert(ns, b.obj());
        }
        {
            BSONObjBuilder b;
            b.appendNull("8");
            _client.insert(ns, b.obj());
        }
        {
            BSONObjBuilder b;
            b.append("7", (double)3.7);
            _client.insert(ns, b.obj());
        }

        t(ns);
        ASSERT_OK(dbtests::createIndex(&_opCtx, ns, BSON("7" << 1)));
        t(ns);
    }
};

class CollectionBase : public ClientBase {
public:
    CollectionBase(string leaf) {
        _ns = "unittests.querytests.";
        _ns += leaf;
        _client.dropCollection(ns());
    }

    virtual ~CollectionBase() {
        _client.dropCollection(ns());
    }

    int count() {
        return (int)_client.count(ns());
    }

    size_t numCursorsOpen() {
        AutoGetCollectionForReadCommand ctx(&_opCtx, NamespaceString(_ns));
        Collection* collection = ctx.getCollection();
        if (!collection)
            return 0;
        return collection->getCursorManager()->numCursors();
    }

    const char* ns() {
        return _ns.c_str();
    }

private:
    string _ns;
};

class SymbolStringSame : public CollectionBase {
public:
    SymbolStringSame() : CollectionBase("symbolstringsame") {}

    void run() {
        {
            BSONObjBuilder b;
            b.appendSymbol("x", "eliot");
            b.append("z", 17);
            _client.insert(ns(), b.obj());
        }
        ASSERT_EQUALS(17, _client.findOne(ns(), BSONObj())["z"].number());
        {
            BSONObjBuilder b;
            b.appendSymbol("x", "eliot");
            ASSERT_EQUALS(17, _client.findOne(ns(), b.obj())["z"].number());
        }
        ASSERT_EQUALS(17,
                      _client
                          .findOne(ns(),
                                   BSON("x"
                                        << "eliot"))["z"]
                          .number());
        ASSERT_OK(dbtests::createIndex(&_opCtx, ns(), BSON("x" << 1)));
        ASSERT_EQUALS(17,
                      _client
                          .findOne(ns(),
                                   BSON("x"
                                        << "eliot"))["z"]
                          .number());
    }
};

class TailableCappedRaceCondition : public CollectionBase {
public:
    TailableCappedRaceCondition() : CollectionBase("tailablecappedrace") {
        _client.dropCollection(ns());
        _n = 0;
    }
    void run() {
        string err;
        OldClientWriteContext ctx(&_opCtx, ns());

        // note that extents are always at least 4KB now - so this will get rounded up
        // a bit.
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT(userCreateNS(&_opCtx,
                                ctx.db(),
                                ns(),
                                fromjson("{ capped : true, size : 2000, max: 10000 }"),
                                CollectionOptions::parseForCommand,
                                false)
                       .isOK());
            wunit.commit();
        }

        for (int i = 0; i < 200; i++) {
            insertNext();
            ASSERT(count() < 90);
        }

        int a = count();

        unique_ptr<DBClientCursor> c =
            _client.query(ns(),
                          QUERY("i" << GT << 0).hint(BSON("$natural" << 1)),
                          0,
                          0,
                          0,
                          QueryOption_CursorTailable);
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
                      UserException);
    }

    void insertNext() {
        BSONObjBuilder b;
        b.appendOID("_id", 0, true);
        b.append("i", _n++);
        insert(ns(), b.obj());
    }

    int _n;
};

class HelperTest : public CollectionBase {
public:
    HelperTest() : CollectionBase("helpertest") {}

    void run() {
        OldClientWriteContext ctx(&_opCtx, ns());

        for (int i = 0; i < 50; i++) {
            insert(ns(), BSON("_id" << i << "x" << i * 2));
        }

        ASSERT_EQUALS(50, count());

        BSONObj res;
        ASSERT(Helpers::findOne(&_opCtx, ctx.getCollection(), BSON("_id" << 20), res, true));
        ASSERT_EQUALS(40, res["x"].numberInt());

        ASSERT(Helpers::findById(&_opCtx, ctx.db(), ns(), BSON("_id" << 20), res));
        ASSERT_EQUALS(40, res["x"].numberInt());

        ASSERT(!Helpers::findById(&_opCtx, ctx.db(), ns(), BSON("_id" << 200), res));

        long long slow;
        long long fast;

        int n = 10000;
        DEV n = 1000;
        {
            Timer t;
            for (int i = 0; i < n; i++) {
                ASSERT(
                    Helpers::findOne(&_opCtx, ctx.getCollection(), BSON("_id" << 20), res, true));
            }
            slow = t.micros();
        }
        {
            Timer t;
            for (int i = 0; i < n; i++) {
                ASSERT(Helpers::findById(&_opCtx, ctx.db(), ns(), BSON("_id" << 20), res));
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
        OldClientWriteContext ctx(&_opCtx, ns());

        for (int i = 0; i < 1000; i++) {
            insert(ns(), BSON("_id" << i << "x" << i * 2));
        }
        for (int i = 0; i < 1000; i += 2) {
            _client.remove(ns(), BSON("_id" << i));
        }

        BSONObj res;
        for (int i = 0; i < 1000; i++) {
            bool found = Helpers::findById(&_opCtx, ctx.db(), ns(), BSON("_id" << i), res);
            ASSERT_EQUALS(i % 2, int(found));
        }
    }
};

class ClientCursorTest : public CollectionBase {
    ClientCursorTest() : CollectionBase("clientcursortest") {}

    void run() {
        OldClientWriteContext ctx(&_opCtx, ns());

        for (int i = 0; i < 1000; i++) {
            insert(ns(), BSON("_id" << i << "x" << i * 2));
        }
    }
};

class FindingStart : public CollectionBase {
public:
    FindingStart() : CollectionBase("findingstart") {}

    void run() {
        BSONObj info;
        ASSERT(_client.runCommand("unittests",
                                  BSON("create"
                                       << "querytests.findingstart"
                                       << "capped"
                                       << true
                                       << "$nExtents"
                                       << 5
                                       << "autoIndexId"
                                       << false),
                                  info));

        int i = 0;
        int max = 1;

        while (1) {
            int oldCount = count();
            _client.insert(ns(), BSON("ts" << i++));
            int newCount = count();
            if (oldCount == newCount || newCount < max)
                break;

            if (newCount > max)
                max = newCount;
        }

        for (int k = 0; k < 5; ++k) {
            _client.insert(ns(), BSON("ts" << i++));
            int min =
                _client.query(ns(), Query().sort(BSON("$natural" << 1)))->next()["ts"].numberInt();
            for (int j = -1; j < i; ++j) {
                unique_ptr<DBClientCursor> c =
                    _client.query(ns(), QUERY("ts" << GTE << j), 0, 0, 0, QueryOption_OplogReplay);
                ASSERT(c->more());
                BSONObj next = c->next();
                ASSERT(!next["ts"].eoo());
                ASSERT_EQUALS((j > min ? j : min), next["ts"].numberInt());
            }
        }
    }
};

class FindingStartPartiallyFull : public CollectionBase {
public:
    FindingStartPartiallyFull() : CollectionBase("findingstart") {}

    void run() {
        size_t startNumCursors = numCursorsOpen();

        BSONObj info;
        ASSERT(_client.runCommand("unittests",
                                  BSON("create"
                                       << "querytests.findingstart"
                                       << "capped"
                                       << true
                                       << "$nExtents"
                                       << 5
                                       << "autoIndexId"
                                       << false),
                                  info));

        int i = 0;
        for (; i < 150; _client.insert(ns(), BSON("ts" << i++)))
            ;

        for (int k = 0; k < 5; ++k) {
            _client.insert(ns(), BSON("ts" << i++));
            int min =
                _client.query(ns(), Query().sort(BSON("$natural" << 1)))->next()["ts"].numberInt();
            for (int j = -1; j < i; ++j) {
                unique_ptr<DBClientCursor> c =
                    _client.query(ns(), QUERY("ts" << GTE << j), 0, 0, 0, QueryOption_OplogReplay);
                ASSERT(c->more());
                BSONObj next = c->next();
                ASSERT(!next["ts"].eoo());
                ASSERT_EQUALS((j > min ? j : min), next["ts"].numberInt());
            }
        }

        ASSERT_EQUALS(startNumCursors, numCursorsOpen());
    }
};

/**
 * Check OplogReplay mode where query timestamp is earlier than the earliest
 * entry in the collection.
 */
class FindingStartStale : public CollectionBase {
public:
    FindingStartStale() : CollectionBase("findingstart") {}

    void run() {
        size_t startNumCursors = numCursorsOpen();

        // Check OplogReplay mode with missing collection.
        unique_ptr<DBClientCursor> c0 =
            _client.query(ns(), QUERY("ts" << GTE << 50), 0, 0, 0, QueryOption_OplogReplay);
        ASSERT(!c0->more());

        BSONObj info;
        ASSERT(_client.runCommand("unittests",
                                  BSON("create"
                                       << "querytests.findingstart"
                                       << "capped"
                                       << true
                                       << "$nExtents"
                                       << 5
                                       << "autoIndexId"
                                       << false),
                                  info));

        // Check OplogReplay mode with empty collection.
        unique_ptr<DBClientCursor> c =
            _client.query(ns(), QUERY("ts" << GTE << 50), 0, 0, 0, QueryOption_OplogReplay);
        ASSERT(!c->more());

        // Check with some docs in the collection.
        for (int i = 100; i < 150; _client.insert(ns(), BSON("ts" << i++)))
            ;
        c = _client.query(ns(), QUERY("ts" << GTE << 50), 0, 0, 0, QueryOption_OplogReplay);
        ASSERT(c->more());
        ASSERT_EQUALS(100, c->next()["ts"].numberInt());

        // Check that no persistent cursors outlast our queries above.
        ASSERT_EQUALS(startNumCursors, numCursorsOpen());
    }
};

class WhatsMyUri : public CollectionBase {
public:
    WhatsMyUri() : CollectionBase("whatsmyuri") {}
    void run() {
        BSONObj result;
        _client.runCommand("admin", BSON("whatsmyuri" << 1), result);
        ASSERT_EQUALS("", result["you"].str());
    }
};

class CollectionInternalBase : public CollectionBase {
public:
    CollectionInternalBase(const char* nsLeaf)
        : CollectionBase(nsLeaf), _lk(&_opCtx, "unittests", MODE_X), _ctx(&_opCtx, ns()) {}

private:
    Lock::DBLock _lk;
    OldClientContext _ctx;
};

class Exhaust : public CollectionInternalBase {
public:
    Exhaust() : CollectionInternalBase("exhaust") {}
    void run() {
        BSONObj info;
        ASSERT(_client.runCommand("unittests",
                                  BSON("create"
                                       << "querytests.exhaust"
                                       << "capped"
                                       << true
                                       << "size"
                                       << 8192),
                                  info));
        _client.insert(ns(), BSON("ts" << 0));
        Message message;
        assembleQueryRequest(ns(),
                             BSON("ts" << GTE << 0),
                             0,
                             0,
                             0,
                             QueryOption_OplogReplay | QueryOption_CursorTailable |
                                 QueryOption_Exhaust,
                             message);
        DbMessage dbMessage(message);
        QueryMessage queryMessage(dbMessage);
        Message result;
        string exhaust = runQuery(&_opCtx, queryMessage, NamespaceString(ns()), result);
        ASSERT(exhaust.size());
        ASSERT_EQUALS(string(ns()), exhaust);
    }
};

class QueryReadsAll : public CollectionBase {
public:
    QueryReadsAll() : CollectionBase("queryreadsall") {}
    void run() {
        for (int i = 0; i < 5; ++i) {
            insert(ns(), BSONObj());
        }
        {
            // With five results and a batch size of 5, a cursor is created since we don't know
            // there are no more results.
            std::unique_ptr<DBClientCursor> c = _client.query(ns(), Query(), 5);
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
            std::unique_ptr<DBClientCursor> c = _client.query(ns(), Query(), 6);
            ASSERT(c->more());
            ASSERT_EQ(0, c->getCursorId());
        }
    }
};

namespace queryobjecttests {
class names1 {
public:
    void run() {
        ASSERT_BSONOBJ_EQ(BSON("x" << 1), QUERY("query" << BSON("x" << 1)).getFilter());
        ASSERT_BSONOBJ_EQ(BSON("x" << 1), QUERY("$query" << BSON("x" << 1)).getFilter());
    }
};
}  // namespace queryobjecttests

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

class All : public Suite {
public:
    All() : Suite("query") {}

    void setupTests() {
        add<FindingStart>();
        add<FindOneOr>();
        add<FindOneRequireIndex>();
        add<FindOneEmptyObj>();
        add<BoundedKey>();
        add<GetMore>();
        add<GetMoreKillOp>();
        add<GetMoreInvalidRequest>();
        add<PositiveLimit>();
        add<TailNotAtEnd>();
        add<EmptyTail>();
        add<TailableDelete>();
        add<TailableDelete2>();
        add<TailableInsertDelete>();
        add<TailCappedOnly>();
        add<TailableQueryOnId>();
        add<OplogReplayMode>();
        add<OplogReplaySlaveReadTill>();
        add<OplogReplayExplain>();
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
        add<MinMax>();
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
        add<Exhaust>();
        add<QueryReadsAll>();
        add<queryobjecttests::names1>();
        add<OrderingTest>();
    }
};

SuiteInstance<All> myall;

}  // namespace QueryTests
}  // namespace
