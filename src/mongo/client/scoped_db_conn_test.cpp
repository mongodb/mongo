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

#include "mongo/base/init.h"
#include "mongo/client/connpool.h"
#include "mongo/platform/cstdint.h"
#include "mongo/util/net/listen.h"
#include "mongo/util/net/message_port.h"
#include "mongo/util/net/message_server.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"
#include "mongo/unittest/unittest.h"

#include <vector>
#include <boost/scoped_ptr.hpp>
#include <boost/thread/thread.hpp>

/**
 * Tests for ScopedDbConnection, particularly in connection pool management.
 * The tests also indirectly tests DBClientConnection's failure detection
 * logic (hence the use of the dummy server as opposed to mocking the
 * connection).
 */

using boost::scoped_ptr;
using mongo::DBClientBase;
using mongo::FailPoint;
using mongo::ScopedDbConnection;
using std::string;
using std::vector;

namespace {
    const string TARGET_HOST = "localhost:27017";
    const int TARGET_PORT = 27017;

    mongo::mutex shutDownMutex("shutDownMutex");
    bool shuttingDown = false;
}

namespace mongo {
    // Symbols defined to build the binary correctly.

    bool inShutdown() {
        scoped_lock sl(shutDownMutex);
        return shuttingDown;
    }

    DBClientBase *createDirectClient() { return NULL; }

    void dbexit(ExitCode rc, const char *why){
        {
            scoped_lock sl(shutDownMutex);
            shuttingDown = true;
        }

        ::_exit(rc);
    }

    bool haveLocalShardingInfo(const string& ns) {
        return false;
    }

    class DummyMessageHandler: public MessageHandler {
    public:
        virtual void connected(AbstractMessagingPort* p) {
        }

        virtual void process(Message& m,
                AbstractMessagingPort* port,
                LastError * le) {
            boost::this_thread::interruption_point();
        }

        virtual void disconnected(AbstractMessagingPort* p) {
        }
    };
}

namespace mongo_test {
    mongo::DummyMessageHandler dummyHandler;

    // TODO: Take this out and make it as a reusable class in a header file. The only
    // thing that is preventing this from happening is the dependency on the inShutdown
    // method to terminate the socket listener thread.
    /**
     * Very basic server that accepts connections. Note: can only create one instance
     * at a time. Should not create as a static global instance because of dependency
     * with ListeningSockets::_instance.
     *
     * Warning: Not thread-safe
     *
     * Note: external symbols used:
     * shutDownMutex, shuttingDown
     */
    class DummyServer {
    public:
        /**
         * Creates a new server that listens to the given port.
         *
         * @param port the port number to listen to.
         */
        DummyServer(int port): _port(port), _server(NULL) {
        }

        ~DummyServer() {
            stop();
        }

        /**
         * Starts the server if it is not yet running.
         *
         * @param messageHandler the message handler to use for this server. Ownership
         *     of this object is passed to this server.
         */
        void run(mongo::MessageHandler* messsageHandler) {
            if (_server != NULL) {
                return;
            }

            mongo::MessageServer::Options options;
            options.port = _port;

            {
                mongo::mutex::scoped_lock sl(shutDownMutex);
                shuttingDown = false;
            }

            _server = mongo::createServer(options, messsageHandler);
            _serverThread = boost::thread(runServer, _server);
        }

        /**
         * Stops the server if it is running.
         */
        void stop() {
            if (_server == NULL) {
                return;
            }

            {
                mongo::mutex::scoped_lock sl(shutDownMutex);
                shuttingDown = true;
            }

            mongo::ListeningSockets::get()->closeAll();
            _serverThread.join();

            int connCount = mongo::Listener::globalTicketHolder.used();
            size_t iterCount = 0;
            while (connCount > 0) {
                if ((++iterCount % 20) == 0) {
                    mongo::log() << "DummyServer: Waiting for " << connCount
                                 << " connections to close." << std::endl;
                }

                mongo::sleepmillis(500);
                connCount = mongo::Listener::globalTicketHolder.used();
            }

            delete _server;
            _server = NULL;
        }

        /**
         * Helper method for running the server on a separate thread.
         */
        static void runServer(mongo::MessageServer* server) {
            server->setupSockets();
            server->run();
        }

    private:
        const int _port;
        boost::thread _serverThread;
        mongo::MessageServer* _server;
    };

    /**
     * Warning: cannot run in parallel
     */
    class DummyServerFixture: public mongo::unittest::Test {
    public:
        void setUp() {
            _maxPoolSizePerHost = mongo::pool.getMaxPoolSize();
            _dummyServer = new DummyServer(TARGET_PORT);

            _dummyServer->run(&dummyHandler);
            mongo::DBClientConnection conn;
            mongo::Timer timer;

            // Make sure the dummy server is up and running before proceeding
            while (true) {
                try {
                    conn.connect(TARGET_HOST);
                    break;
                } catch (const mongo::ConnectException&) {
                    if (timer.seconds() > 20) {
                        FAIL("Timed out connecting to dummy server");
                    }
                }
            }
        }

