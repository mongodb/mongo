// replsettests.cpp : Unit tests for replica sets
//

/**
 *    Copyright (C) 2009 10gen Inc.
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

#include "mongo/pch.h"

#include "mongo/db/db.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/instance.h"
#include "mongo/db/json.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_server_status.h"  // replSettings
#include "mongo/db/repl/rs.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/util/time_support.h"


namespace mongo {
    void createOplog();
}

namespace ReplSetTests {
    const int replWriterThreadCount(32);
    const int replPrefetcherThreadCount(32);
    class ReplSetTest : public ReplSet {
        ReplSetConfig *_config;
        ReplSetConfig::MemberCfg *_myConfig;
        replset::BackgroundSyncInterface *_syncTail;
    public:
        static const int replWriterThreadCount;
        static const int replPrefetcherThreadCount;
        static ReplSetTest* make() {
            auto_ptr<ReplSetTest> ret(new ReplSetTest());
            ret->init();
            return ret.release();
        }
        virtual ~ReplSetTest() {
            delete _myConfig;
            delete _config;
        }
        virtual bool isSecondary() {
            return true;
        }
        virtual bool isPrimary() {
            return false;
        }
        virtual bool tryToGoLiveAsASecondary(OpTime& minvalid) {
            return false;
        }
        virtual const ReplSetConfig& config() {
            return *_config;
        }
        virtual const ReplSetConfig::MemberCfg& myConfig() {
            return *_myConfig;
        }
        virtual bool buildIndexes() const {
            return true;
        }
        void setSyncTail(replset::BackgroundSyncInterface *syncTail) {
            _syncTail = syncTail;
        }
    private:
        ReplSetTest() :
            _syncTail(0) {
        }
        void init() {
            BSONArrayBuilder members;
            members.append(BSON("_id" << 0 << "host" << "host1"));
            _config = ReplSetConfig::make(BSON("_id" << "foo" << "members" << members.arr()));
            _myConfig = new ReplSetConfig::MemberCfg();
        }
    };

    class BackgroundSyncTest : public replset::BackgroundSyncInterface {
        std::queue<BSONObj> _queue;
    public:
        BackgroundSyncTest() {}
        virtual ~BackgroundSyncTest() {}
        virtual bool peek(BSONObj* op) {
            if (_queue.empty()) {
                return false;
            }
            *op = _queue.front();
            return true;
        }
        virtual void consume() {
            _queue.pop();
        }
        virtual Member* getSyncTarget() {
            return 0;
        }
        void addDoc(BSONObj doc) {
            _queue.push(doc.getOwned());
        }
        virtual void waitForMore() {
            return;
        }
    };


    class Base {
    private:
        static DBDirectClient client_;
    protected:
        static BackgroundSyncTest* _bgsync;
        static replset::SyncTail* _tailer;
    public:
        Base() {
        }
        ~Base() {
        }

        static const char *ns() {
            return "unittests.repltests";
        }

        DBDirectClient *client() const { return &client_; }

        static void insert( const BSONObj &o, bool god = false ) {
            Lock::DBWrite lk(ns());
            Client::Context ctx(ns());
            Database* db = ctx.db();
            Collection* coll = db->getCollection(ns());
            if (!coll) {
                coll = db->createCollection(ns());
            }

            if (o.hasField("_id")) {
                coll->insertDocument(o, true);
                return;
            }

            class BSONObjBuilder b;
            OID id;
            id.init();
            b.appendOID("_id", &id);
            b.appendElements(o);
            coll->insertDocument(b.obj(), true);
        }

        BSONObj findOne( const BSONObj &query = BSONObj() ) const {
            return client()->findOne( ns(), query );
        }

        void drop() {
            Client::WriteContext c(ns());
            Database* db = c.ctx().db();

            if ( db->getCollection( ns() ) == NULL ) {
                return;
            }

            db->dropCollection(ns());
        }
        static void setup() {
            replSettings.replSet = "foo";
            replSettings.oplogSize = 5 * 1024 * 1024;
            createOplog();

            // setup background sync instance
            _bgsync = new BackgroundSyncTest();

            // setup tail
            _tailer = new replset::SyncTail(_bgsync);

            // setup theReplSet
            ReplSetTest *rst = ReplSetTest::make();
            rst->setSyncTail(_bgsync);

            delete theReplSet;
            theReplSet = rst;
        }
    };

    DBDirectClient Base::client_;
    BackgroundSyncTest* Base::_bgsync = NULL;
    replset::SyncTail* Base::_tailer = NULL;

    class MockInitialSync : public replset::InitialSync {
        int step;
    public:
        MockInitialSync() : InitialSync(0), step(0), failOnStep(SUCCEED), retry(true) {}

        enum FailOn {SUCCEED, FAIL_FIRST_APPLY, FAIL_BOTH_APPLY};

        FailOn failOnStep;
        bool retry;

        // instead of actually applying operations, we return success or failure
        virtual bool syncApply(const BSONObj& o, bool convertUpdateToUpsert) {
            step++;

            if ((failOnStep == FAIL_FIRST_APPLY && step == 1) ||
                (failOnStep == FAIL_BOTH_APPLY)) {
                return false;
            }

            return true;
        }

        virtual bool shouldRetry(const BSONObj& o) {
            return retry;
        }
    };

    class TestInitApplyOp : public Base {
    public:
        void run() {

            OpTime o;

            {
                mongo::mutex::scoped_lock lk2(OpTime::m);
                o = OpTime::now(lk2);
            }

            BSONObjBuilder b;
            b.append("ns","dummy");
            b.appendTimestamp("ts", o.asLL());
            BSONObj obj = b.obj();
            MockInitialSync mock;

            // all three should succeed
            std::vector<BSONObj> ops;
            ops.push_back(obj);
            replset::multiInitialSyncApply(ops, &mock);

            mock.failOnStep = MockInitialSync::FAIL_FIRST_APPLY;
            replset::multiInitialSyncApply(ops, &mock);

            mock.retry = false;
            replset::multiInitialSyncApply(ops, &mock);

            drop();
        }
    };

    class SyncTest2 : public replset::InitialSync {
    public:
        bool insertOnRetry;
        SyncTest2() : InitialSync(0), insertOnRetry(false) {}
        virtual ~SyncTest2() {}
        virtual bool shouldRetry(const BSONObj& o) {
            if (!insertOnRetry) {
                return true;
            }

            Base::insert(BSON("_id" << 123));
            return true;
        }
    };

    class TestInitApplyOp2 : public Base {
    public:
        void run() {
            OpTime o = OpTime::_now();

            BSONObjBuilder b;
            b.appendTimestamp("ts", o.asLL());
            b.append("op", "u");
            b.append("o", BSON("$set" << BSON("x" << 456)));
            b.append("o2", BSON("_id" << 123));
            b.append("ns", ns());
            BSONObj obj = b.obj();
            SyncTest2 sync2;
            std::vector<BSONObj> ops;
            ops.push_back(obj);

            sync2.insertOnRetry = true;
            // succeeds
            multiInitialSyncApply(ops, &sync2);

            BSONObj fin = findOne();
            verify(fin["x"].Number() == 456);

            drop();
        }
    };

    class CappedInitialSync : public Base {
        string _cappedNs;
        Lock::DBWrite _lk;

        string spec() const {
            return "{\"capped\":true,\"size\":512}";
        }

        void create() {
            Client::Context c(_cappedNs);
            string err;
            ASSERT(userCreateNS( _cappedNs.c_str(), fromjson( spec() ), err, false ));
        }

        void dropCapped() {
            Client::Context c(_cappedNs);
            Database* db = c.db();
            if ( db->getCollection( _cappedNs ) ) {
                db->dropCollection( _cappedNs );
            }
        }

        BSONObj updateFail() {
            BSONObjBuilder b;
            {
                mongo::mutex::scoped_lock lk2(OpTime::m);
                b.appendTimestamp("ts", OpTime::now(lk2).asLL());
            }
            b.append("op", "u");
            b.append("o", BSON("$set" << BSON("x" << 456)));
            b.append("o2", BSON("_id" << 123 << "x" << 123));
            b.append("ns", _cappedNs);
            BSONObj o = b.obj();

            verify(!apply(o));
            return o;
        }
    public:
        CappedInitialSync() : _cappedNs("unittests.foo.bar"), _lk(_cappedNs) {
            dropCapped();
            create();
        }
        virtual ~CappedInitialSync() {
            dropCapped();
        }

        string& cappedNs() {
            return _cappedNs;
        }

        // returns true on success, false on failure
        bool apply(const BSONObj& op) {
            Client::Context ctx( _cappedNs );
            // in an annoying twist of api, returns true on failure
            return !applyOperation_inlock(op, true);
        }

        void run() {
            Lock::DBWrite lk(_cappedNs);

            BSONObj op = updateFail();

            Sync s("");
            verify(!s.shouldRetry(op));
        }
    };

    class CappedUpdate : public CappedInitialSync {
        void updateSucceed() {
            BSONObjBuilder b;
            {
                mongo::mutex::scoped_lock lk2(OpTime::m);
                b.appendTimestamp("ts", OpTime::now(lk2).asLL());
            }
            b.append("op", "u");
            b.append("o", BSON("$set" << BSON("x" << 789)));
            b.append("o2", BSON("x" << 456));
            b.append("ns", cappedNs());

            verify(apply(b.obj()));
        }

        void insert() {
            Client::Context ctx(cappedNs());
            Database* db = ctx.db();
            Collection* coll = db->getCollection(cappedNs());
            if (!coll) {
                coll = db->createCollection(cappedNs());
            }

            BSONObj o = BSON(GENOID << "x" << 456);
            DiskLoc loc = coll->insertDocument(o, true).getValue();
            verify(!loc.isNull());
        }
    public:
        virtual ~CappedUpdate() {}
        void run() {
            // RARELY shoud be once/128x
            for (int i=0; i<150; i++) {
                insert();
                updateSucceed();
            }

            DBDirectClient client;
            int count = (int) client.count(cappedNs(), BSONObj());
            verify(count > 1);

            // check _id index created
            Client::Context ctx(cappedNs());
            Collection* collection = ctx.db()->getCollection( cappedNs() );
            verify(collection->getIndexCatalog()->findIdIndex());
        }
    };

    class CappedInsert : public CappedInitialSync {
        void insertSucceed() {
            BSONObjBuilder b;
            {
                mongo::mutex::scoped_lock lk2(OpTime::m);
                b.appendTimestamp("ts", OpTime::now(lk2).asLL());
            }
            b.append("op", "i");
            b.append("o", BSON("_id" << 123 << "x" << 456));
            b.append("ns", cappedNs());
            verify(apply(b.obj()));
        }
    public:
        virtual ~CappedInsert() {}
        void run() {
            // This will succeed, but not insert anything because they are changed to upserts
            for (int i=0; i<150; i++) {
                insertSucceed();
            }

            // this changed in 2.1.2
            // we now have indexes on capped collections
            Client::Context ctx(cappedNs());
            Collection* collection = ctx.db()->getCollection( cappedNs() );
            verify(collection->getIndexCatalog()->findIdIndex());
        }
    };

    class TestRSSync : public Base {

        void addOp(const string& op, BSONObj o, BSONObj* o2 = NULL, const char* coll = NULL,
                   int version = 0) {
            OpTime ts;
            {
                Lock::GlobalWrite lk;
                ts = OpTime::_now();
            }

            BSONObjBuilder b;
            b.appendTimestamp("ts", ts.asLL());
            if (version != 0) {
                b.append("v", version);
            }
            b.append("op", op);
            b.append("o", o);

            if (o2) {
                b.append("o2", *o2);
            }

            if (coll) {
                b.append("ns", coll);
            }
            else {
                b.append("ns", ns());
            }

            _bgsync->addDoc(b.done());
        }

        void addInserts(int expected) {
            for (int i=0; i<expected; i++) {
                addOp("i", BSON("_id" << i << "x" << 789));
            }
        }

        void addVersionedInserts(int expected) {
            for (int i=0; i < expected; i++) {
                addOp("i", BSON("_id" << i << "x" << 789), NULL, NULL, i);
            }
        }

        void addUpdates() {
            BSONObj id = BSON("_id" << "123456something");
            addOp("i", id);

            addOp("u", BSON("$set" << BSON("requests.1000001_2" << BSON(
                    "id" << "1000001_2" <<
                    "timestamp" << 1334813340))), &id);

            addOp("u", BSON("$set" << BSON("requests.1000002_2" << BSON(
                    "id" << "1000002_2" <<
                    "timestamp" << 1334813368))), &id);

            addOp("u", BSON("$set" << BSON("requests.100002_1" << BSON(
                    "id" << "100002_1" <<
                    "timestamp" << 1334810820))), &id);
        }

        void addConflictingUpdates() {
            BSONObj first = BSON("_id" << "asdfasdfasdf");
            addOp("i", first);

            BSONObj filter = BSON("_id" << "asdfasdfasdf" << "sp" << BSON("$size" << 2));
            // Test an op with no version, op is ignored and replication continues (code assumes
            // version 1)
            addOp("u", BSON("$push" << BSON("sp" << 42)), &filter, NULL, 0);
            // The following line generates an fassert because it's version 2
            //addOp("u", BSON("$push" << BSON("sp" << 42)), &filter, NULL, 2);
        }

        void addUniqueIndex() {
            addOp("i", BSON("ns" << ns() << "key" << BSON("x" << 1) << "name" << "x1" << "unique" << true), 0, "unittests.system.indexes");
            addInserts(2);
        }

        void applyOplog() {
            _tailer->oplogApplication();
        }
    public:
        void run() {
            const int expected = 100;

            drop();
            addInserts(100);
            applyOplog();

            ASSERT_EQUALS(expected, static_cast<int>(client()->count(ns())));

            drop();
            addVersionedInserts(100);
            applyOplog();

            ASSERT_EQUALS(expected, static_cast<int>(client()->count(ns())));

            drop();
            addUpdates();
            applyOplog();

            BSONObj obj = findOne();

            ASSERT_EQUALS(1334813340, obj["requests"]["1000001_2"]["timestamp"].number());
            ASSERT_EQUALS(1334813368, obj["requests"]["1000002_2"]["timestamp"].number());
            ASSERT_EQUALS(1334810820, obj["requests"]["100002_1"]["timestamp"].number());

            drop();

            // test converting updates to upserts but only for version 2.2.1 and greater,
            // which means oplog version 2 and greater.
            addConflictingUpdates();
            applyOplog();

            drop();

        }
    };

    class All : public Suite {
    public:
        All() : Suite( "replset" ) {
        }

        void setupTests() {
            Base::setup();
            add< TestInitApplyOp >();
            add< TestInitApplyOp2 >();
            add< CappedInitialSync >();
            add< CappedUpdate >();
            add< CappedInsert >();
            add< TestRSSync >();
        }
    } myall;
}
