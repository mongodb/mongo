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

#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/json.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/oplog_writer_impl.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/transport_layer_asio.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {
namespace repl {
namespace ReplTests {

using std::string;
using std::unique_ptr;
using std::vector;

/**
 * Creates an OplogEntry with given parameters and preset defaults for this test suite.
 */
repl::OplogEntry makeOplogEntry(repl::OpTime opTime,
                                repl::OpTypeEnum opType,
                                NamespaceString nss,
                                BSONObj object,
                                boost::optional<BSONObj> object2) {
    return {
        repl::DurableOplogEntry(opTime,                     // optime
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
                                Date_t(),                   // wall clock time
                                {},                         // statement ids
                                boost::none,    // optime of previous write within same transaction
                                boost::none,    // pre-image optime
                                boost::none,    // post-image optime
                                boost::none,    // ShardId of resharding recipient
                                boost::none,    // _id
                                boost::none)};  // needsRetryImage
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
              ReplicationCoordinator::get(_opCtx.getServiceContext())->getSettings()) {
        auto* const sc = _opCtx.getServiceContext();

        transport::TransportLayerASIO::Options opts;
        opts.mode = transport::TransportLayerASIO::Options::kEgress;
        sc->setTransportLayer(std::make_unique<transport::TransportLayerASIO>(opts, nullptr));
        ASSERT_OK(sc->getTransportLayer()->setup());
        ASSERT_OK(sc->getTransportLayer()->start());

        ReplSettings replSettings;
        replSettings.setReplSetString("rs0/host1");
        ReplicationCoordinator::set(sc,
                                    std::unique_ptr<repl::ReplicationCoordinator>(
                                        new repl::ReplicationCoordinatorMock(sc, replSettings)));
        ASSERT_OK(ReplicationCoordinator::get(sc)->setFollowerMode(MemberState::RS_PRIMARY));

        // Since the Client object persists across tests, even though the global
        // ReplicationCoordinator does not, we need to clear the last op associated with the client
        // to avoid the invariant in ReplClientInfo::setLastOp that the optime only goes forward.
        repl::ReplClientInfo::forClient(_opCtx.getClient()).clearLastOp();

        sc->setOpObserver(std::make_unique<OpObserverImpl>(std::make_unique<OplogWriterImpl>()));

        createOplog(&_opCtx);

        // Prevent upgrading from MODE_IX to MODE_X when deleteAll() is issued.
        Lock::GlobalWrite lk(&_opCtx);
        dbtests::WriteContextForTests ctx(&_opCtx, ns());
        WriteUnitOfWork wuow(&_opCtx);

        CollectionPtr c =
            CollectionCatalog::get(&_opCtx)->lookupCollectionByNamespace(&_opCtx, nss());
        if (!c) {
            c = ctx.db()->createCollection(&_opCtx, nss());
        }

        ASSERT(c->getIndexCatalog()->haveIdIndex(&_opCtx));
        wuow.commit();

        _opCtx.getServiceContext()->getStorageEngine()->setOldestTimestamp(Timestamp(1, 1));

        // Start with a fresh oplog.
        deleteAll(cllNS());
    }

