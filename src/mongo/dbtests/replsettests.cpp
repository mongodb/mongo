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
 */

#include "pch.h"
#include "../db/repl.h"

#include "../db/db.h"
#include "../db/instance.h"
#include "../db/json.h"

#include "dbtests.h"
#include "../db/oplog.h"

#include "mongo/db/repl/rs.h"
#include "mongo/db/repl/bgsync.h"

namespace mongo {
    void createOplog();
}

namespace ReplSetTests {

    class Base {
        static DBDirectClient client_;
    public:
        Base() {
            cmdLine._replSet = "foo";
            cmdLine.oplogSize = 5;
            createOplog();
        }

        static const char *ns() {
            return "unittests.repltests";
        }

        DBDirectClient *client() const { return &client_; }

        static void insert( const BSONObj &o, bool god = false ) {
            Lock::DBWrite lk(ns());
            Client::Context ctx( ns() );
            theDataFileMgr.insert( ns(), o.objdata(), o.objsize(), god );
        }

        BSONObj findOne( const BSONObj &query = BSONObj() ) const {
            return client()->findOne( ns(), query );
        }

        void drop() {
            Client::WriteContext c(ns());
            string errmsg;
            BSONObjBuilder result;

            if (nsdetails(ns()) == NULL) {
                return;
            }

            dropCollection( string(ns()), errmsg, result );
        }
    };
    DBDirectClient Base::client_;


    class MockInitialSync : public replset::InitialSync {
        int step;
    public:
        MockInitialSync() : InitialSync(0), step(0), failOnStep(SUCCEED), retry(true) {}

        enum FailOn {SUCCEED, FAIL_FIRST_APPLY, FAIL_BOTH_APPLY};

        FailOn failOnStep;
        bool retry;

        // instead of actually applying operations, we return success or failure
        virtual bool syncApply(const BSONObj& o) {
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
            Lock::GlobalWrite lk;

            OpTime o;

            {
                mongo::mutex::scoped_lock lk2(OpTime::m);
                o = OpTime::now(lk2);
            }

            BSONObjBuilder b;
            b.appendTimestamp("ts", o.asLL());
            BSONObj obj = b.obj();

            MockInitialSync mock;

            // all three should succeed
            mock.applyOp(obj);

            mock.failOnStep = MockInitialSync::FAIL_FIRST_APPLY;
            mock.applyOp(obj);

            mock.retry = false;
            mock.applyOp(obj);

            // force failure
            MockInitialSync mock2;
            mock2.failOnStep = MockInitialSync::FAIL_BOTH_APPLY;

            ASSERT_THROWS(mock2.applyOp(obj), UserException);
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
            Lock::GlobalWrite lk;

            OpTime o = OpTime::_now();

            BSONObjBuilder b;
            b.appendTimestamp("ts", o.asLL());
            b.append("op", "u");
            b.append("o", BSON("$set" << BSON("x" << 456)));
            b.append("o2", BSON("_id" << 123));
            b.append("ns", ns());
            BSONObj obj = b.obj();

            SyncTest2 sync;
            ASSERT_THROWS(sync.applyOp(obj), UserException);

            sync.insertOnRetry = true;
            // succeeds
            sync.applyOp(obj);

            BSONObj fin = findOne();
            verify(fin["x"].Number() == 456);
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
            if (nsdetails(_cappedNs.c_str()) != NULL) {
                string errmsg;
                BSONObjBuilder result;
                dropCollection( string(_cappedNs), errmsg, result );
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

    // check that applying ops doesn't cause _id index to be created

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
            Client::Context ctx( cappedNs() );
            BSONObj o = BSON("x" << 456);
            DiskLoc loc = theDataFileMgr.insert( cappedNs().c_str(), o.objdata(), o.objsize(), false );
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

            // Just to be sure, no _id index, right?
            Client::Context ctx(cappedNs());
            NamespaceDetails *nsd = nsdetails(cappedNs().c_str());
            verify(nsd->findIdIndex() == -1);
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
            // we know have indexes on capped collections
            Client::Context ctx(cappedNs());
            NamespaceDetails *nsd = nsdetails(cappedNs().c_str());
            verify(nsd->findIdIndex() >= 0);
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
    };

    class ReplSetTest : public ReplSet {
        ReplSetConfig *_config;
        ReplSetConfig::MemberCfg *_myConfig;
        replset::BackgroundSyncInterface *_syncTail;
    public:
        virtual ~ReplSetTest() {
            delete _myConfig;
            delete _config;
        }
        ReplSetTest() : _syncTail(0) {
            BSONArrayBuilder members;
            members.append(BSON("_id" << 0 << "host" << "host1"));
            members.append(BSON("_id" << 1 << "host" << "host2"));
            _config = new ReplSetConfig(BSON("_id" << "foo" << "members" << members.arr()));

            _myConfig = new ReplSetConfig::MemberCfg();
        }
        virtual bool isSecondary() {
            return true;
        }
        virtual bool isPrimary() {
            BSONObj obj;
            return _syncTail->peek(&obj) == false;
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
    };

    class TestRSSync : public Base {
        BackgroundSyncTest *_bgsync;
        replset::SyncTail *_tailer;

        void setup() {
            // setup background sync instance
            _bgsync = new BackgroundSyncTest();

            // setup tail
            _tailer = new replset::SyncTail(_bgsync);

            // setup theReplSet
            ReplSetTest *rst = new ReplSetTest();
            rst->setSyncTail(_bgsync);
            theReplSet = rst;
        }

        void addOp(const string& op, BSONObj o, BSONObj* o2 = 0, const char* coll = 0) {
            OpTime ts;
            {
                Lock::GlobalWrite lk;
                ts = OpTime::_now();
            }

            BSONObjBuilder b;
            b.appendTimestamp("ts", ts.asLL());
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
                addOp("i", BSON("_id" << i << "x" << 123));
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

        void addUniqueIndex() {
            addOp("i", BSON("ns" << ns() << "key" << BSON("x" << 1) << "name" << "x1" << "unique" << true), 0, "unittests.system.indexes");
            addInserts(2);
        }

        void applyOplog() {
            _tailer->oplogApplication();
        }
    public:
        ~TestRSSync() {
            delete _bgsync;
            delete _tailer;
        }

        void run() {
            const int expected = 100;

            setup();

            drop();
            addInserts(100);
            applyOplog();

            ASSERT_EQUALS(expected, static_cast<int>(client()->count(ns())));

            drop();
            addUpdates();
            applyOplog();

            BSONObj obj = findOne();

            ASSERT_EQUALS(1334813340, obj["requests"]["1000001_2"]["timestamp"].number());
            ASSERT_EQUALS(1334813368, obj["requests"]["1000002_2"]["timestamp"].number());
            ASSERT_EQUALS(1334810820, obj["requests"]["100002_1"]["timestamp"].number());

            // test dup key error
            drop();
            addUniqueIndex();
            applyOplog();

            ASSERT_EQUALS(1, static_cast<int>(client()->count(ns())));
            BSONObj obj2;
            ASSERT(_bgsync->peek(&obj2));
        }
    };

    class All : public Suite {
    public:
        All() : Suite( "replset" ) {
        }

        void setupTests() {
            add< TestInitApplyOp >();
            add< TestInitApplyOp2 >();
            add< CappedInitialSync >();
            add< CappedUpdate >();
            add< CappedInsert >();
            add< TestRSSync >();
        }
    } myall;
}
