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
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/json.h"
#include "mongo/db/op_observer_impl.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/sync_tail.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/transport/transport_layer_asio.h"
#include "mongo/util/log.h"

using namespace mongo::repl;

namespace ReplTests {

using std::unique_ptr;
using std::endl;
using std::string;
using std::stringstream;
using std::vector;

/**
 * Creates an OplogEntry with given parameters and preset defaults for this test suite.
 */
repl::OplogEntry makeOplogEntry(repl::OpTime opTime,
                                repl::OpTypeEnum opType,
                                NamespaceString nss,
                                BSONObj object,
                                boost::optional<BSONObj> object2) {
    return repl::OplogEntry(opTime,                     // optime
                            0,                          // hash
                            opType,                     // opType
                            nss,                        // namespace
                            boost::none,                // uuid
                            boost::none,                // fromMigrate
                            OplogEntry::kOplogVersion,  // version
                            object,                     // o
                            object2,                    // o2
                            {},                         // sessionInfo
                            boost::none,                // upsert
                            boost::none,                // wall clock time
                            boost::none,                // statement id
                            boost::none,   // optime of previous write within same transaction
                            boost::none,   // pre-image optime
                            boost::none);  // post-image optime
}

BSONObj f(const char* s) {
    return fromjson(s);
}

class Base {
protected:
    const ServiceContext::UniqueOperationContext _opCtxPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_opCtxPtr;
    mutable DBDirectClient _client;
    ReplSettings _defaultReplSettings;

public:
    Base()
        : _client(&_opCtx),
          _defaultReplSettings(
              ReplicationCoordinator::get(getGlobalServiceContext())->getSettings()) {
        // Replication is not supported by mobile SE.
        if (mongo::storageGlobalParams.engine == "mobile") {
            return;
        }

        transport::TransportLayerASIO::Options opts;
        opts.mode = transport::TransportLayerASIO::Options::kEgress;
        auto sc = getGlobalServiceContext();

        sc->setTransportLayer(std::make_unique<transport::TransportLayerASIO>(opts, nullptr));
        ASSERT_OK(sc->getTransportLayer()->setup());
        ASSERT_OK(sc->getTransportLayer()->start());

        ReplSettings replSettings;
        replSettings.setReplSetString("rs0/host1");
        ReplicationCoordinator::set(
            getGlobalServiceContext(),
            std::unique_ptr<repl::ReplicationCoordinator>(
                new repl::ReplicationCoordinatorMock(_opCtx.getServiceContext(), replSettings)));
        ASSERT_OK(ReplicationCoordinator::get(getGlobalServiceContext())
                      ->setFollowerMode(MemberState::RS_PRIMARY));

        // Since the Client object persists across tests, even though the global
        // ReplicationCoordinator does not, we need to clear the last op associated with the client
        // to avoid the invariant in ReplClientInfo::setLastOp that the optime only goes forward.
        repl::ReplClientInfo::forClient(_opCtx.getClient()).clearLastOp_forTest();

        getGlobalServiceContext()->setOpObserver(stdx::make_unique<OpObserverImpl>());

        setOplogCollectionName(getGlobalServiceContext());
        createOplog(&_opCtx);

        OldClientWriteContext ctx(&_opCtx, ns());
        WriteUnitOfWork wuow(&_opCtx);

        Collection* c = ctx.db()->getCollection(&_opCtx, ns());
        if (!c) {
            c = ctx.db()->createCollection(&_opCtx, ns());
        }

        ASSERT(c->getIndexCatalog()->haveIdIndex(&_opCtx));
        wuow.commit();
    }
    ~Base() {
        // Replication is not supported by mobile SE.
        if (mongo::storageGlobalParams.engine == "mobile") {
            return;
        }
        try {
            deleteAll(ns());
            deleteAll(cllNS());
            repl::ReplicationCoordinator::set(
                getGlobalServiceContext(),
                std::unique_ptr<repl::ReplicationCoordinator>(new repl::ReplicationCoordinatorMock(
                    _opCtx.getServiceContext(), _defaultReplSettings)));
            repl::ReplicationCoordinator::get(getGlobalServiceContext())
                ->setFollowerMode(repl::MemberState::RS_PRIMARY)
                .ignore();

            getGlobalServiceContext()->getTransportLayer()->shutdown();

        } catch (...) {
            FAIL("Exception while cleaning up test");
        }
    }

protected:
    static const char* ns() {
        return "unittests.repltests";
    }
    static const char* cllNS() {
        return "local.oplog.rs";
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
        ASSERT_BSONOBJ_EQ(expected, got);
    }
    BSONObj oneOp() const {
        return _client.findOne(cllNS(), BSONObj());
    }
    int count() const {
        Lock::GlobalWrite lk(&_opCtx);
        OldClientContext ctx(&_opCtx, ns());
        Database* db = ctx.db();
        Collection* coll = db->getCollection(&_opCtx, ns());
        if (!coll) {
            WriteUnitOfWork wunit(&_opCtx);
            coll = db->createCollection(&_opCtx, ns());
            wunit.commit();
        }

        int count = 0;
        auto cursor = coll->getCursor(&_opCtx);
        while (auto record = cursor->next()) {
            ++count;
        }
        return count;
    }
    int opCount() {
        return DBDirectClient(&_opCtx).query(cllNS(), BSONObj())->itcount();
    }
    void applyAllOperations() {
        Lock::GlobalWrite lk(&_opCtx);
        vector<BSONObj> ops;
        {
            DBDirectClient db(&_opCtx);
            auto cursor = db.query(cllNS(), BSONObj());
            while (cursor->more()) {
                ops.push_back(cursor->nextSafe());
            }
        }
        {
            OldClientContext ctx(&_opCtx, ns());
            for (vector<BSONObj>::iterator i = ops.begin(); i != ops.end(); ++i) {
                if (0) {
                    mongo::unittest::log() << "op: " << *i << endl;
                }
                repl::UnreplicatedWritesBlock uwb(&_opCtx);
                uassertStatusOK(applyOperation_inlock(
                    &_opCtx, ctx.db(), *i, false, OplogApplication::Mode::kSecondary));
            }
        }
    }
    void printAll(const char* ns) {
        Lock::GlobalWrite lk(&_opCtx);
        OldClientContext ctx(&_opCtx, ns);

        Database* db = ctx.db();
        Collection* coll = db->getCollection(&_opCtx, ns);
        if (!coll) {
            WriteUnitOfWork wunit(&_opCtx);
            coll = db->createCollection(&_opCtx, ns);
            wunit.commit();
        }

        auto cursor = coll->getCursor(&_opCtx);
        ::mongo::log() << "all for " << ns << endl;
        while (auto record = cursor->next()) {
            ::mongo::log() << record->data.releaseToBson() << endl;
        }
    }
    // These deletes don't get logged.
    void deleteAll(const char* ns) const {
        ::mongo::writeConflictRetry(&_opCtx, "deleteAll", ns, [&] {
            Lock::GlobalWrite lk(&_opCtx);
            OldClientContext ctx(&_opCtx, ns);
            WriteUnitOfWork wunit(&_opCtx);
            Database* db = ctx.db();
            Collection* coll = db->getCollection(&_opCtx, ns);
            if (!coll) {
                coll = db->createCollection(&_opCtx, ns);
            }

            ASSERT_OK(coll->truncate(&_opCtx));
            wunit.commit();
        });
    }
    void insert(const BSONObj& o) const {
        Lock::GlobalWrite lk(&_opCtx);
        OldClientContext ctx(&_opCtx, ns());
        WriteUnitOfWork wunit(&_opCtx);
        Database* db = ctx.db();
        Collection* coll = db->getCollection(&_opCtx, ns());
        if (!coll) {
            coll = db->createCollection(&_opCtx, ns());
        }

        OpDebug* const nullOpDebug = nullptr;
        if (o.hasField("_id")) {
            repl::UnreplicatedWritesBlock uwb(&_opCtx);
            coll->insertDocument(&_opCtx, InsertStatement(o), nullOpDebug, true)
                .transitional_ignore();
            wunit.commit();
            return;
        }

        class BSONObjBuilder b;
        OID id;
        id.init();
        b.appendOID("_id", &id);
        b.appendElements(o);
        repl::UnreplicatedWritesBlock uwb(&_opCtx);
        coll->insertDocument(&_opCtx, InsertStatement(b.obj()), nullOpDebug, true)
            .transitional_ignore();
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
        // Replication is not supported by mobile SE.
        if (mongo::storageGlobalParams.engine == "mobile") {
            return;
        }
        ASSERT_EQUALS(1, opCount());
        _client.insert(ns(), fromjson("{\"a\":\"b\"}"));
        ASSERT_EQUALS(2, opCount());
    }
};

namespace Idempotence {

class Base : public ReplTests::Base {
public:
    virtual ~Base() {}
    void run() {
        // Replication is not supported by mobile SE.
        if (mongo::storageGlobalParams.engine == "mobile") {
            return;
        }
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
        ASSERT_OK(dbtests::createIndex(&_opCtx, ns(), BSON("a" << 1)));
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
        // Replication is not supported by mobile SE.
        if (mongo::storageGlobalParams.engine == "mobile") {
            return;
        }
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

class SyncTest : public SyncTail {
public:
    bool returnEmpty;
    explicit SyncTest(OplogApplier::Observer* observer)
        : SyncTail(observer, nullptr, nullptr, SyncTail::MultiSyncApplyFunc(), nullptr),
          returnEmpty(false) {}
    virtual ~SyncTest() {}
    BSONObj getMissingDoc(OperationContext* opCtx, const OplogEntry& oplogEntry) override {
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

class FetchAndInsertMissingDocumentObserver : public OplogApplier::Observer {
public:
    void onBatchBegin(const OplogApplier::Operations&) final {}
    void onBatchEnd(const StatusWith<OpTime>&, const OplogApplier::Operations&) final {}
    void onMissingDocumentsFetchedAndInserted(const std::vector<FetchInfo>&) final {
        fetched = true;
    }
    bool fetched = false;
};

class FetchAndInsertMissingDocument : public Base {
public:
    void run() {
        // Replication is not supported by mobile SE.
        if (mongo::storageGlobalParams.engine == "mobile") {
            return;
        }
        bool threw = false;
        auto oplogEntry = makeOplogEntry(OpTime(Timestamp(100, 1), 1LL),  // optime
                                         OpTypeEnum::kUpdate,             // op type
                                         NamespaceString(ns()),           // namespace
                                         BSON("foo"
                                              << "bar"),  // o
                                         BSON("_id"
                                              << "in oplog"
                                              << "foo"
                                              << "bar"));  // o2

        Lock::GlobalWrite lk(&_opCtx);

        // this should fail because we can't connect
        try {
            OplogApplier::Options options;
            options.allowNamespaceNotFoundErrorsOnCrudOps = true;
            options.missingDocumentSourceForInitialSync = HostAndPort("localhost", 123);
            SyncTail badSource(
                nullptr, nullptr, nullptr, SyncTail::MultiSyncApplyFunc(), nullptr, options);

            OldClientContext ctx(&_opCtx, ns());
            badSource.getMissingDoc(&_opCtx, oplogEntry);
        } catch (DBException&) {
            threw = true;
        }
        verify(threw);

        // now this should succeed
        FetchAndInsertMissingDocumentObserver observer;
        SyncTest t(&observer);
        verify(t.fetchAndInsertMissingDocument(&_opCtx, oplogEntry));
        ASSERT(observer.fetched);
        verify(!_client
                    .findOne(ns(),
                             BSON("_id"
                                  << "on remote"))
                    .isEmpty());

        // Reset flag in observer before next test case.
        observer.fetched = false;

        // force it not to find an obj
        t.returnEmpty = true;
        verify(!t.fetchAndInsertMissingDocument(&_opCtx, oplogEntry));
        ASSERT_FALSE(observer.fetched);
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
        add<Idempotence::PushSlice>();
        add<Idempotence::PushSliceInitiallyInexistent>();
        add<Idempotence::PushSliceToZero>();
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
        add<FetchAndInsertMissingDocument>();
    }
};

SuiteInstance<All> myall;

}  // namespace ReplTests