        void tearDown() {
            ScopedDbConnection::clearPool();
            delete _dummyServer;

            mongo::pool.setMaxPoolSize(_maxPoolSizePerHost);
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
            vector<ScopedDbConnection*> newConnList;
            for (size_t x = 0; x < newConnsToCreate; x++) {
                ScopedDbConnection* newConn = new ScopedDbConnection(TARGET_HOST);
                checkFunc(newConn->get()->getSockCreationMicroSec(), arg2);
                newConnList.push_back(newConn);
            }

            const uint64_t oldCreationTime = mongo::curTimeMicros64();

            for (vector<ScopedDbConnection*>::iterator iter = newConnList.begin();
                    iter != newConnList.end(); ++iter) {
                (*iter)->done();
                delete *iter;
            }

            newConnList.clear();

            // Check that connections created after the purge was put back to the pool.
            for (size_t x = 0; x < newConnsToCreate; x++) {
                ScopedDbConnection* newConn = new ScopedDbConnection(TARGET_HOST);
                ASSERT_LESS_THAN(newConn->get()->getSockCreationMicroSec(), oldCreationTime);
                newConnList.push_back(newConn);
            }

            for (vector<ScopedDbConnection*>::iterator iter = newConnList.begin();
                    iter != newConnList.end(); ++iter) {
                (*iter)->done();
                delete *iter;
            }
        }

    private:
        static void runServer(mongo::MessageServer* server) {
            server->setupSockets();
            server->run();
        }

        DummyServer* _dummyServer;
        uint32_t _maxPoolSizePerHost;
    };

    TEST_F(DummyServerFixture, BasicScopedDbConnection) {
        ScopedDbConnection conn1(TARGET_HOST);
        ScopedDbConnection conn2(TARGET_HOST);

        DBClientBase* conn1Ptr = conn1.get();
        conn1.done();

        ScopedDbConnection conn3(TARGET_HOST);
        ASSERT_EQUALS(conn1Ptr, conn3.get());

        conn2.done();
        conn3.done();
    }

    TEST_F(DummyServerFixture, InvalidateBadConnInPool) {
        ScopedDbConnection conn1(TARGET_HOST);
        ScopedDbConnection conn2(TARGET_HOST);
        ScopedDbConnection conn3(TARGET_HOST);

        conn1.done();
        conn3.done();

        const uint64_t badCreationTime = mongo::curTimeMicros64();

        mongo::getGlobalFailPointRegistry()->getFailPoint("throwSockExcep")->
                setMode(FailPoint::alwaysOn);

        try {
            conn2->query("test.user", mongo::Query());
        }
        catch (const mongo::SocketException&) {
        }

        mongo::getGlobalFailPointRegistry()->getFailPoint("throwSockExcep")->
                setMode(FailPoint::off);
        conn2.done();

        checkNewConns(assertGreaterThan, badCreationTime, 10);
    }

    TEST_F(DummyServerFixture, DontReturnKnownBadConnToPool) {
        ScopedDbConnection conn1(TARGET_HOST);
        ScopedDbConnection conn2(TARGET_HOST);
        ScopedDbConnection conn3(TARGET_HOST);

        conn1.done();

        mongo::getGlobalFailPointRegistry()->getFailPoint("throwSockExcep")->
                setMode(FailPoint::alwaysOn);

        try {
            conn3->query("test.user", mongo::Query());
        }
        catch (const mongo::SocketException&) {
        }

        mongo::getGlobalFailPointRegistry()->getFailPoint("throwSockExcep")->
                setMode(FailPoint::off);

        const uint64_t badCreationTime = conn3->getSockCreationMicroSec();
        conn3.done();
        // attempting to put a 'bad' connection back to the pool
        conn2.done();

        checkNewConns(assertGreaterThan, badCreationTime, 10);
    }

    TEST_F(DummyServerFixture, InvalidateBadConnEvenWhenPoolIsFull) {
        mongo::pool.setMaxPoolSize(2);

        ScopedDbConnection conn1(TARGET_HOST);
        ScopedDbConnection conn2(TARGET_HOST);
        ScopedDbConnection conn3(TARGET_HOST);

        conn1.done();
        conn3.done();

        const uint64_t badCreationTime = mongo::curTimeMicros64();

        mongo::getGlobalFailPointRegistry()->getFailPoint("throwSockExcep")->
                setMode(FailPoint::alwaysOn);

        try {
            conn2->query("test.user", mongo::Query());
        }
        catch (const mongo::SocketException&) {
        }

        mongo::getGlobalFailPointRegistry()->getFailPoint("throwSockExcep")->
                setMode(FailPoint::off);
        conn2.done();

        checkNewConns(assertGreaterThan, badCreationTime, 2);
    }

    TEST_F(DummyServerFixture, DontReturnConnGoneBadToPool) {
        ScopedDbConnection conn1(TARGET_HOST);

        const uint64_t conn1CreationTime = conn1->getSockCreationMicroSec();

        uint64_t conn2CreationTime = 0;

        {
            ScopedDbConnection conn2(TARGET_HOST);
            conn2CreationTime = conn2->getSockCreationMicroSec();

            conn1.done();
            // conn2 gets out of scope without calling done()
        }

        // conn2 should not have been put back into the pool but it should
        // also not invalidate older connections since it didn't encounter
        // a socket exception.

        ScopedDbConnection conn1Again(TARGET_HOST);
        ASSERT_EQUALS(conn1CreationTime, conn1Again->getSockCreationMicroSec());

        checkNewConns(assertNotEqual, conn2CreationTime, 10);

        conn1Again.done();
    }
}