    ~Base() {
        auto* const sc = _opCtx.getServiceContext();
        try {
            deleteAll(ns());
            deleteAll(cllNS());
            repl::ReplicationCoordinator::set(
                sc,
                std::unique_ptr<repl::ReplicationCoordinator>(
                    new repl::ReplicationCoordinatorMock(sc, _defaultReplSettings)));
            repl::ReplicationCoordinator::get(sc)
                ->setFollowerMode(repl::MemberState::RS_PRIMARY)
                .ignore();

            sc->getTransportLayer()->shutdown();
        } catch (...) {
            FAIL("Exception while cleaning up test");
        }
    }

protected:
    virtual OplogApplication::Mode getOplogApplicationMode() {
        return OplogApplication::Mode::kSecondary;
    }
    static const char* ns() {
        return "unittests.repltests";
    }
    static NamespaceString nss() {
        return NamespaceString(ns());
    }
    static const char* cllNS() {
        return "local.oplog.rs";
    }
    BSONObj one(const BSONObj& query = BSONObj()) const {
        return _client.findOne(nss(), query);
    }
    void checkOne(const BSONObj& o) const {
        check(o, one(o));
    }
    void check(const BSONObj& expected, const BSONObj& got) const {
        if (expected.woCompare(got)) {
            LOGV2(22500,
                  "expected: {expected}, got: {got}",
                  "expected"_attr = expected,
                  "got"_attr = got);
        }
        ASSERT_BSONOBJ_EQ(expected, got);
    }
    int count() const {
        Lock::GlobalWrite lk(&_opCtx);
        OldClientContext ctx(&_opCtx, nss());
        Database* db = ctx.db();
        CollectionPtr coll =
            CollectionCatalog::get(&_opCtx)->lookupCollectionByNamespace(&_opCtx, nss());
        if (!coll) {
            WriteUnitOfWork wunit(&_opCtx);
            coll = db->createCollection(&_opCtx, nss());
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
        return DBDirectClient(&_opCtx)
            .find(FindCommandRequest{NamespaceString{cllNS()}})
            ->itcount();
    }
    void applyAllOperations() {
        Lock::GlobalWrite lk(&_opCtx);
        vector<BSONObj> ops;
        {
            DBDirectClient db(&_opCtx);
            auto cursor = db.find(FindCommandRequest{NamespaceString{cllNS()}});
            while (cursor->more()) {
                ops.push_back(cursor->nextSafe());
            }
        }

        if (!serverGlobalParams.enableMajorityReadConcern) {
            if (ops.size() > 0) {
                if (auto tsElem = ops.front()["ts"]) {
                    _opCtx.getServiceContext()->getStorageEngine()->setOldestTimestamp(
                        tsElem.timestamp());
                }
            }
        }

        OldClientContext ctx(&_opCtx, nss());
        for (vector<BSONObj>::iterator i = ops.begin(); i != ops.end(); ++i) {
            if (0) {
                LOGV2(22501, "op: {i}", "i"_attr = *i);
            }
            repl::UnreplicatedWritesBlock uwb(&_opCtx);
            auto entry = uassertStatusOK(OplogEntry::parse(*i));
            // Handle the case of batched writes which generate command-type (applyOps) oplog
            // entries.
            if (entry.getOpType() == repl::OpTypeEnum::kCommand) {
                uassertStatusOK(applyCommand_inlock(&_opCtx, entry, getOplogApplicationMode()));
            } else {
                WriteUnitOfWork wunit(&_opCtx);
                auto lastApplied = repl::ReplicationCoordinator::get(_opCtx.getServiceContext())
                                       ->getMyLastAppliedOpTime()
                                       .getTimestamp();
                auto nextTimestamp = std::max(lastApplied + 1, Timestamp(1, 1));
                ASSERT_OK(_opCtx.recoveryUnit()->setTimestamp(nextTimestamp));
                const bool dataIsConsistent = true;
                uassertStatusOK(applyOperation_inlock(
                    &_opCtx, ctx.db(), &entry, false, getOplogApplicationMode(), dataIsConsistent));
                wunit.commit();
            }
        }
    }
    // These deletes don't get logged.
    void deleteAll(const char* ns) const {
        ::mongo::writeConflictRetry(&_opCtx, "deleteAll", ns, [&] {
            NamespaceString nss(ns);
            Lock::GlobalWrite lk(&_opCtx);
            OldClientContext ctx(&_opCtx, nss);
            WriteUnitOfWork wunit(&_opCtx);
            Database* db = ctx.db();
            Collection* coll =
                CollectionCatalog::get(&_opCtx)->lookupCollectionByNamespaceForMetadataWrite(
                    &_opCtx, nss);
            if (!coll) {
                coll = db->createCollection(&_opCtx, nss);
            }

            auto lastApplied = repl::ReplicationCoordinator::get(_opCtx.getServiceContext())
                                   ->getMyLastAppliedOpTime()
                                   .getTimestamp();
            auto nextTimestamp = std::max(lastApplied + 1, Timestamp(1, 1));

            repl::UnreplicatedWritesBlock uwb(&_opCtx);
            ASSERT_OK(_opCtx.recoveryUnit()->setTimestamp(nextTimestamp));
            ASSERT_OK(coll->truncate(&_opCtx));
            wunit.commit();
        });
    }
    void insert(const BSONObj& o) const {
        Lock::GlobalWrite lk(&_opCtx);
        OldClientContext ctx(&_opCtx, nss());
        WriteUnitOfWork wunit(&_opCtx);
        Database* db = ctx.db();
        CollectionPtr coll =
            CollectionCatalog::get(&_opCtx)->lookupCollectionByNamespace(&_opCtx, nss());
        if (!coll) {
            coll = db->createCollection(&_opCtx, nss());
        }

        auto lastApplied = repl::ReplicationCoordinator::get(_opCtx.getServiceContext())
                               ->getMyLastAppliedOpTime()
                               .getTimestamp();
        // The oplog collection may already have some oplog entries for writes prior to this insert.
        // And the oplog visibility timestamp may already reflect those entries (e.g. no holes exist
        // before lastApplied). Thus, it is invalid to do any timestamped writes using a timestamp
        // less than or equal to the WT "all_durable" timestamp. Therefore, we use the next
        // timestamp of the lastApplied to be safe. In the case where there is no oplog entries in
        // the oplog collection, we will use a non-zero timestamp (Timestamp(1, 1)) for the insert.
        auto nextTimestamp = std::max(lastApplied + 1, Timestamp(1, 1));
        OpDebug* const nullOpDebug = nullptr;
        if (o.hasField("_id")) {
            repl::UnreplicatedWritesBlock uwb(&_opCtx);
            coll->insertDocument(&_opCtx, InsertStatement(o), nullOpDebug, true)
                .transitional_ignore();
            ASSERT_OK(_opCtx.recoveryUnit()->setTimestamp(nextTimestamp));
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
        ASSERT_OK(_opCtx.recoveryUnit()->setTimestamp(nextTimestamp));
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
        ASSERT_EQUALS(0, opCount());
        _client.insert(ns(), fromjson("{\"a\":\"b\"}"));
        ASSERT_EQUALS(1, opCount());
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

// Some operations are only idempotent when in RECOVERING, not in SECONDARY.  This includes
// duplicate inserts and deletes.
class Recovering : public Base {
protected:
    virtual OplogApplication::Mode getOplogApplicationMode() {
        return OplogApplication::Mode::kRecovering;
    }
};

class InsertTimestamp : public Recovering {
public:
    void doIt() const {
        BSONObjBuilder b;
        b.append("a", 1);
        b.appendTimestamp("t");
        _client.insert(ns(), b.done());
        date_ = _client.findOne(nss(), BSON("a" << 1)).getField("t").date();
    }
    void check() const {
        BSONObj o = _client.findOne(nss(), BSON("a" << 1));
        ASSERT(Date_t{} != o.getField("t").date());
        ASSERT_EQUALS(date_, o.getField("t").date());
    }
    void reset() const {
        deleteAll(ns());
    }

private:
    mutable Date_t date_;
};

class InsertAutoId : public Recovering {
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

class InsertTwo : public Recovering {
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

class InsertTwoIdentical : public Recovering {
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
        date_ = _client.findOne(nss(), BSON("_id" << 1)).getField("t").date();
    }
    void check() const {
        BSONObj o = _client.findOne(nss(), BSON("_id" << 1));
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
        ASSERT(!_client.findOne(nss(), q_).isEmpty());
        ASSERT(!_client.findOne(nss(), u_).isEmpty());
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
        ASSERT(!_client.findOne(nss(), q_).isEmpty());
        ASSERT(!_client.findOne(nss(), u_).isEmpty());
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


class UpsertInsertIdMod : public Recovering {
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

class UpsertInsertSet : public Recovering {
public:
    UpsertInsertSet()
        : q_(fromjson("{a:5}")), u_(fromjson("{$set:{a:7}}")), ou_(fromjson("{a:7}")) {}
    void doIt() const {
        _client.update(ns(), q_, u_, true);
    }
    void check() const {
        ASSERT_EQUALS(2, count());
        ASSERT(!_client.findOne(nss(), ou_).isEmpty());
    }
    void reset() const {
        deleteAll(ns());
        insert(fromjson("{'_id':7,a:7}"));
    }

protected:
    BSONObj o_, q_, u_, ou_;
};

class UpsertInsertInc : public Recovering {
public:
    UpsertInsertInc()
        : q_(fromjson("{a:5}")), u_(fromjson("{$inc:{a:3}}")), ou_(fromjson("{a:8}")) {}
    void doIt() const {
        _client.update(ns(), q_, u_, true);
    }
    void check() const {
        ASSERT_EQUALS(1, count());
        ASSERT(!_client.findOne(nss(), ou_).isEmpty());
    }
    void reset() const {
        deleteAll(ns());
    }

protected:
    BSONObj o_, q_, u_, ou_;
};

class MultiInc : public Recovering {
public:
    string s() const {
        StringBuilder ss;
        FindCommandRequest findRequest{NamespaceString{ns()}};
        findRequest.setSort(BSON("_id" << 1));
        std::unique_ptr<DBClientCursor> cc = _client.find(std::move(findRequest));
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

class Remove : public Recovering {
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
        _client.remove(ns(), q_, false /*removeMany*/);
    }
    void check() const {
        ASSERT_EQUALS(1, count());
    }
};

class FailingUpdate : public Recovering {
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
        // These inserts don't write oplog entries.
        insert(BSON("_id" << 0 << "a" << 10));
        insert(BSON("_id" << 1 << "a" << 11));
        insert(BSON("_id" << 3 << "a" << 10));
        _client.remove(ns(), BSON("a" << 10));
        ASSERT_EQUALS(1U, _client.count(nss(), BSONObj()));
        insert(BSON("_id" << 0 << "a" << 11));
        insert(BSON("_id" << 2 << "a" << 10));
        insert(BSON("_id" << 3 << "a" << 10));
        // Now the collection has _ids 0, 1, 2, 3. Apply the delete oplog entries for _id 0 and 3.
        applyAllOperations();
        // _id 1 and 2 remain.
        ASSERT_EQUALS(2U, _client.count(nss(), BSONObj()));
        ASSERT(!one(BSON("_id" << 1)).isEmpty());
        ASSERT(!one(BSON("_id" << 2)).isEmpty());
    }
};

class All : public OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("repl") {}

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
    }
};

OldStyleSuiteInitializer<All> myall;

}  // namespace ReplTests
}  // namespace repl
}  // namespace mongo
