// repltests.cpp : Unit tests for replication
//

/**
 *    Copyright (C) 2009-2014 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/client.h"
#include "mongo/db/db.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/json.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/repl/master_slave.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/sync_tail.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/util/log.h"

using namespace mongo::repl;

namespace ReplTests {

using std::unique_ptr;
using std::endl;
using std::string;
using std::stringstream;
using std::vector;

BSONObj f(const char* s) {
    return fromjson(s);
}

class Base {
protected:
    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _txn = *_txnPtr;
    mutable DBDirectClient _client;

public:
    Base() : _client(&_txn) {
        ReplSettings replSettings;
        replSettings.setOplogSizeBytes(10 * 1024 * 1024);
        replSettings.setMaster(true);
        setGlobalReplicationCoordinator(new repl::ReplicationCoordinatorMock(replSettings));

        getGlobalServiceContext()->setOpObserver(stdx::make_unique<OpObserver>());

        setOplogCollectionName();
        createOplog(&_txn);

        OldClientWriteContext ctx(&_txn, ns());
        WriteUnitOfWork wuow(&_txn);

        Collection* c = ctx.db()->getCollection(ns());
        if (!c) {
            c = ctx.db()->createCollection(&_txn, ns());
        }

        ASSERT(c->getIndexCatalog()->haveIdIndex(&_txn));
        wuow.commit();
    }
    ~Base() {
        try {
            deleteAll(ns());
            deleteAll(cllNS());
            ReplSettings replSettings;
            replSettings.setOplogSizeBytes(10 * 1024 * 1024);
            setGlobalReplicationCoordinator(new repl::ReplicationCoordinatorMock(replSettings));
        } catch (...) {
            FAIL("Exception while cleaning up test");
        }
    }

protected:
    static const char* ns() {
        return "unittests.repltests";
    }
    static const char* cllNS() {
        return "local.oplog.$main";
    }
    BSONObj one(const BSONObj& query = BSONObj()) const {
        return _client.findOne(ns(), query);
    }
    void checkOne(const BSONObj& o) const {
        check(o, one(o));
    }
    void checkAll(const BSONObj& o) const {
        unique_ptr<DBClientCursor> c = _client.query(ns(), o);
        verify(c->more());
        while (c->more()) {
            check(o, c->next());
        }
    }
    void check(const BSONObj& expected, const BSONObj& got) const {
        if (expected.woCompare(got)) {
            ::mongo::log() << "expected: " << expected.toString() << ", got: " << got.toString()
                           << endl;
        }
        ASSERT_EQUALS(expected, got);
    }
    BSONObj oneOp() const {
        return _client.findOne(cllNS(), BSONObj());
    }
    int count() const {
        ScopedTransaction transaction(&_txn, MODE_X);
        Lock::GlobalWrite lk(_txn.lockState());
        OldClientContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = db->getCollection(ns());
        if (!coll) {
            WriteUnitOfWork wunit(&_txn);
            coll = db->createCollection(&_txn, ns());
            wunit.commit();
        }

        int count = 0;
        auto cursor = coll->getCursor(&_txn);
        while (auto record = cursor->next()) {
            ++count;
        }
        return count;
    }
    int opCount() {
        ScopedTransaction transaction(&_txn, MODE_X);
        Lock::GlobalWrite lk(_txn.lockState());
        OldClientContext ctx(&_txn, cllNS());

        Database* db = ctx.db();
        Collection* coll = db->getCollection(cllNS());
        if (!coll) {
            WriteUnitOfWork wunit(&_txn);
            coll = db->createCollection(&_txn, cllNS());
            wunit.commit();
        }

        int count = 0;
        auto cursor = coll->getCursor(&_txn);
        while (auto record = cursor->next()) {
            ++count;
        }
        return count;
    }
    void applyAllOperations() {
        ScopedTransaction transaction(&_txn, MODE_X);
        Lock::GlobalWrite lk(_txn.lockState());
        vector<BSONObj> ops;
        {
            OldClientContext ctx(&_txn, cllNS());
            Database* db = ctx.db();
            Collection* coll = db->getCollection(cllNS());

            auto cursor = coll->getCursor(&_txn);
            while (auto record = cursor->next()) {
                ops.push_back(record->data.releaseToBson().getOwned());
            }
        }
        {
            OldClientContext ctx(&_txn, ns());
            BSONObjBuilder b;
            b.append("host", "localhost");
            b.appendTimestamp("syncedTo", 0);
            ReplSource a(&_txn, b.obj());
            for (vector<BSONObj>::iterator i = ops.begin(); i != ops.end(); ++i) {
                if (0) {
                    mongo::unittest::log() << "op: " << *i << endl;
                }
                _txn.setReplicatedWrites(false);
                a.applyOperation(&_txn, ctx.db(), *i);
                _txn.setReplicatedWrites(true);
            }
        }
    }
    void printAll(const char* ns) {
        ScopedTransaction transaction(&_txn, MODE_X);
        Lock::GlobalWrite lk(_txn.lockState());
        OldClientContext ctx(&_txn, ns);

        Database* db = ctx.db();
        Collection* coll = db->getCollection(ns);
        if (!coll) {
            WriteUnitOfWork wunit(&_txn);
            coll = db->createCollection(&_txn, ns);
            wunit.commit();
        }

        auto cursor = coll->getCursor(&_txn);
        ::mongo::log() << "all for " << ns << endl;
        while (auto record = cursor->next()) {
            ::mongo::log() << record->data.releaseToBson() << endl;
        }
    }
    // These deletes don't get logged.
    void deleteAll(const char* ns) const {
        ScopedTransaction transaction(&_txn, MODE_X);
        Lock::GlobalWrite lk(_txn.lockState());
        OldClientContext ctx(&_txn, ns);
        WriteUnitOfWork wunit(&_txn);
        Database* db = ctx.db();
        Collection* coll = db->getCollection(ns);
        if (!coll) {
            coll = db->createCollection(&_txn, ns);
        }

        ASSERT_OK(coll->truncate(&_txn));
        wunit.commit();
    }
    void insert(const BSONObj& o) const {
        ScopedTransaction transaction(&_txn, MODE_X);
        Lock::GlobalWrite lk(_txn.lockState());
        OldClientContext ctx(&_txn, ns());
        WriteUnitOfWork wunit(&_txn);
        Database* db = ctx.db();
        Collection* coll = db->getCollection(ns());
        if (!coll) {
            coll = db->createCollection(&_txn, ns());
        }

        OpDebug* const nullOpDebug = nullptr;
        if (o.hasField("_id")) {
            _txn.setReplicatedWrites(false);
            coll->insertDocument(&_txn, o, nullOpDebug, true);
            _txn.setReplicatedWrites(true);
            wunit.commit();
            return;
        }

        class BSONObjBuilder b;
        OID id;
        id.init();
        b.appendOID("_id", &id);
        b.appendElements(o);
        _txn.setReplicatedWrites(false);
        coll->insertDocument(&_txn, b.obj(), nullOpDebug, true);
        _txn.setReplicatedWrites(true);
        wunit.commit();
    }
    static BSONObj wid(const char* json) {
        class BSONObjBuilder b;
        OID id;
        id.init();
        b.appendOID("_id", &id);
        b.appendElements(fromjson(json));
        return b.obj();
    }
};


class LogBasic : public Base {
public:
    void run() {
        ASSERT_EQUALS(2, opCount());
        _client.insert(ns(), fromjson("{\"a\":\"b\"}"));
        ASSERT_EQUALS(3, opCount());
    }
};

namespace Idempotence {

class Base : public ReplTests::Base {
public:
    virtual ~Base() {}
    void run() {
        reset();
        doIt();
        int nOps = opCount();
        check();
        applyAllOperations();
        check();
        ASSERT_EQUALS(nOps, opCount());

        reset();
        applyAllOperations();
        check();
        ASSERT_EQUALS(nOps, opCount());
        applyAllOperations();
        check();
        ASSERT_EQUALS(nOps, opCount());
    }

protected:
    virtual void doIt() const = 0;
    virtual void check() const = 0;
    virtual void reset() const = 0;
};

class InsertTimestamp : public Base {
public:
    void doIt() const {
        BSONObjBuilder b;
        b.append("a", 1);
        b.appendTimestamp("t");
        _client.insert(ns(), b.done());
        date_ = _client.findOne(ns(), QUERY("a" << 1)).getField("t").date();
    }
    void check() const {
        BSONObj o = _client.findOne(ns(), QUERY("a" << 1));
        ASSERT(Date_t{} != o.getField("t").date());
        ASSERT_EQUALS(date_, o.getField("t").date());
    }
    void reset() const {
        deleteAll(ns());
    }

private:
    mutable Date_t date_;
};

class InsertAutoId : public Base {
public:
    InsertAutoId() : o_(fromjson("{\"a\":\"b\"}")) {}
    void doIt() const {
        _client.insert(ns(), o_);
    }
    void check() const {
        ASSERT_EQUALS(1, count());
    }
    void reset() const {
        deleteAll(ns());
    }

protected:
    BSONObj o_;
};

class InsertWithId : public InsertAutoId {
public:
    InsertWithId() {
        o_ = fromjson("{\"_id\":ObjectId(\"0f0f0f0f0f0f0f0f0f0f0f0f\"),\"a\":\"b\"}");
    }
    void check() const {
        ASSERT_EQUALS(1, count());
        checkOne(o_);
    }
};

class InsertTwo : public Base {
public:
    InsertTwo() : o_(fromjson("{'_id':1,a:'b'}")), t_(fromjson("{'_id':2,c:'d'}")) {}
    void doIt() const {
        vector<BSONObj> v;
        v.push_back(o_);
        v.push_back(t_);
        _client.insert(ns(), v);
    }
    void check() const {
        ASSERT_EQUALS(2, count());
        checkOne(o_);
        checkOne(t_);
    }
    void reset() const {
        deleteAll(ns());
    }

private:
    BSONObj o_;
    BSONObj t_;
};

class InsertTwoIdentical : public Base {
public:
    InsertTwoIdentical() : o_(fromjson("{\"a\":\"b\"}")) {}
    void doIt() const {
        _client.insert(ns(), o_);
        _client.insert(ns(), o_);
    }
    void check() const {
        ASSERT_EQUALS(2, count());
    }
    void reset() const {
        deleteAll(ns());
    }

private:
    BSONObj o_;
};

class UpdateTimestamp : public Base {
public:
    void doIt() const {
        BSONObjBuilder b;
        b.append("_id", 1);
        b.appendTimestamp("t");
        _client.update(ns(), BSON("_id" << 1), b.done());
        date_ = _client.findOne(ns(), QUERY("_id" << 1)).getField("t").date();
    }
    void check() const {
        BSONObj o = _client.findOne(ns(), QUERY("_id" << 1));
        ASSERT(Date_t{} != o.getField("t").date());
        ASSERT_EQUALS(date_, o.getField("t").date());
    }
    void reset() const {
        deleteAll(ns());
        insert(BSON("_id" << 1));
    }

private:
    mutable Date_t date_;
};

class UpdateSameField : public Base {
public:
    UpdateSameField()
        : q_(fromjson("{a:'b'}")),
          o1_(wid("{a:'b'}")),
          o2_(wid("{a:'b'}")),
          u_(fromjson("{a:'c'}")) {}
    void doIt() const {
        _client.update(ns(), q_, u_);
    }
    void check() const {
        ASSERT_EQUALS(2, count());
        ASSERT(!_client.findOne(ns(), q_).isEmpty());
        ASSERT(!_client.findOne(ns(), u_).isEmpty());
    }
    void reset() const {
        deleteAll(ns());
        insert(o1_);
        insert(o2_);
    }

private:
    BSONObj q_, o1_, o2_, u_;
};

class UpdateSameFieldWithId : public Base {
public:
    UpdateSameFieldWithId()
        : o_(fromjson("{'_id':1,a:'b'}")),
          q_(fromjson("{a:'b'}")),
          u_(fromjson("{'_id':1,a:'c'}")) {}
    void doIt() const {
        _client.update(ns(), q_, u_);
    }
    void check() const {
        ASSERT_EQUALS(2, count());
        ASSERT(!_client.findOne(ns(), q_).isEmpty());
        ASSERT(!_client.findOne(ns(), u_).isEmpty());
    }
    void reset() const {
        deleteAll(ns());
        insert(o_);
        insert(fromjson("{'_id':2,a:'b'}"));
    }

private:
    BSONObj o_, q_, u_;
};

class UpdateSameFieldExplicitId : public Base {
public:
    UpdateSameFieldExplicitId()
        : o_(fromjson("{'_id':1,a:'b'}")), u_(fromjson("{'_id':1,a:'c'}")) {}
    void doIt() const {
        _client.update(ns(), o_, u_);
    }
    void check() const {
        ASSERT_EQUALS(1, count());
        checkOne(u_);
    }
    void reset() const {
        deleteAll(ns());
        insert(o_);
    }

protected:
    BSONObj o_, u_;
};

class UpdateDifferentFieldExplicitId : public Base {
public:
    UpdateDifferentFieldExplicitId()
        : o_(fromjson("{'_id':1,a:'b'}")),
          q_(fromjson("{'_id':1}")),
          u_(fromjson("{'_id':1,a:'c'}")) {}
    void doIt() const {
        _client.update(ns(), q_, u_);
    }
    void check() const {
        ASSERT_EQUALS(1, count());
        checkOne(u_);
    }
    void reset() const {
        deleteAll(ns());
        insert(o_);
    }

protected:
    BSONObj o_, q_, u_;
};

class UpsertUpdateNoMods : public UpdateDifferentFieldExplicitId {
    void doIt() const {
        _client.update(ns(), q_, u_, true);
    }
};

class UpsertInsertNoMods : public InsertAutoId {
    void doIt() const {
        _client.update(ns(), fromjson("{a:'c'}"), o_, true);
    }
};

class UpdateSet : public Base {
public:
    UpdateSet()
        : o_(fromjson("{'_id':1,a:5}")),
          q_(fromjson("{a:5}")),
          u_(fromjson("{$set:{a:7}}")),
          ou_(fromjson("{'_id':1,a:7}")) {}
    void doIt() const {
        _client.update(ns(), q_, u_);
    }
    void check() const {
        ASSERT_EQUALS(1, count());
        checkOne(ou_);
    }
    void reset() const {
        deleteAll(ns());
        insert(o_);
    }

protected:
    BSONObj o_, q_, u_, ou_;
};

class UpdateInc : public Base {
public:
    UpdateInc()
        : o_(fromjson("{'_id':1,a:5}")),
          q_(fromjson("{a:5}")),
          u_(fromjson("{$inc:{a:3}}")),
          ou_(fromjson("{'_id':1,a:8}")) {}
    void doIt() const {
        _client.update(ns(), q_, u_);
    }
    void check() const {
        ASSERT_EQUALS(1, count());
        checkOne(ou_);
    }
    void reset() const {
        deleteAll(ns());
        insert(o_);
    }

protected:
    BSONObj o_, q_, u_, ou_;
};

class UpdateInc2 : public Base {
public:
    UpdateInc2()
        : o_(fromjson("{'_id':1,a:5}")),
          q_(fromjson("{a:5}")),
          u_(fromjson("{$inc:{a:3},$set:{x:5}}")),
          ou_(fromjson("{'_id':1,a:8,x:5}")) {}
    void doIt() const {
        _client.update(ns(), q_, u_);
    }
    void check() const {
        ASSERT_EQUALS(1, count());
        checkOne(ou_);
    }
    void reset() const {
        deleteAll(ns());
        insert(o_);
    }

protected:
    BSONObj o_, q_, u_, ou_;
};

class IncEmbedded : public Base {
public:
    IncEmbedded()
        : o_(fromjson("{'_id':1,a:{b:3},b:{b:1}}")),
          q_(fromjson("{'_id':1}")),
          u_(fromjson("{$inc:{'a.b':1,'b.b':1}}")),
          ou_(fromjson("{'_id':1,a:{b:4},b:{b:2}}")) {}
    void doIt() const {
        _client.update(ns(), q_, u_);
    }
    void check() const {
        ASSERT_EQUALS(1, count());
        checkOne(ou_);
    }
    void reset() const {
        deleteAll(ns());
        insert(o_);
    }

protected:
    BSONObj o_, q_, u_, ou_;
};

class IncCreates : public Base {
public:
    IncCreates()
        : o_(fromjson("{'_id':1}")),
          q_(fromjson("{'_id':1}")),
          u_(fromjson("{$inc:{'a':1}}")),
          ou_(fromjson("{'_id':1,a:1}")) {}
    void doIt() const {
        _client.update(ns(), q_, u_);
    }
    void check() const {
        ASSERT_EQUALS(1, count());
        checkOne(ou_);
    }
    void reset() const {
        deleteAll(ns());
        insert(o_);
    }

protected:
    BSONObj o_, q_, u_, ou_;
};


class UpsertInsertIdMod : public Base {
public:
    UpsertInsertIdMod()
        : q_(fromjson("{'_id':5,a:4}")),
          u_(fromjson("{$inc:{a:3}}")),
          ou_(fromjson("{'_id':5,a:7}")) {}
    void doIt() const {
        _client.update(ns(), q_, u_, true);
    }
    void check() const {
        ASSERT_EQUALS(1, count());
        checkOne(ou_);
    }
    void reset() const {
        deleteAll(ns());
    }

protected:
    BSONObj q_, u_, ou_;
};

class UpsertInsertSet : public Base {
public:
    UpsertInsertSet()
        : q_(fromjson("{a:5}")), u_(fromjson("{$set:{a:7}}")), ou_(fromjson("{a:7}")) {}
    void doIt() const {
        _client.update(ns(), q_, u_, true);
    }
    void check() const {
        ASSERT_EQUALS(2, count());
        ASSERT(!_client.findOne(ns(), ou_).isEmpty());
    }
    void reset() const {
        deleteAll(ns());
        insert(fromjson("{'_id':7,a:7}"));
    }

protected:
    BSONObj o_, q_, u_, ou_;
};

class UpsertInsertInc : public Base {
public:
    UpsertInsertInc()
        : q_(fromjson("{a:5}")), u_(fromjson("{$inc:{a:3}}")), ou_(fromjson("{a:8}")) {}
    void doIt() const {
        _client.update(ns(), q_, u_, true);
    }
    void check() const {
        ASSERT_EQUALS(1, count());
        ASSERT(!_client.findOne(ns(), ou_).isEmpty());
    }
    void reset() const {
        deleteAll(ns());
    }

protected:
    BSONObj o_, q_, u_, ou_;
};

class MultiInc : public Base {
public:
    string s() const {
        stringstream ss;
        unique_ptr<DBClientCursor> cc = _client.query(ns(), Query().sort(BSON("_id" << 1)));
        bool first = true;
        while (cc->more()) {
            if (first)
                first = false;
            else
                ss << ",";

            BSONObj o = cc->next();
            ss << o["x"].numberInt();
        }
        return ss.str();
    }

    void doIt() const {
        _client.insert(ns(), BSON("_id" << 1 << "x" << 1));
        _client.insert(ns(), BSON("_id" << 2 << "x" << 5));

        ASSERT_EQUALS("1,5", s());

        _client.update(ns(), BSON("_id" << 1), BSON("$inc" << BSON("x" << 1)));
        ASSERT_EQUALS("2,5", s());

        _client.update(ns(), BSONObj(), BSON("$inc" << BSON("x" << 1)));
        ASSERT_EQUALS("3,5", s());

        _client.update(ns(), BSONObj(), BSON("$inc" << BSON("x" << 1)), false, true);
        check();
    }

    void check() const {
        ASSERT_EQUALS("4,6", s());
    }

    void reset() const {
        deleteAll(ns());
    }
};

class UpdateWithoutPreexistingId : public Base {
public:
    UpdateWithoutPreexistingId()
        : o_(fromjson("{a:5}")), u_(fromjson("{a:5}")), ot_(fromjson("{b:4}")) {}
    void doIt() const {
        _client.update(ns(), o_, u_);
    }
    void check() const {
        ASSERT_EQUALS(2, count());
        checkOne(u_);
        checkOne(ot_);
    }
    void reset() const {
        deleteAll(ns());
        insert(ot_);
        insert(o_);
    }

protected:
    BSONObj o_, u_, ot_;
};

class Remove : public Base {
public:
    Remove()
        : o1_(f("{\"_id\":\"010101010101010101010101\",\"a\":\"b\"}")),
          o2_(f("{\"_id\":\"010101010101010101010102\",\"a\":\"b\"}")),
          q_(f("{\"a\":\"b\"}")) {}
    void doIt() const {
        _client.remove(ns(), q_);
    }
    void check() const {
        ASSERT_EQUALS(0, count());
    }
    void reset() const {
        deleteAll(ns());
        insert(o1_);
        insert(o2_);
    }

protected:
    BSONObj o1_, o2_, q_;
};

class RemoveOne : public Remove {
    void doIt() const {
        _client.remove(ns(), q_, true);
    }
    void check() const {
        ASSERT_EQUALS(1, count());
    }
};

class FailingUpdate : public Base {
public:
    FailingUpdate() : o_(fromjson("{'_id':1,a:'b'}")), u_(fromjson("{'_id':1,c:'d'}")) {}
    void doIt() const {
        _client.update(ns(), o_, u_);
        _client.insert(ns(), o_);
    }
    void check() const {
        ASSERT_EQUALS(1, count());
        checkOne(o_);
    }
    void reset() const {
        deleteAll(ns());
    }

protected:
    BSONObj o_, u_;
};

class SetNumToStr : public Base {
public:
    void doIt() const {
        _client.update(ns(),
                       BSON("_id" << 0),
                       BSON("$set" << BSON("a"
                                           << "bcd")));
    }
    void check() const {
        ASSERT_EQUALS(1, count());
        checkOne(BSON("_id" << 0 << "a"
                            << "bcd"));
    }
    void reset() const {
        deleteAll(ns());
        insert(BSON("_id" << 0 << "a" << 4.0));
    }
};

class Push : public Base {
public:
    void doIt() const {
        _client.update(ns(), BSON("_id" << 0), BSON("$push" << BSON("a" << 5.0)));
    }
    using ReplTests::Base::check;
    void check() const {
        ASSERT_EQUALS(1, count());
        check(fromjson("{'_id':0,a:[4,5]}"), one(fromjson("{'_id':0}")));
    }
    void reset() const {
        deleteAll(ns());
        insert(fromjson("{'_id':0,a:[4]}"));
    }
};

class PushUpsert : public Base {
public:
    void doIt() const {
        _client.update(ns(), BSON("_id" << 0), BSON("$push" << BSON("a" << 5.0)), true);
    }
    using ReplTests::Base::check;
    void check() const {
        ASSERT_EQUALS(1, count());
        check(fromjson("{'_id':0,a:[4,5]}"), one(fromjson("{'_id':0}")));
    }
    void reset() const {
        deleteAll(ns());
        insert(fromjson("{'_id':0,a:[4]}"));
    }
};

class MultiPush : public Base {
public:
    void doIt() const {
        _client.update(ns(),
                       BSON("_id" << 0),
                       BSON("$push" << BSON("a" << 5.0) << "$push" << BSON("b.c" << 6.0)));
    }
    using ReplTests::Base::check;
    void check() const {
        ASSERT_EQUALS(1, count());
        check(fromjson("{'_id':0,a:[4,5],b:{c:[6]}}"), one(fromjson("{'_id':0}")));
    }
    void reset() const {
        deleteAll(ns());
        insert(fromjson("{'_id':0,a:[4]}"));
    }
};

class EmptyPush : public Base {
public:
    void doIt() const {
        _client.update(ns(), BSON("_id" << 0), BSON("$push" << BSON("a" << 5.0)));
    }
    using ReplTests::Base::check;
    void check() const {
        ASSERT_EQUALS(1, count());
        check(fromjson("{'_id':0,a:[5]}"), one(fromjson("{'_id':0}")));
    }
    void reset() const {
        deleteAll(ns());
        insert(fromjson("{'_id':0}"));
    }
};

class EmptyPushSparseIndex : public EmptyPush {
public:
    EmptyPushSparseIndex() {
        _client.insert("unittests.system.indexes",
                       BSON("ns" << ns() << "key" << BSON("a" << 1) << "name"
                                 << "foo"
                                 << "sparse"
                                 << true));
    }
    ~EmptyPushSparseIndex() {
        _client.dropIndexes(ns());
    }
};

class PushAll : public Base {
public:
    void doIt() const {
        _client.update(ns(), BSON("_id" << 0), fromjson("{$pushAll:{a:[5.0,6.0]}}"));
    }
    using ReplTests::Base::check;
    void check() const {
        ASSERT_EQUALS(1, count());
        check(fromjson("{'_id':0,a:[4,5,6]}"), one(fromjson("{'_id':0}")));
    }
    void reset() const {
        deleteAll(ns());
        insert(fromjson("{'_id':0,a:[4]}"));
    }
};

class PushWithDollarSigns : public Base {
    void doIt() const {
        _client.update(ns(), BSON("_id" << 0), BSON("$push" << BSON("a" << BSON("$foo" << 1))));
    }
    using ReplTests::Base::check;
    void check() const {
        ASSERT_EQUALS(1, count());
        check(fromjson("{'_id':0, a:[0, {'$foo':1}]}"), one(fromjson("{'_id':0}")));
    }
    void reset() const {
        deleteAll(ns());
        insert(BSON("_id" << 0 << "a" << BSON_ARRAY(0)));
    }
};

class PushSlice : public Base {
    void doIt() const {
        _client.update(
            ns(),
            BSON("_id" << 0),
            BSON("$push" << BSON("a" << BSON("$each" << BSON_ARRAY(3) << "$slice" << -2))));
    }
    using ReplTests::Base::check;
    void check() const {
        ASSERT_EQUALS(1, count());
        check(fromjson("{'_id':0, a:[2,3]}"), one(fromjson("{'_id':0}")));
    }
    void reset() const {
        deleteAll(ns());
        insert(BSON("_id" << 0 << "a" << BSON_ARRAY(1 << 2)));
    }
};

class PushSliceInitiallyInexistent : public Base {
    void doIt() const {
        _client.update(
            ns(),
            BSON("_id" << 0),
            BSON("$push" << BSON("a" << BSON("$each" << BSON_ARRAY(1 << 2) << "$slice" << -2))));
    }
    using ReplTests::Base::check;
    void check() const {
        ASSERT_EQUALS(1, count());
        check(fromjson("{'_id':0, a:[1,2] }"), one(fromjson("{'_id':0}")));
    }
    void reset() const {
        deleteAll(ns());
        insert(BSON("_id" << 0));
    }
};

class PushSliceToZero : public Base {
    void doIt() const {
        _client.update(
            ns(),
            BSON("_id" << 0),
            BSON("$push" << BSON("a" << BSON("$each" << BSON_ARRAY(3) << "$slice" << 0))));
    }
    using ReplTests::Base::check;
    void check() const {
        ASSERT_EQUALS(1, count());
        check(fromjson("{'_id':0, a:[]}"), one(fromjson("{'_id':0}")));
    }
    void reset() const {
        deleteAll(ns());
        insert(BSON("_id" << 0));
    }
};

class PushAllUpsert : public Base {
public:
    void doIt() const {
        _client.update(ns(), BSON("_id" << 0), fromjson("{$pushAll:{a:[5.0,6.0]}}"), true);
    }
    using ReplTests::Base::check;
    void check() const {
        ASSERT_EQUALS(1, count());
        check(fromjson("{'_id':0,a:[4,5,6]}"), one(fromjson("{'_id':0}")));
    }
    void reset() const {
        deleteAll(ns());
        insert(fromjson("{'_id':0,a:[4]}"));
    }
};

class EmptyPushAll : public Base {
public:
    void doIt() const {
        _client.update(ns(), BSON("_id" << 0), fromjson("{$pushAll:{a:[5.0,6.0]}}"));
    }
    using ReplTests::Base::check;
    void check() const {
        ASSERT_EQUALS(1, count());
        check(fromjson("{'_id':0,a:[5,6]}"), one(fromjson("{'_id':0}")));
    }
    void reset() const {
        deleteAll(ns());
        insert(fromjson("{'_id':0}"));
    }
};

class Pull : public Base {
public:
    void doIt() const {
        _client.update(ns(), BSON("_id" << 0), BSON("$pull" << BSON("a" << 4.0)));
    }
    using ReplTests::Base::check;
    void check() const {
        ASSERT_EQUALS(1, count());
        check(fromjson("{'_id':0,a:[5]}"), one(fromjson("{'_id':0}")));
    }
    void reset() const {
        deleteAll(ns());
        insert(fromjson("{'_id':0,a:[4,5]}"));
    }
};

class PullNothing : public Base {
public:
    void doIt() const {
        _client.update(ns(), BSON("_id" << 0), BSON("$pull" << BSON("a" << 6.0)));
    }
    using ReplTests::Base::check;
    void check() const {
        ASSERT_EQUALS(1, count());
        check(fromjson("{'_id':0,a:[4,5]}"), one(fromjson("{'_id':0}")));
    }
    void reset() const {
        deleteAll(ns());
        insert(fromjson("{'_id':0,a:[4,5]}"));
    }
};

class PullAll : public Base {
public:
    void doIt() const {
        _client.update(ns(), BSON("_id" << 0), fromjson("{$pullAll:{a:[4,5]}}"));
    }
    using ReplTests::Base::check;
    void check() const {
        ASSERT_EQUALS(1, count());
        check(fromjson("{'_id':0,a:[6]}"), one(fromjson("{'_id':0}")));
    }
    void reset() const {
        deleteAll(ns());
        insert(fromjson("{'_id':0,a:[4,5,6]}"));
    }
};

class Pop : public Base {
public:
    void doIt() const {
        _client.update(ns(), BSON("_id" << 0), fromjson("{$pop:{a:1}}"));
    }
    using ReplTests::Base::check;
    void check() const {
        ASSERT_EQUALS(1, count());
        check(fromjson("{'_id':0,a:[4,5]}"), one(fromjson("{'_id':0}")));
    }
    void reset() const {
        deleteAll(ns());
        insert(fromjson("{'_id':0,a:[4,5,6]}"));
    }
};

class PopReverse : public Base {
public:
    void doIt() const {
        _client.update(ns(), BSON("_id" << 0), fromjson("{$pop:{a:-1}}"));
    }
    using ReplTests::Base::check;
    void check() const {
        ASSERT_EQUALS(1, count());
        check(fromjson("{'_id':0,a:[5,6]}"), one(fromjson("{'_id':0}")));
    }
    void reset() const {
        deleteAll(ns());
        insert(fromjson("{'_id':0,a:[4,5,6]}"));
    }
};

class BitOp : public Base {
public:
    void doIt() const {
        _client.update(ns(), BSON("_id" << 0), fromjson("{$bit:{a:{and:2,or:8}}}"));
    }
    using ReplTests::Base::check;
    void check() const {
        ASSERT_EQUALS(1, count());
        check(BSON("_id" << 0 << "a" << ((3 & 2) | 8)), one(fromjson("{'_id':0}")));
    }
    void reset() const {
        deleteAll(ns());
        insert(fromjson("{'_id':0,a:3}"));
    }
};

class Rename : public Base {
public:
    void doIt() const {
        _client.update(ns(), BSON("_id" << 0), fromjson("{$rename:{a:'b'}}"));
        _client.update(ns(), BSON("_id" << 0), fromjson("{$set:{a:50}}"));
    }
    using ReplTests::Base::check;
    void check() const {
        ASSERT_EQUALS(1, count());
        ASSERT_EQUALS(mutablebson::unordered(BSON("_id" << 0 << "a" << 50 << "b" << 3)),
                      mutablebson::unordered(one(fromjson("{'_id':0}"))));
    }
    void reset() const {
        deleteAll(ns());
        insert(fromjson("{'_id':0,a:3}"));
    }
};

class RenameReplace : public Base {
public:
    void doIt() const {
        _client.update(ns(), BSON("_id" << 0), fromjson("{$rename:{a:'b'}}"));
        _client.update(ns(), BSON("_id" << 0), fromjson("{$set:{a:50}}"));
    }
    using ReplTests::Base::check;
    void check() const {
        ASSERT_EQUALS(1, count());
        ASSERT_EQUALS(mutablebson::unordered(BSON("_id" << 0 << "a" << 50 << "b" << 3)),
                      mutablebson::unordered(one(fromjson("{'_id':0}"))));
    }
    void reset() const {
        deleteAll(ns());
        insert(fromjson("{'_id':0,a:3,b:100}"));
    }
};

class RenameOverwrite : public Base {
public:
    void doIt() const {
        _client.update(ns(), BSON("_id" << 0), fromjson("{$rename:{a:'b'}}"));
    }
    using ReplTests::Base::check;
    void check() const {
        ASSERT_EQUALS(1, count());
        ASSERT_EQUALS(mutablebson::unordered(BSON("_id" << 0 << "b" << 3 << "z" << 1)),
                      mutablebson::unordered(one(fromjson("{'_id':0}"))));
    }
    void reset() const {
        deleteAll(ns());
        insert(fromjson("{'_id':0,z:1,a:3}"));
    }
};

class NoRename : public Base {
public:
    void doIt() const {
        _client.update(ns(), BSON("_id" << 0), fromjson("{$rename:{c:'b'},$set:{z:1}}"));
    }
    using ReplTests::Base::check;
    void check() const {
        ASSERT_EQUALS(1, count());
        check(BSON("_id" << 0 << "a" << 3 << "z" << 1), one(fromjson("{'_id':0}")));
    }
    void reset() const {
        deleteAll(ns());
        insert(fromjson("{'_id':0,a:3}"));
    }
};

class NestedNoRename : public Base {
public:
    void doIt() const {
        _client.update(ns(), BSON("_id" << 0), fromjson("{$rename:{'a.b':'c.d'},$set:{z:1}}"));
    }
    using ReplTests::Base::check;
    void check() const {
        ASSERT_EQUALS(1, count());
        check(BSON("_id" << 0 << "z" << 1), one(fromjson("{'_id':0}")));
    }
    void reset() const {
        deleteAll(ns());
        insert(fromjson("{'_id':0}"));
    }
};

class SingletonNoRename : public Base {
public:
    void doIt() const {
        _client.update(ns(), BSONObj(), fromjson("{$rename:{a:'b'}}"));
    }
    using ReplTests::Base::check;
    void check() const {
        ASSERT_EQUALS(1, count());
        check(fromjson("{_id:0,z:1}"), one(fromjson("{'_id':0}")));
    }
    void reset() const {
        deleteAll(ns());
        insert(fromjson("{'_id':0,z:1}"));
    }
};

class IndexedSingletonNoRename : public Base {
public:
    void doIt() const {
        _client.update(ns(), BSONObj(), fromjson("{$rename:{a:'b'}}"));
    }
    using ReplTests::Base::check;
    void check() const {
        ASSERT_EQUALS(1, count());
        check(fromjson("{_id:0,z:1}"), one(fromjson("{'_id':0}")));
    }
    void reset() const {
        deleteAll(ns());
        // Add an index on 'a'.  This prevents the update from running 'in place'.
        ASSERT_OK(dbtests::createIndex(&_txn, ns(), BSON("a" << 1)));
        insert(fromjson("{'_id':0,z:1}"));
    }
};

class AddToSetEmptyMissing : public Base {
public:
    void doIt() const {
        _client.update(ns(), BSON("_id" << 0), fromjson("{$addToSet:{a:{$each:[]}}}"));
    }
    using ReplTests::Base::check;
    void check() const {
        ASSERT_EQUALS(1, count());
        check(fromjson("{_id:0,a:[]}"), one(fromjson("{'_id':0}")));
    }
    void reset() const {
        deleteAll(ns());
        insert(fromjson("{'_id':0}"));
    }
};

class AddToSetWithDollarSigns : public Base {
    void doIt() const {
        _client.update(ns(), BSON("_id" << 0), BSON("$addToSet" << BSON("a" << BSON("$foo" << 1))));
    }
    using ReplTests::Base::check;
    void check() const {
        ASSERT_EQUALS(1, count());
        check(fromjson("{'_id':0, a:[0, {'$foo':1}]}"), one(fromjson("{'_id':0}")));
    }
    void reset() const {
        deleteAll(ns());
        insert(BSON("_id" << 0 << "a" << BSON_ARRAY(0)));
    }
};

//
// replay cases
//

class ReplaySetPreexistingNoOpPull : public Base {
public:
    void doIt() const {
        _client.update(ns(), BSONObj(), fromjson("{$unset:{z:1}}"));

        // This is logged as {$set:{'a.b':[]},$set:{z:1}}, which might not be
        // replayable against future versions of a document (here {_id:0,a:1,z:1}) due
        // to SERVER-4781. As a result the $set:{z:1} will not be replayed in such
        // cases (and also an exception may abort replication). If this were instead
        // logged as {$set:{z:1}}, SERVER-4781 would not be triggered.
        _client.update(ns(), BSONObj(), fromjson("{$pull:{'a.b':1}, $set:{z:1}}"));
        _client.update(ns(), BSONObj(), fromjson("{$set:{a:1}}"));
    }
    using ReplTests::Base::check;
    void check() const {
        ASSERT_EQUALS(1, count());
        check(fromjson("{_id:0,a:1,z:1}"), one(fromjson("{'_id':0}")));
    }
    void reset() const {
        deleteAll(ns());
        insert(fromjson("{'_id':0,a:{b:[]},z:1}"));
    }
};

class ReplayArrayFieldNotAppended : public Base {
public:
    void doIt() const {
        _client.update(ns(), BSONObj(), fromjson("{$push:{'a.0.b':2}}"));
        _client.update(ns(), BSONObj(), fromjson("{$set:{'a.0':1}}"));
    }
    using ReplTests::Base::check;
    void check() const {
        ASSERT_EQUALS(1, count());
        check(fromjson("{_id:0,a:[1,{b:[1]}]}"), one(fromjson("{'_id':0}")));
    }
    void reset() const {
        deleteAll(ns());
        insert(fromjson("{'_id':0,a:[{b:[0]},{b:[1]}]}"));
    }
};

}  // namespace Idempotence

class DeleteOpIsIdBased : public Base {
public:
    void run() {
        insert(BSON("_id" << 0 << "a" << 10));
        insert(BSON("_id" << 1 << "a" << 11));
        insert(BSON("_id" << 3 << "a" << 10));
        _client.remove(ns(), BSON("a" << 10));
        ASSERT_EQUALS(1U, _client.count(ns(), BSONObj()));
        insert(BSON("_id" << 0 << "a" << 11));
        insert(BSON("_id" << 2 << "a" << 10));
        insert(BSON("_id" << 3 << "a" << 10));

        applyAllOperations();
        ASSERT_EQUALS(2U, _client.count(ns(), BSONObj()));
        ASSERT(!one(BSON("_id" << 1)).isEmpty());
        ASSERT(!one(BSON("_id" << 2)).isEmpty());
    }
};

class DatabaseIgnorerBasic {
public:
    void run() {
        DatabaseIgnorer d;
        ASSERT(!d.ignoreAt("a", Timestamp(4, 0)));
        d.doIgnoreUntilAfter("a", Timestamp(5, 0));
        ASSERT(d.ignoreAt("a", Timestamp(4, 0)));
        ASSERT(!d.ignoreAt("b", Timestamp(4, 0)));
        ASSERT(d.ignoreAt("a", Timestamp(4, 10)));
        ASSERT(d.ignoreAt("a", Timestamp(5, 0)));
        ASSERT(!d.ignoreAt("a", Timestamp(5, 1)));
        // Ignore state is expired.
        ASSERT(!d.ignoreAt("a", Timestamp(4, 0)));
    }
};

class DatabaseIgnorerUpdate {
public:
    void run() {
        DatabaseIgnorer d;
        d.doIgnoreUntilAfter("a", Timestamp(5, 0));
        d.doIgnoreUntilAfter("a", Timestamp(6, 0));
        ASSERT(d.ignoreAt("a", Timestamp(5, 5)));
        ASSERT(d.ignoreAt("a", Timestamp(6, 0)));
        ASSERT(!d.ignoreAt("a", Timestamp(6, 1)));

        d.doIgnoreUntilAfter("a", Timestamp(5, 0));
        d.doIgnoreUntilAfter("a", Timestamp(6, 0));
        d.doIgnoreUntilAfter("a", Timestamp(6, 0));
        d.doIgnoreUntilAfter("a", Timestamp(5, 0));
        ASSERT(d.ignoreAt("a", Timestamp(5, 5)));
        ASSERT(d.ignoreAt("a", Timestamp(6, 0)));
        ASSERT(!d.ignoreAt("a", Timestamp(6, 1)));
    }
};

class SyncTest : public SyncTail {
public:
    bool returnEmpty;
    SyncTest() : SyncTail(nullptr, SyncTail::MultiSyncApplyFunc()), returnEmpty(false) {}
    virtual ~SyncTest() {}
    virtual BSONObj getMissingDoc(OperationContext* txn, Database* db, const BSONObj& o) {
        if (returnEmpty) {
            BSONObj o;
            return o;
        }
        return BSON("_id"
                    << "on remote"
                    << "foo"
                    << "baz");
    }
};

class ShouldRetry : public Base {
public:
    void run() {
        bool threw = false;
        BSONObj o = BSON("ns" << ns() << "o" << BSON("foo"
                                                     << "bar")
                              << "o2"
                              << BSON("_id"
                                      << "in oplog"
                                      << "foo"
                                      << "bar"));

        ScopedTransaction transaction(&_txn, MODE_X);
        Lock::GlobalWrite lk(_txn.lockState());

        // this should fail because we can't connect
        try {
            SyncTail badSource(nullptr, SyncTail::MultiSyncApplyFunc());
            badSource.setHostname("localhost:123");

            OldClientContext ctx(&_txn, ns());
            badSource.getMissingDoc(&_txn, ctx.db(), o);
        } catch (DBException&) {
            threw = true;
        }
        verify(threw);

        // now this should succeed
        SyncTest t;
        verify(t.shouldRetry(&_txn, o));
        verify(!_client
                    .findOne(ns(),
                             BSON("_id"
                                  << "on remote"))
                    .isEmpty());

        // force it not to find an obj
        t.returnEmpty = true;
        verify(!t.shouldRetry(&_txn, o));
    }
};

class All : public Suite {
public:
    All() : Suite("repl") {}

    void setupTests() {
        add<LogBasic>();
        add<Idempotence::InsertTimestamp>();
        add<Idempotence::InsertAutoId>();
        add<Idempotence::InsertWithId>();
        add<Idempotence::InsertTwo>();
        add<Idempotence::InsertTwoIdentical>();
        add<Idempotence::UpdateTimestamp>();
        add<Idempotence::UpdateSameField>();
        add<Idempotence::UpdateSameFieldWithId>();
        add<Idempotence::UpdateSameFieldExplicitId>();
        add<Idempotence::UpdateDifferentFieldExplicitId>();
        add<Idempotence::UpsertUpdateNoMods>();
        add<Idempotence::UpsertInsertNoMods>();
        add<Idempotence::UpdateSet>();
        add<Idempotence::UpdateInc>();
        add<Idempotence::UpdateInc2>();
        add<Idempotence::IncEmbedded>();  // SERVER-716
        add<Idempotence::IncCreates>();   // SERVER-717
        add<Idempotence::UpsertInsertIdMod>();
        add<Idempotence::UpsertInsertSet>();
        add<Idempotence::UpsertInsertInc>();
        add<Idempotence::MultiInc>();
        // Don't worry about this until someone wants this functionality.
        //            add< Idempotence::UpdateWithoutPreexistingId >();
        add<Idempotence::Remove>();
        add<Idempotence::RemoveOne>();
        add<Idempotence::FailingUpdate>();
        add<Idempotence::SetNumToStr>();
        add<Idempotence::Push>();
        add<Idempotence::PushUpsert>();
        add<Idempotence::MultiPush>();
        add<Idempotence::EmptyPush>();
        add<Idempotence::EmptyPushSparseIndex>();
        add<Idempotence::PushAll>();
        add<Idempotence::PushSlice>();
        add<Idempotence::PushSliceInitiallyInexistent>();
        add<Idempotence::PushSliceToZero>();
        add<Idempotence::PushAllUpsert>();
        add<Idempotence::EmptyPushAll>();
        add<Idempotence::Pull>();
        add<Idempotence::PullNothing>();
        add<Idempotence::PullAll>();
        add<Idempotence::Pop>();
        add<Idempotence::PopReverse>();
        add<Idempotence::BitOp>();
        add<Idempotence::Rename>();
        add<Idempotence::RenameReplace>();
        add<Idempotence::RenameOverwrite>();
        add<Idempotence::NoRename>();
        add<Idempotence::NestedNoRename>();
        add<Idempotence::SingletonNoRename>();
        add<Idempotence::IndexedSingletonNoRename>();
        add<Idempotence::AddToSetEmptyMissing>();
        add<Idempotence::ReplaySetPreexistingNoOpPull>();
        add<Idempotence::ReplayArrayFieldNotAppended>();
        add<DeleteOpIsIdBased>();
        add<DatabaseIgnorerBasic>();
        add<DatabaseIgnorerUpdate>();
        add<ShouldRetry>();
    }
};

SuiteInstance<All> myall;

}  // namespace ReplTests
