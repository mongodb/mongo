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

#include "../db/repl/rs.h"

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
    };
    DBDirectClient Base::client_;


    class MockInitialSync : public replset::InitialSync {
        int step;
    public:
        MockInitialSync() : replset::InitialSync(""), step(0), failOnStep(SUCCEED), retry(true) {}

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

            OpTime o1,o2;

            {
                mongo::mutex::scoped_lock lk2(OpTime::m);
                o1 = OpTime::now(lk2);
                o2 = OpTime::now(lk2);
            }

            BSONObjBuilder b;
            b.appendTimestamp("ts", o2.asLL());
            BSONObj obj = b.obj();

            MockInitialSync mock;

            // all three should succeed
            mock.applyOp(obj, o1);

            mock.failOnStep = MockInitialSync::FAIL_FIRST_APPLY;
            mock.applyOp(obj, o1);

            mock.retry = false;
            mock.applyOp(obj, o1);

            // force failure
            MockInitialSync mock2;
            mock2.failOnStep = MockInitialSync::FAIL_BOTH_APPLY;

            ASSERT_THROWS(mock2.applyOp(obj, o2), UserException);
        }
    };

    class SyncTest2 : public replset::InitialSync {
    public:
        bool insertOnRetry;
        SyncTest2() : replset::InitialSync(""), insertOnRetry(false) {}
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

            OpTime o1 = OpTime::_now();
            OpTime o2 = OpTime::_now();

            BSONObjBuilder b;
            b.appendTimestamp("ts", o2.asLL());
            b.append("op", "u");
            b.append("o", BSON("$set" << BSON("x" << 456)));
            b.append("o2", BSON("_id" << 123));
            b.append("ns", ns());
            BSONObj obj = b.obj();

            SyncTest2 sync;
            ASSERT_THROWS(sync.applyOp(obj, o1), UserException);

            sync.insertOnRetry = true;
            // succeeds
            sync.applyOp(obj, o1);

            BSONObj fin = findOne();
            verify(fin["x"].Number() == 456);
        }
    };

    class CappedInitialSync : public Base {
        string _ns;
        Lock::DBWrite _lk;

        string spec() const {
            return "{\"capped\":true,\"size\":512}";
        }

        void create() {
            Client::Context c(_ns);
            string err;
            ASSERT(userCreateNS( _ns.c_str(), fromjson( spec() ), err, false ));
        }

        void drop() {
            Client::Context c(_ns);
            if (nsdetails(_ns.c_str()) != NULL) {
                string errmsg;
                BSONObjBuilder result;
                dropCollection( string(_ns), errmsg, result );
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
            b.append("ns", _ns);
            BSONObj o = b.obj();

            verify(!apply(o));
            return o;
        }
    public:
        CappedInitialSync() : _ns("unittests.foo.bar"), _lk(_ns) {
            drop();
            create();
        }
        virtual ~CappedInitialSync() {
            drop();
        }

        string& cappedNs() {
            return _ns;
        }

        // returns true on success, false on failure
        bool apply(const BSONObj& op) {
            Client::Context ctx( _ns );
            // in an annoying twist of api, returns true on failure
            return !applyOperation_inlock(op, true);
        }

        void run() {
            Lock::DBWrite lk(_ns);

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

            // Just to be sure, no _id index, right?
            Client::Context ctx(cappedNs());
            NamespaceDetails *nsd = nsdetails(cappedNs().c_str());
            verify(nsd->findIdIndex() == -1);
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
        }
    } myall;
}
