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

#include "mongo/pch.h"

#include "mongo/db/db.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/instance.h"
#include "mongo/db/json.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/oplog.h"
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
            Client::Context ctx( ns() );
            theDataFileMgr.insert( ns(), o.objdata(), o.objsize(), false, god );
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
        static void setup() {
            cmdLine._replSet = "foo";
            cmdLine.oplogSize = 5 * 1024 * 1024;
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

    class IndexBuildThread : public BackgroundJob {
    public:
        IndexBuildThread(const BSONObj index) :
            _done(false), _index(index.getOwned()), _client(NULL), _curop(NULL) {
        }

        std::string name() const {
            return "index build helper";
        }

        void run() {
            std::string ns = nsToDatabase(_index.getStringField("ns"))+".system.indexes";

            Client::initThread("in progress idx build");
            Client::WriteContext ctx(ns);
            {
                boost::mutex::scoped_lock lk(_mtx);
                _client = currentClient.get();
            }

            // This spins to mimic the db building an index.  Yield the read lock so that other
            // dbs can be opened (which requires the write lock)
            while (true) {
                dbtemprelease temp;
                sleepmillis(10);
                boost::mutex::scoped_lock lk(_mtx);
                if (_curop) {
                    if (_curop->killPendingStrict()) {
                        break;
                    }
                }
            }
            mongo::unittest::log() << "index build ending" << endl;
            killCurrentOp.notifyAllWaiters();
            cc().shutdown();

            boost::mutex::scoped_lock lk(_mtx);
            _done = true;
        }

        // Make sure the test doesn't end while this "index build" is still spinning
        void finish() {
            while (true) {
                sleepmillis(10);
                boost::mutex::scoped_lock lk(_mtx);
                if (_curop) break;
            }

            _curop->kill();

            while (!finished()) {
                sleepmillis(10);
            }
        }

        bool finished() {
            boost::mutex::scoped_lock lk(_mtx);
            return _done;
        }

        CurOp* curop() {
            {
                boost::mutex::scoped_lock lk(_mtx);
                if (_curop != NULL) {
                    return _curop;
                }
            }

            while (true) {
                sleepmillis(10);
                boost::mutex::scoped_lock lk(_mtx);
                if (_client) break;
            }

            boost::mutex::scoped_lock lk(_mtx);
            // On the first time through, make sure the curop is set up correctly
            _client->curop()->reset(HostAndPort::me(), dbInsert);
            _client->curop()->enter(_client->getContext());
            _client->curop()->setQuery(_index);


            _curop = _client->curop();
            return _curop;
        }

    private:
        bool _done;
        BSONObj _index;
        Client* _client;
        CurOp* _curop;
        // Mutex protects all other members of this class, except _index which is
        // set in the constructor and never subsequently changed
        boost::mutex _mtx;
    };

    class TestDropDB : public Base {
    public:
        void run() {
            drop();

            std::string dbname = nsToDatabase(ns());
            Command *dropDB = Command::findCommand("dropDatabase");
            BSONObj cmdObj = BSON("dropDatabase" << 1);

            BSONObj indexOp1 = BSON("ns" << ns() << "name" << "foo_1" <<  "key" <<
                                    BSON("foo" << 1));
            BSONObj indexOp2 = BSON("ns" << (dbname+".something.else") << "name" << "baz_1" <<
                                    "key" << BSON("baz" << 1));
            // Different database - foo
            BSONObj indexOp3 = BSON("ns" << "foo.something.else" << "name" << "baz_1" <<
                                    "key" << BSON("baz" << 1));
            // Different database - unittestsx
            BSONObj indexOp4 = BSON("ns" << (dbname+"x.something.else") << "name" << "baz_1" <<
                                    "key" << BSON("baz" << 1));
            // Different database - xunittests
            BSONObj indexOp5 = BSON("ns" << ("x"+dbname+".something.else") << "name" << "baz_1" <<
                                    "key" << BSON("baz" << 1));

            IndexBuildThread t1(indexOp1);
            IndexBuildThread t2(indexOp2);
            IndexBuildThread t3(indexOp3);
            IndexBuildThread t4(indexOp4);
            IndexBuildThread t5(indexOp5);

            t1.go();
            t2.go();
            t3.go();
            t4.go();
            t5.go();

            ASSERT(!t1.curop()->killPending());
            ASSERT(!t2.curop()->killPending());
            ASSERT(!t3.curop()->killPending());
            ASSERT(!t4.curop()->killPending());
            ASSERT(!t5.curop()->killPending());

            {
                Lock::DBWrite lk(ns());
                dropDB->stopIndexBuilds(dbname, cmdObj);
            }
            sleepsecs(1);

            ASSERT(t1.finished());
            ASSERT(t2.finished());
            ASSERT(!t3.finished());
            ASSERT(!t4.finished());
            ASSERT(!t5.finished());

            t3.finish();
            t4.finish();
            t5.finish();
        }
    };

    class TestDrop : public Base {
    public:
        void run() {
            drop();

            std::string dbname = nsToDatabase(ns());
            Command *drop = Command::findCommand("drop");

            BSONObj indexOp1 = BSON("ns" << ns() << "name" << "foo_1" <<
                                    "key" << BSON("foo" << 1));
            BSONObj indexOp2 = BSON("ns" << ns() << "name" << "bar_1" <<
                                    "key" << BSON("bar" << 1) << "background" << true);
            // Different collection
            BSONObj indexOp3 = BSON("ns" << (dbname+".something.else") << "name" << "baz_1" <<
                                    "key" << BSON("baz" << 1));

            IndexBuildThread t1(indexOp1);
            IndexBuildThread t2(indexOp2);
            IndexBuildThread t3(indexOp3);

            t1.go();
            t2.go();
            t3.go();

            ASSERT(!t1.curop()->killPending());
            ASSERT(!t2.curop()->killPending());
            ASSERT(!t3.curop()->killPending());

            std::string coll(strchr(ns(), '.')+1);
            BSONObj cmdObj = BSON("drop" << coll);
            {
                Lock::DBWrite lk(ns());
                drop->stopIndexBuilds(dbname, cmdObj);
            }
            sleepsecs(1);

            ASSERT(t1.finished());
            ASSERT(t2.finished());
            ASSERT(!t3.finished());

            t3.finish();
        }
    };

    class TestDropIndexes : public Base {

        void testDropIndexes1() {
            drop();

            std::string dbname = nsToDatabase(ns());
            Command *c = Command::findCommand("dropIndexes");

            BSONObj indexOp1 = BSON("ns" << ns() << "name" << "x_1" <<  "key" << BSON("x" << 1));
            BSONObj indexOp2 = BSON("ns" << ns() << "name" << "y_1" <<  "key" << BSON("y" << 1));
            BSONObj indexOp3 = BSON("ns" << ns() << "name" << "z_1" <<  "key" << BSON("z" << 1));

            std::string coll(strchr(ns(), '.')+1);
            BSONObj cmd1 = BSON("dropIndexes" << coll << "index" << "*");

            IndexBuildThread t1(indexOp1);
            IndexBuildThread t2(indexOp2);
            IndexBuildThread t3(indexOp3);

            t1.go();
            t2.go();
            t3.go();

            ASSERT(!t1.curop()->killPending());
            ASSERT(!t2.curop()->killPending());
            ASSERT(!t3.curop()->killPending());
            {
                Lock::DBWrite lk(ns());
                c->stopIndexBuilds(dbname, cmd1);
            }
            sleepsecs(1);

            ASSERT(t1.finished());
            ASSERT(t2.finished());
            ASSERT(t3.finished());
        }

        void testDropIndexes2() {
            drop();

            std::string dbname = nsToDatabase(ns());
            Command *c = Command::findCommand("dropIndexes");

            BSONObj indexOp1 = BSON("ns" << ns() << "name" << "x_1" <<  "key" << BSON("x" << 1));
            BSONObj indexOp2 = BSON("ns" << ns() << "name" << "y_1" <<  "key" << BSON("y" << 1));
            BSONObj indexOp3 = BSON("ns" << ns() << "name" << "z_1" <<  "key" << BSON("z" << 1));

            std::string coll(strchr(ns(), '.')+1);
            BSONObj cmd2 = BSON("dropIndexes" << coll << "index" << "y_1");

            IndexBuildThread t1(indexOp1);
            IndexBuildThread t2(indexOp2);
            IndexBuildThread t3(indexOp3);

            t1.go();
            t2.go();
            t3.go();

            ASSERT(!t1.curop()->killPending());
            ASSERT(!t2.curop()->killPending());
            ASSERT(!t3.curop()->killPending());
            {
                Lock::DBWrite lk(ns());
                c->stopIndexBuilds(dbname, cmd2);
            }
            sleepsecs(1);

            ASSERT(!t1.finished());
            ASSERT(t2.finished());
            ASSERT(!t3.finished());

            t1.finish();
            t3.finish();
        }

        void testDropIndexes3() {
            drop();

            std::string dbname = nsToDatabase(ns());
            std::string coll(strchr(ns(), '.')+1);
            BSONObj cmd3 = BSON("dropIndexes" << coll << "index" << BSON("z" << 1));
            Command *c = Command::findCommand("dropIndexes");

            BSONObj indexOp1 = BSON("ns" << ns() << "name" << "x_1" <<  "key" << BSON("x" << 1));
            BSONObj indexOp2 = BSON("ns" << ns() << "name" << "y_1" <<  "key" << BSON("y" << 1));
            BSONObj indexOp3 = BSON("ns" << ns() << "name" << "z_1" <<  "key" << BSON("z" << 1));

            IndexBuildThread t1(indexOp1);
            IndexBuildThread t2(indexOp2);
            IndexBuildThread t3(indexOp3);

            t1.go();
            t2.go();
            t3.go();

            ASSERT(!t1.curop()->killPending());
            ASSERT(!t2.curop()->killPending());
            ASSERT(!t3.curop()->killPending());
            {
                Lock::DBWrite lk(ns());
                c->stopIndexBuilds(dbname, cmd3);
            }
            sleepsecs(1);

            ASSERT(!t1.finished());
            ASSERT(!t2.finished());
            ASSERT(t3.finished());

            t1.finish();
            t2.finish();
        }

    public:
        void run() {
            testDropIndexes1();
            testDropIndexes2();
            testDropIndexes3();
        }
    };

    class TestRename : public Base {
    public:
        void run() {
            drop();

            std::string dbname = nsToDatabase(ns());
            std::string coll(strchr(ns(), '.')+1);
            BSONObj cmdObj = BSON("renameCollection" << ns() << "to" << dbname+".bar");
            Command *c = Command::findCommand("renameCollection");

            BSONObj indexOp1 = BSON("ns" << ns() << "name" << "x_1" <<  "key" << BSON("x" << 1));
            BSONObj indexOp2 = BSON("ns" << ns() << "name" << "y_1" <<  "key" << BSON("y" << 1));
            BSONObj indexOp3 = BSON("ns" << (dbname+".bar") << "name" << "z_1" <<  "key" << 
                                    BSON("z" << 1));
            BSONObj indexOp4 = BSON("ns" << ("x."+coll) << "name" << "z_1" <<  "key" << 
                                    BSON("z" << 1));

            IndexBuildThread t1(indexOp1);
            IndexBuildThread t2(indexOp2);
            IndexBuildThread t3(indexOp3);
            IndexBuildThread t4(indexOp4);

            t1.go();
            t2.go();
            t3.go();
            t4.go();

            ASSERT(!t1.curop()->killPending());
            ASSERT(!t2.curop()->killPending());
            ASSERT(!t3.curop()->killPending());
            ASSERT(!t4.curop()->killPending());

            std::vector<BSONObj> indexes;
            {
                Lock::DBWrite lk(ns());
                indexes = c->stopIndexBuilds(dbname, cmdObj);
            }

            ASSERT(t1.finished());
            ASSERT(t2.finished());
            ASSERT(!t3.finished());
            ASSERT(!t4.finished());

            t3.finish();
            t4.finish();

            // Build indexes
            IndexBuilder::restoreIndexes(dbname+".system.indexes", indexes);

            DBDirectClient cli;
            time_t max = time(0)+10;
            // assert.soon
            while (time(0) < max) {
                std::string ns = dbname+".system.indexes";
                if (cli.count(ns) == 3) {
                    return;
                }
            };

            ASSERT(false);
        }
    };

    class TestReIndex : public Base {
    public:
        void run() {
            drop();

            std::string dbname = nsToDatabase(ns());
            std::string coll(strchr(ns(), '.')+1);
            BSONObj cmdObj = BSON("reIndex" << coll);
            Command *c = Command::findCommand("reIndex");

            DBDirectClient cli;
            int originalIndexCount = cli.count(dbname+".system.indexes");

            BSONObj indexOp1 = BSON("ns" << ns() << "name" << "foo_1" <<
                                    "key" << BSON("foo" << 1));
            BSONObj indexOp2 = BSON("ns" << (dbname+".something.else") << "name" << "baz_1" <<
                                    "key" << BSON("baz" << 1));

            IndexBuildThread t1(indexOp1);
            IndexBuildThread t2(indexOp2);

            t1.go();
            t2.go();

            ASSERT(!t1.curop()->killPending());
            ASSERT(!t2.curop()->killPending());

            {
                Lock::DBWrite lk(ns());
                std::vector<BSONObj> indexes = c->stopIndexBuilds(dbname, cmdObj);
                ASSERT_EQUALS(1U, indexes.size());
                IndexBuilder::restoreIndexes(dbname+".system.indexes", indexes);
            }
            ASSERT(!t2.finished());
            t2.finish();

            time_t max = time(0)+10;
            // assert.soon(t1.finished())
            while (time(0) < max) {
                std::string ns = dbname+".system.indexes";
                if (static_cast<int>(cli.count(ns)) == originalIndexCount+2) {
                    return;
                }
            };

            ASSERT(false);
        }
    };

    class TestTruncateCapped : public Base {
    public:
        void run() {
            drop();

            std::string dbname = nsToDatabase(ns());
            std::string coll(strchr(ns(), '.')+1);
            BSONObj cmdObj = BSON("emptycapped" << coll);
            Command *c = Command::findCommand("emptycapped");

            BSONObj indexOp1 = BSON("ns" << ns() << "name" << "foo_1" <<
                                    "key" << BSON("foo" << 1));
            BSONObj indexOp2 = BSON("ns" << (dbname+".something.else") << "name" << "baz_1" <<
                                    "key" << BSON("baz" << 1));

            IndexBuildThread t1(indexOp1);
            IndexBuildThread t2(indexOp2);

            t1.go();
            t2.go();

            ASSERT(!t1.curop()->killPending());
            ASSERT(!t2.curop()->killPending());
            {
                Lock::DBWrite lk(ns());
                c->stopIndexBuilds(dbname, cmdObj);
            }
            sleepsecs(1);

            ASSERT(t1.finished());
            ASSERT(!t2.finished());

            t2.finish();
        }
    };

    class TestCompact : public Base {
    public:
        void run() {
            drop();

            std::string dbname = nsToDatabase(ns());
            std::string coll(strchr(ns(), '.')+1);
            BSONObj cmdObj = BSON("compact" << coll);
            Command *c = Command::findCommand("compact");
            DBDirectClient cli;
            int originalIndexCount = cli.count(dbname+".system.indexes");

            BSONObj indexOp1 = BSON("ns" << ns() << "name" << "foo_1" <<
                                    "key" << BSON("foo" << 1));
            BSONObj indexOp2 = BSON("ns" << (dbname+".something.else") << "name" << "baz_1" <<
                                    "key" << BSON("baz" << 1));

            IndexBuildThread t1(indexOp1);
            IndexBuildThread t2(indexOp2);

            t1.go();
            t2.go();

            ASSERT(!t1.curop()->killPending());
            ASSERT(!t2.curop()->killPending());
            {
                Lock::DBWrite lk(ns());
                std::vector<BSONObj> indexes = c->stopIndexBuilds(dbname, cmdObj);
                IndexBuilder::restoreIndexes(dbname+".system.indexes", indexes);
            }
            ASSERT(!t2.finished());
            t2.finish();

            time_t max = time(0)+10;
            // assert.soon(t1.finished())
            while (time(0) < max) {
                std::string ns = dbname+".system.indexes";
                if (static_cast<int>(cli.count(ns)) == originalIndexCount+2) {
                    return;
                }
            };

            ASSERT(false);
        }
    };

    class TestRepair : public Base {
    public:
        void run() {
            drop();

            std::string dbname = nsToDatabase(ns());
            Command *c = Command::findCommand("repairDatabase");
            BSONObj cmdObj = BSON("repairDatabase" << 1);
            DBDirectClient cli;
            int originalIndexCount = cli.count(dbname+".system.indexes");

            BSONObj indexOp1 = BSON("ns" << ns() << "name" << "foo_1" <<  "key" <<
                                    BSON("foo" << 1));
            BSONObj indexOp2 = BSON("ns" << (dbname+".something.else") << "name" << "baz_1" <<
                                    "key" << BSON("baz" << 1));
            // Different database - foo
            BSONObj indexOp3 = BSON("ns" << "foo.something.else" << "name" << "baz_1" <<
                                    "key" << BSON("baz" << 1));
            // Different database - unittestsx
            BSONObj indexOp4 = BSON("ns" << (dbname+"x.something.else") << "name" << "baz_1" <<
                                    "key" << BSON("baz" << 1));
            // Different database - xunittests
            BSONObj indexOp5 = BSON("ns" << ("x"+dbname+".something.else") << "name" << "baz_1" <<
                                    "key" << BSON("baz" << 1));

            IndexBuildThread t1(indexOp1);
            IndexBuildThread t2(indexOp2);
            IndexBuildThread t3(indexOp3);
            IndexBuildThread t4(indexOp4);
            IndexBuildThread t5(indexOp5);

            t1.go();
            t2.go();
            t3.go();
            t4.go();
            t5.go();

            ASSERT(!t1.curop()->killPending());
            ASSERT(!t2.curop()->killPending());
            ASSERT(!t3.curop()->killPending());
            ASSERT(!t4.curop()->killPending());
            ASSERT(!t5.curop()->killPending());

            std::vector<BSONObj> indexes;
            {
                Lock::DBWrite lk(ns());
                indexes = c->stopIndexBuilds(dbname, cmdObj);
            }
            sleepsecs(1);

            ASSERT(t1.finished());
            ASSERT(t2.finished());
            ASSERT(!t3.finished());
            ASSERT(!t4.finished());
            ASSERT(!t5.finished());

            IndexBuilder::restoreIndexes(dbname+".system.indexes", indexes);

            ASSERT(!t3.finished());
            ASSERT(!t4.finished());
            ASSERT(!t5.finished());

            t3.finish();
            t4.finish();
            t5.finish();

            time_t max = time(0)+10;
            // assert.soon(t1.finished() && t2.finished())
            while (time(0) < max) {
                std::string ns = dbname+".system.indexes";
                if (static_cast<int>(cli.count(ns)) == originalIndexCount+4) {
                    return;
                }
            };

            ASSERT(false);
        }
    };

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
            if (nsdetails(_cappedNs) != NULL) {
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
            BSONObj o = BSON(GENOID << "x" << 456);
            DiskLoc loc = theDataFileMgr.insert( cappedNs().c_str(),
                                                 o.objdata(),
                                                 o.objsize(),
                                                 false,
                                                 false );
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
            NamespaceDetails *nsd = nsdetails(cappedNs());
            verify(nsd->findIdIndex() > -1);
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
            NamespaceDetails *nsd = nsdetails(cappedNs());
            verify(nsd->findIdIndex() >= 0);
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
            add< TestDropDB >();
            add< TestDrop >();
            add< TestDropIndexes >();
            add< TestTruncateCapped >();
            add< TestReIndex >();
            add< TestRepair >();
            add< TestCompact >();
        }
    } myall;
}
