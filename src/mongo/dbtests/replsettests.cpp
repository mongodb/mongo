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
#include "../db/queryoptimizer.h"

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
            dblock lk;
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
            writelock lk("");

            OpTime o1 = OpTime::now();
            OpTime o2 = OpTime::now();

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
            writelock lk("");

            OpTime o1 = OpTime::now();
            OpTime o2 = OpTime::now();

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
            assert(fin["x"].Number() == 456);
        }
    };

    class CappedInitialSync : public Base {
        string _ns;
        dblock lk;
        Client::Context _context;

        string spec() const {
            return "{\"capped\":true,\"size\":512}";
        }

        void create() {
            dblock lk;
            string err;
            ASSERT(userCreateNS( _ns.c_str(), fromjson( spec() ), err, false ));
        }

        void drop() {
            string s( _ns );
            string errmsg;
            BSONObjBuilder result;
            dropCollection( s, errmsg, result );
        }
    public:
        CappedInitialSync() : _ns("unittests.foo.bar"), _context(_ns) {
            if (nsdetails(_ns.c_str()) != NULL) {
                drop();
            }
        }
        ~CappedInitialSync() {
            if ( nsdetails(_ns.c_str()) == NULL )
                return;
            drop();
        }

        void run() {
            create();

            BSONObjBuilder b;
            b.appendTimestamp("ts", OpTime::now().asLL());
            b.append("op", "u");
            b.append("o", BSON("$set" << BSON("x" << 456)));
            b.append("o2", BSON("_id" << 123 << "x" << 123));
            b.append("ns", _ns);
            BSONObj op = b.obj();

            // in an annoying twist of api, returns true on failure
            assert(applyOperation_inlock(op, true));

            Sync s("");
            assert(!s.shouldRetry(op));
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
        }
    } myall;
}
