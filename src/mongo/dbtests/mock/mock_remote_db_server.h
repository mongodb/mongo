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

#pragma once

#include <boost/shared_ptr.hpp>
#include <string>
#include <vector>

#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/platform/unordered_map.h"
#include "mongo/util/concurrency/spin_lock.h"

namespace mongo {

    const std::string IdentityNS("local.me");
    const BSONField<string> HostField("host");

    /**
     * A very simple mock that acts like a database server. Every object keeps track of its own
     * InstanceID, which initially starts at zero and increments every time it is restarted.
     * This is primarily used for simulating the state of which old connections won't be able
     * to talk to the sockets that has already been closed on this server.
     *
     * Note: All operations on this server are protected by a lock.
     */
    class MockRemoteDBServer {
    public:
        typedef size_t InstanceID;

        /**
         * Creates a new mock server. This can also be setup to work with the
         * ConnectionString class by using mongo::MockConnRegistry as follows:
         *
         * ConnectionString::setConnectionHook(MockConnRegistry::get()->getConnStrHook());
         * MockRemoteDBServer server("$a:27017");
         * MockConnRegistry::get()->addServer(&server);
         *
         * This allows clients using the ConnectionString::connect interface to create
         * connections to this server. The requirements to make this hook fully functional are:
         *
         * 1. hostAndPort of this server should start with $.
         * 2. No other instance has the same hostAndPort as this.
         *
         * This server will also contain the hostAndPort inside the IdentityNS
         * collection. This is convenient for testing query routing.
         *
         * @param hostAndPort the host name with port for this server.
         *
         * @see MockConnRegistry
         */
        MockRemoteDBServer(const std::string& hostAndPort);
        virtual ~MockRemoteDBServer();

        //
        // Connectivity methods
        //

        /**
         * Set a delay for calls to query and runCommand
         */
        void setDelay(long long milliSec);

        /**
         * Shuts down this server. Any operations on this server with an InstanceID
         * less than or equal to the current one will throw a mongo::SocketException.
         * To bring the server up again, use the reboot method.
         */
        void shutdown();

        /**
         * Increments the instanceID of this server.
         */
        void reboot();

        /**
         * @return true if this server is running
         */
        bool isRunning() const;

        //
        // Mocking methods
        //

        /**
         * Sets the reply for a command.
         *
         * @param cmdName the name of the command
         * @param replyObj the exact reply for the command
         */
        void setCommandReply(const std::string& cmdName,
                const mongo::BSONObj& replyObj);

        /**
         * Sets the reply for a command.
         *
         * @param cmdName the name of the command.
         * @param replySequence the sequence of replies to cycle through every time
         *     the given command is requested. This is useful for setting up a
         *     sequence of response when the command can be called more than once
         *     that requires different results when calling a method.
         */
        void setCommandReply(const std::string& cmdName,
                const std::vector<mongo::BSONObj>& replySequence);

        /**
         * Inserts a single document to this server.
         *
         * @param ns the namespace to insert the document to.
         * @param obj the document to insert.
         * @param flags ignored.
         */
        void insert(const string& ns, BSONObj obj, int flags = 0);

        /**
         * Removes documents from this server.
         *
         * @param ns the namespace to remove documents from.
         * @param query ignored.
         * @param flags ignored.
         */
        void remove(const string& ns, Query query, int flags = 0);

        //
        // DBClientBase methods
        //
        bool runCommand(InstanceID id, const std::string& dbname,
                const mongo::BSONObj& cmdObj,
                mongo::BSONObj &info, int options = 0);

        mongo::BSONArray query(InstanceID id,
                const std::string &ns,
                mongo::Query query = mongo::Query(),
                int nToReturn = 0,
                int nToSkip = 0,
                const mongo::BSONObj* fieldsToReturn = 0,
                int queryOptions = 0,
                int batchSize = 0);

        //
        // Getters
        //

        InstanceID getInstanceID() const;
        mongo::ConnectionString::ConnectionType type() const;
        double getSoTimeout() const;

        /**
         * @return the exact string address passed to hostAndPort parameter of the
         *     constructor. In other words, doesn't automatically append a
         *     'default' port if none is specified.
         */
        std::string getServerAddress() const;
        std::string toString();

        //
        // Call counters
        //

        size_t getCmdCount() const;
        size_t getQueryCount() const;
        void clearCounters();

    private:
        /**
         * A very simple class for cycling through a set of BSONObj
         */
        class CircularBSONIterator {
        public:
            /**
             * Creates a new iterator with a deep copy of the vector.
             */
            CircularBSONIterator(const std::vector<mongo::BSONObj>& replyVector);
            mongo::BSONObj next();

        private:
            std::vector<mongo::BSONObj>::iterator _iter;
            std::vector<mongo::BSONObj> _replyObjs;
        };

        /**
         * Checks whether the instance of the server is still up.
         *
         * @throws mongo::SocketException if this server is down
         */
        void checkIfUp(InstanceID id) const;

        typedef unordered_map<std::string, boost::shared_ptr<CircularBSONIterator> > CmdToReplyObj;
        typedef unordered_map<std::string, vector<BSONObj> > MockDataMgr;

        bool _isRunning;

        const std::string _hostAndPort;
        long long _delayMilliSec;

        //
        // Mock replies
        //
        CmdToReplyObj _cmdMap;
        MockDataMgr _dataMgr;

        //
        // Op Counters
        //
        size_t _cmdCount;
        size_t _queryCount;

        // Unique id for every restart of this server used for rejecting requests from
        // connections that are still "connected" to the old instance
        InstanceID _instanceID;

        // protects this entire instance
        mutable mongo::SpinLock _lock;
    };
}
