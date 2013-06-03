/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/base/init.h"
#include "mongo/client/connpool.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authz_manager_external_state_mock.h"
#include "mongo/dbtests/mock/mock_conn_registry.h"
#include "mongo/dbtests/mock/mock_dbclient_connection.h"
#include "mongo/platform/cstdint.h"
#include "mongo/s/shard.h"
#include "mongo/unittest/unittest.h"

#include <vector>
#include <boost/scoped_ptr.hpp>
#include <boost/thread/thread.hpp>

/**
 * Tests for ShardConnection, particularly in connection pool management.
 * The tests focuses more on ShardConnection's logic as opposed to testing
 * the internal connections together, like in client/scoped_db_conn_test.cpp.
 */

using boost::scoped_ptr;
using mongo::DBClientBase;
using mongo::MockRemoteDBServer;
using mongo::ShardConnection;
using std::string;
using std::vector;

namespace mongo {
    // Note: these are all crutch and hopefully will eventually go away
    CmdLine cmdLine;

    bool inShutdown() {
        return false;
    }

    DBClientBase *createDirectClient() { return NULL; }

    void dbexit(ExitCode rc, const char *why){
        ::_exit(-1);
    }

    bool haveLocalShardingInfo(const string& ns) {
        return false;
    }
}

namespace mongo_test {
    const string TARGET_HOST = "$dummy:27017";

    /**
     * Warning: cannot run in parallel
     */
    class ShardConnFixture: public mongo::unittest::Test {
    public:
        void setUp() {
            _maxPoolSizePerHost = mongo::PoolForHost::getMaxPerHost();

            mongo::ConnectionString::setConnectionHook(
                    mongo::MockConnRegistry::get()->getConnStrHook());
            _dummyServer = new MockRemoteDBServer(TARGET_HOST);
            mongo::MockConnRegistry::get()->addServer(_dummyServer);

            mongo::setGlobalAuthorizationManager(new mongo::AuthorizationManager(
                    new mongo::AuthzManagerExternalStateMock()));
        }

        void tearDown() {
            ShardConnection::clearPool();

            mongo::MockConnRegistry::get()->removeServer(_dummyServer->getServerAddress());
            delete _dummyServer;

            mongo::PoolForHost::setMaxPerHost(_maxPoolSizePerHost);

            mongo::clearGlobalAuthorizationManager();
        }

        void killServer() {
            _dummyServer->shutdown();
        }

        void restartServer() {
            _dummyServer->reboot();
        }

    protected:
        static void assertGreaterThan(uint64_t a, uint64_t b) {
            ASSERT_GREATER_THAN(a, b);
        }

        static void assertNotEqual(uint64_t a, uint64_t b) {
            ASSERT_NOT_EQUALS(a, b);
        }

        /**
         * Tries to grab a series of connections from the pool, perform checks on
         * them, then put them back into the pool. After that, it checks these
         * connections can be retrieved again from the pool.
         *
         * @param checkFunc method for comparing new connections and arg2.
         * @param arg2 the value to pass as the 2nd parameter of checkFunc.
         * @param newConnsToCreate the number of new connections to make.
         */
        void checkNewConns(void (*checkFunc)(uint64_t, uint64_t), uint64_t arg2,
                size_t newConnsToCreate) {
            vector<ShardConnection*> newConnList;
            for (size_t x = 0; x < newConnsToCreate; x++) {
                ShardConnection* newConn = new ShardConnection(TARGET_HOST, "test.user");
                checkFunc(newConn->get()->getSockCreationMicroSec(), arg2);
                newConnList.push_back(newConn);
            }

            const uint64_t oldCreationTime = mongo::curTimeMicros64();

            for (vector<ShardConnection*>::iterator iter = newConnList.begin();
                    iter != newConnList.end(); ++iter) {
                (*iter)->done();
                delete *iter;
            }

            newConnList.clear();

            // Check that connections created after the purge was put back to the pool.
            for (size_t x = 0; x < newConnsToCreate; x++) {
                ShardConnection* newConn = new ShardConnection(TARGET_HOST, "test.user");
                ASSERT_LESS_THAN(newConn->get()->getSockCreationMicroSec(), oldCreationTime);
                newConnList.push_back(newConn);
            }

            for (vector<ShardConnection*>::iterator iter = newConnList.begin();
                    iter != newConnList.end(); ++iter) {
                (*iter)->done();
                delete *iter;
            }
        }

    private:
        MockRemoteDBServer* _dummyServer;
        uint32_t _maxPoolSizePerHost;
    };

    TEST_F(ShardConnFixture, BasicShardConnection) {
        ShardConnection conn1(TARGET_HOST, "test.user");
        ShardConnection conn2(TARGET_HOST, "test.user");

        DBClientBase* conn1Ptr = conn1.get();
        conn1.done();

        ShardConnection conn3(TARGET_HOST, "test.user");
        ASSERT_EQUALS(conn1Ptr, conn3.get());

        conn2.done();
        conn3.done();
    }

