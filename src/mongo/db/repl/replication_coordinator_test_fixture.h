/**
 *    Copyright (C) 2014 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <boost/scoped_ptr.hpp>
#include <string>

#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

    class BSONObj;
    struct HostAndPort;

namespace repl {

    class NetworkInterfaceMock;
    class ReplicaSetConfig;
    class ReplicationCoordinatorExternalStateMock;
    class ReplicationCoordinatorImpl;
    class TopologyCoordinatorImpl;

    /**
     * Fixture for testing ReplicationCoordinatorImpl behaviors.
     */
    class ReplCoordTest : public mongo::unittest::Test {
    public:
        /**
         * Makes a ResponseStatus with the given "doc" response and optional elapsed time "millis".
         */
        static ResponseStatus makeResponseStatus(const BSONObj& doc,
                                                 Milliseconds millis = Milliseconds(0));

        /**
         * Constructs a ReplicaSetConfig from the given BSON, or raises a test failure exception.
         */
        static ReplicaSetConfig assertMakeRSConfig(const BSONObj& configBSON);

        ReplCoordTest();
        virtual ~ReplCoordTest();

    protected:
        virtual void setUp();
        virtual void tearDown();

        /**
         * Gets the network mock.
         */
        NetworkInterfaceMock* getNet() { return _net; }

        /**
         * Gets the replication coordinator under test.
         */
        ReplicationCoordinatorImpl* getReplCoord() { return _repl.get();}

        /**
         * Gets the topology coordinator used by the replication coordinator under test.
         */
        TopologyCoordinatorImpl& getTopoCoord() { return *_topo;}

        /**
         * Gets the external state used by the replication coordinator under test.
         */
        ReplicationCoordinatorExternalStateMock* getExternalState() { return _externalState; }

        /**
         * Adds "selfHost" to the list of hosts that identify as "this" host.
         */
        void addSelf(const HostAndPort& selfHost);

        /**
         * Moves time forward in the network until the new time, and asserts if now!=newTime after
         */
        void assertRunUntil(Date_t newTime);

        /**
         * Shorthand for getNet()->enterNetwork()
         */
        void enterNetwork();

        /**
         * Shorthand for getNet()->exitNetwork()
         */
        void exitNetwork();

        /**
         * Initializes the objects under test; this behavior is optional, in case you need to call
         * any methods on the network or coordinator objects before calling start.
         */
        void init();

        /**
         * Initializes the objects under test, using the given "settings".
         */
        void init(const ReplSettings& settings);

        /**
         * Initializes the objects under test, using "replSet" as the name of the replica set under
         * test.
         */
        void init(const std::string& replSet);

        /**
         * Starts the replication coordinator under test, with no local config document and
         * no notion of what host or hosts are represented by the network interface.
         */
        void start();

        /**
         * Starts the replication coordinator under test, with the given configuration in
         * local storage and the given host name.
         */
        void start(const BSONObj& configDoc, const HostAndPort& selfHost);

        /**
         * Starts the replication coordinator under test with the given host name.
         */
        void start(const HostAndPort& selfHost);

        /**
         * Brings the repl coord from SECONDARY to PRIMARY by simulating the messages required to
         * elect it.
         *
         * Behavior is unspecified if node does not have a clean config, is not in SECONDARY, etc.
         */
        void simulateSuccessfulElection();

        /**
         * Brings the repl coord from PRIMARY to SECONDARY by simulating a period of time in which
         * all heartbeats respond with an error condition, such as time out.
         */
        void simulateStepDownOnIsolation();

        /**
         * Asserts that calling start(configDoc, selfHost) successfully initiates the
         * ReplicationCoordinator under test.
         */
        void assertStartSuccess(const BSONObj& configDoc, const HostAndPort& selfHost);

        /**
         * Shuts down the objects under test.
         */
        void shutdown();

        /**
         * Returns the number of collected log lines containing "needle".
         */
        int64_t countLogLinesContaining(const std::string& needle);

    private:
        boost::scoped_ptr<ReplicationCoordinatorImpl> _repl;
        // Owned by ReplicationCoordinatorImpl
        TopologyCoordinatorImpl* _topo;
        // Owned by ReplicationCoordinatorImpl
        NetworkInterfaceMock* _net;
        // Owned by ReplicationCoordinatorImpl
        ReplicationCoordinatorExternalStateMock* _externalState;
        ReplSettings _settings;
        bool _callShutdown;
    };

}  // namespace repl
}  // namespace mongo
