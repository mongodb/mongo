/*    Copyright 2012 10gen Inc.
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

#include <cstdint>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/client/connpool.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authz_manager_external_state_mock.h"
#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/dbtests/mock/mock_conn_registry.h"
#include "mongo/dbtests/mock/mock_dbclient_connection.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/socket_exception.h"

/**
 * Tests for ShardConnection, particularly in connection pool management.
 * The tests focuses more on ShardConnection's logic as opposed to testing
 * the internal connections together, like in client/scoped_db_conn_test.cpp.
 */

namespace mongo {
namespace {

using std::string;
using std::vector;

const string TARGET_HOST = "$dummy:27017";

/**
 * Warning: cannot run in parallel
 */
class ShardConnFixture : public mongo::unittest::Test {
public:
    void setUp() {
        if (!haveClient()) {
            Client::initThread("ShardConnFixture", getGlobalServiceContext(), NULL);
        }
        _maxPoolSizePerHost = mongo::shardConnectionPool.getMaxPoolSize();

        mongo::ConnectionString::setConnectionHook(
            mongo::MockConnRegistry::get()->getConnStrHook());
        _dummyServer = new MockRemoteDBServer(TARGET_HOST);
        mongo::MockConnRegistry::get()->addServer(_dummyServer);
    }

    void tearDown() {
        ShardConnection::clearPool();

        mongo::MockConnRegistry::get()->removeServer(_dummyServer->getServerAddress());
        delete _dummyServer;

        mongo::shardConnectionPool.setMaxPoolSize(_maxPoolSizePerHost);
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
    void checkNewConns(void (*checkFunc)(uint64_t, uint64_t),
                       uint64_t arg2,
                       size_t newConnsToCreate) {
        vector<ShardConnection*> newConnList;
        for (size_t x = 0; x < newConnsToCreate; x++) {
            ShardConnection* newConn =
                new ShardConnection(ConnectionString(HostAndPort(TARGET_HOST)), "test.user");
            checkFunc(newConn->get()->getSockCreationMicroSec(), arg2);
            newConnList.push_back(newConn);
        }

        const uint64_t oldCreationTime = mongo::curTimeMicros64();

        for (vector<ShardConnection*>::iterator iter = newConnList.begin();
             iter != newConnList.end();
             ++iter) {
            (*iter)->done();
            delete *iter;
        }

        newConnList.clear();

        // Check that connections created after the purge was put back to the pool.
        for (size_t x = 0; x < newConnsToCreate; x++) {
            ShardConnection* newConn =
                new ShardConnection(ConnectionString(HostAndPort(TARGET_HOST)), "test.user");
            ASSERT_LESS_THAN(newConn->get()->getSockCreationMicroSec(), oldCreationTime);
            newConnList.push_back(newConn);
        }

        for (vector<ShardConnection*>::iterator iter = newConnList.begin();
             iter != newConnList.end();
             ++iter) {
            (*iter)->done();
            delete *iter;
        }
    }

private:
    MockRemoteDBServer* _dummyServer;
    uint32_t _maxPoolSizePerHost;
};

TEST_F(ShardConnFixture, BasicShardConnection) {
    ShardConnection conn1(ConnectionString(HostAndPort(TARGET_HOST)), "test.user");
    ShardConnection conn2(ConnectionString(HostAndPort(TARGET_HOST)), "test.user");

    DBClientBase* conn1Ptr = conn1.get();
    conn1.done();

    ShardConnection conn3(ConnectionString(HostAndPort(TARGET_HOST)), "test.user");
    ASSERT_EQUALS(conn1Ptr, conn3.get());

    conn2.done();
    conn3.done();
}

TEST_F(ShardConnFixture, InvalidateBadConnInPool) {
    ShardConnection conn1(ConnectionString(HostAndPort(TARGET_HOST)), "test.user");
    ShardConnection conn2(ConnectionString(HostAndPort(TARGET_HOST)), "test.user");
    ShardConnection conn3(ConnectionString(HostAndPort(TARGET_HOST)), "test.user");

    conn1.done();
    conn3.done();

    const uint64_t badCreationTime = mongo::curTimeMicros64();
    killServer();

    try {
        conn2.get()->query("test.user", mongo::Query());
    } catch (const mongo::SocketException&) {
    }

    conn2.done();

    restartServer();
    checkNewConns(assertGreaterThan, badCreationTime, 10);
}

TEST_F(ShardConnFixture, DontReturnKnownBadConnToPool) {
    ShardConnection conn1(ConnectionString(HostAndPort(TARGET_HOST)), "test.user");
    ShardConnection conn2(ConnectionString(HostAndPort(TARGET_HOST)), "test.user");
    ShardConnection conn3(ConnectionString(HostAndPort(TARGET_HOST)), "test.user");

    conn1.done();
    killServer();

    try {
        conn3.get()->query("test.user", mongo::Query());
    } catch (const mongo::SocketException&) {
    }

    restartServer();

    const uint64_t badCreationTime = conn3.get()->getSockCreationMicroSec();
    conn3.done();
    // attempting to put a 'bad' connection back to the pool
    conn2.done();

    checkNewConns(assertGreaterThan, badCreationTime, 10);
}

TEST_F(ShardConnFixture, BadConnClearsPoolWhenKilled) {
    ShardConnection conn1(ConnectionString(HostAndPort(TARGET_HOST)), "test.user");
    ShardConnection conn2(ConnectionString(HostAndPort(TARGET_HOST)), "test.user");
    ShardConnection conn3(ConnectionString(HostAndPort(TARGET_HOST)), "test.user");

    conn1.done();
    killServer();

    try {
        conn3.get()->query("test.user", mongo::Query());
    } catch (const mongo::SocketException&) {
    }

    restartServer();

    const uint64_t badCreationTime = conn3.get()->getSockCreationMicroSec();
    conn3.kill();
    // attempting to put a 'bad' connection back to the pool
    conn2.done();

    checkNewConns(assertGreaterThan, badCreationTime, 10);
}

TEST_F(ShardConnFixture, KilledGoodConnShouldNotClearPool) {
    ShardConnection conn1(ConnectionString(HostAndPort(TARGET_HOST)), "test.user");
    ShardConnection conn2(ConnectionString(HostAndPort(TARGET_HOST)), "test.user");
    ShardConnection conn3(ConnectionString(HostAndPort(TARGET_HOST)), "test.user");

    const uint64_t upperBoundCreationTime = conn3.get()->getSockCreationMicroSec();
    conn3.done();

    const uint64_t badCreationTime = conn1.get()->getSockCreationMicroSec();
    conn1.kill();

    conn2.done();

    ShardConnection conn4(ConnectionString(HostAndPort(TARGET_HOST)), "test.user");
    ShardConnection conn5(ConnectionString(HostAndPort(TARGET_HOST)), "test.user");

    ASSERT_GREATER_THAN(conn4.get()->getSockCreationMicroSec(), badCreationTime);
    ASSERT_LESS_THAN_OR_EQUALS(conn4.get()->getSockCreationMicroSec(), upperBoundCreationTime);

    ASSERT_GREATER_THAN(conn5.get()->getSockCreationMicroSec(), badCreationTime);
    ASSERT_LESS_THAN_OR_EQUALS(conn5.get()->getSockCreationMicroSec(), upperBoundCreationTime);

    checkNewConns(assertGreaterThan, upperBoundCreationTime, 10);
}

TEST_F(ShardConnFixture, InvalidateBadConnEvenWhenPoolIsFull) {
    mongo::shardConnectionPool.setMaxPoolSize(2);

    ShardConnection conn1(ConnectionString(HostAndPort(TARGET_HOST)), "test.user");
    ShardConnection conn2(ConnectionString(HostAndPort(TARGET_HOST)), "test.user");
    ShardConnection conn3(ConnectionString(HostAndPort(TARGET_HOST)), "test.user");

    conn1.done();
    conn3.done();

    const uint64_t badCreationTime = mongo::curTimeMicros64();
    killServer();

    try {
        conn2.get()->query("test.user", mongo::Query());
    } catch (const mongo::SocketException&) {
    }

    conn2.done();

    restartServer();
    checkNewConns(assertGreaterThan, badCreationTime, 2);
}

TEST_F(ShardConnFixture, DontReturnConnGoneBadToPool) {
    ShardConnection conn1(ConnectionString(HostAndPort(TARGET_HOST)), "test.user");
    const uint64_t conn1CreationTime = conn1.get()->getSockCreationMicroSec();

    uint64_t conn2CreationTime = 0;

    {
        ShardConnection conn2(ConnectionString(HostAndPort(TARGET_HOST)), "test.user");
        conn2CreationTime = conn2.get()->getSockCreationMicroSec();

        conn1.done();
        // conn2 gets out of scope without calling done()
    }

    // conn2 should not have been put back into the pool but it should
    // also not invalidate older connections since it didn't encounter
    // a socket exception.

    ShardConnection conn1Again(ConnectionString(HostAndPort(TARGET_HOST)), "test.user");
    ASSERT_EQUALS(conn1CreationTime, conn1Again.get()->getSockCreationMicroSec());

    checkNewConns(assertNotEqual, conn2CreationTime, 10);
    conn1Again.done();
}

}  // namespace
}  // namespace mongo