    TEST_F(ShardConnFixture, InvalidateBadConnInPool) {
        ShardConnection conn1(TARGET_HOST, "test.user");
        ShardConnection conn2(TARGET_HOST, "test.user");
        ShardConnection conn3(TARGET_HOST, "test.user");

        conn1.done();
        conn3.done();

        const uint64_t badCreationTime = mongo::curTimeMicros64();
        killServer();

        try {
            conn2.get()->query("test.user", mongo::Query());
        }
        catch (const mongo::SocketException&) {
        }

        conn2.done();

        restartServer();
        checkNewConns(assertGreaterThan, badCreationTime, 10);
    }

    TEST_F(ShardConnFixture, DontReturnKnownBadConnToPool) {
        ShardConnection conn1(TARGET_HOST, "test.user");
        ShardConnection conn2(TARGET_HOST, "test.user");
        ShardConnection conn3(TARGET_HOST, "test.user");

        conn1.done();
        killServer();

        try {
            conn3.get()->query("test.user", mongo::Query());
        }
        catch (const mongo::SocketException&) {
        }

        restartServer();

        const uint64_t badCreationTime = conn3.get()->getSockCreationMicroSec();
        conn3.done();
        // attempting to put a 'bad' connection back to the pool
        conn2.done();

        checkNewConns(assertGreaterThan, badCreationTime, 10);
    }

    TEST_F(ShardConnFixture, BadConnClearsPoolWhenKilled) {
        ShardConnection conn1(TARGET_HOST, "test.user");
        ShardConnection conn2(TARGET_HOST, "test.user");
        ShardConnection conn3(TARGET_HOST, "test.user");

        conn1.done();
        killServer();

        try {
            conn3.get()->query("test.user", mongo::Query());
        }
        catch (const mongo::SocketException&) {
        }

        restartServer();

        const uint64_t badCreationTime = conn3.get()->getSockCreationMicroSec();
        conn3.kill();
        // attempting to put a 'bad' connection back to the pool
        conn2.done();

        checkNewConns(assertGreaterThan, badCreationTime, 10);
    }

    TEST_F(ShardConnFixture, KilledGoodConnShouldNotClearPool) {
        ShardConnection conn1(TARGET_HOST, "test.user");
        ShardConnection conn2(TARGET_HOST, "test.user");
        ShardConnection conn3(TARGET_HOST, "test.user");

        const uint64_t upperBoundCreationTime =
                conn3.get()->getSockCreationMicroSec();
        conn3.done();

        const uint64_t badCreationTime = conn1.get()->getSockCreationMicroSec();
        conn1.kill();

        conn2.done();

        ShardConnection conn4(TARGET_HOST, "test.user");
        ShardConnection conn5(TARGET_HOST, "test.user");

        ASSERT_GREATER_THAN(conn4.get()->getSockCreationMicroSec(), badCreationTime);
        ASSERT_LESS_THAN_OR_EQUALS(conn4.get()->getSockCreationMicroSec(),
                upperBoundCreationTime);

        ASSERT_GREATER_THAN(conn5.get()->getSockCreationMicroSec(), badCreationTime);
        ASSERT_LESS_THAN_OR_EQUALS(conn5.get()->getSockCreationMicroSec(),
                upperBoundCreationTime);

        checkNewConns(assertGreaterThan, upperBoundCreationTime, 10);
    }

    TEST_F(ShardConnFixture, InvalidateBadConnEvenWhenPoolIsFull) {
        mongo::PoolForHost::setMaxPerHost(2);

        ShardConnection conn1(TARGET_HOST, "test.user");
        ShardConnection conn2(TARGET_HOST, "test.user");
        ShardConnection conn3(TARGET_HOST, "test.user");

        conn1.done();
        conn3.done();

        const uint64_t badCreationTime = mongo::curTimeMicros64();
        killServer();

        try {
            conn2.get()->query("test.user", mongo::Query());
        }
        catch (const mongo::SocketException&) {
        }

        conn2.done();

        restartServer();
        checkNewConns(assertGreaterThan, badCreationTime, 2);
    }

    TEST_F(ShardConnFixture, DontReturnConnGoneBadToPool) {
        ShardConnection conn1(TARGET_HOST, "test.user");
        const uint64_t conn1CreationTime = conn1.get()->getSockCreationMicroSec();

        uint64_t conn2CreationTime = 0;

        {
            ShardConnection conn2(TARGET_HOST, "test.user");
            conn2CreationTime = conn2.get()->getSockCreationMicroSec();

            conn1.done();
            // conn2 gets out of scope without calling done()
        }

        // conn2 should not have been put back into the pool but it should
        // also not invalidate older connections since it didn't encounter
        // a socket exception.

        ShardConnection conn1Again(TARGET_HOST, "test.user");
        ASSERT_EQUALS(conn1CreationTime, conn1Again.get()->getSockCreationMicroSec());

        checkNewConns(assertNotEqual, conn2CreationTime, 10);
        conn1Again.done();
    }
}
