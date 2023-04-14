/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <string>
#include <vector>

#include "mongo/client/connection_string.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/query/find_command_gen.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/concurrency/spin_lock.h"

namespace mongo {
namespace projection_executor {
class ProjectionExecutor;
}  // namespace projection_executor

const NamespaceString IdentityNS = NamespaceString::createNamespaceString_forTest("local.me");
const BSONField<std::string> HostField("host");

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
    void setCommandReply(const std::string& cmdName, const StatusWith<mongo::BSONObj>& replyObj);

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
                         const std::vector<StatusWith<mongo::BSONObj>>& replySequence);

    /**
     * Inserts a single document to this server.
     *
     * @param nss the namespace to insert the document to.
     * @param obj the document to insert.
     */
    void insert(const NamespaceString& nss, BSONObj obj);

    /**
     * Removes documents from this server.
     *
     * @param nss the namespace to remove documents from.
     * @param filter ignored.
     */
    void remove(const NamespaceString& nss, const BSONObj& filter);

    /**
     * Assign a UUID to a collection
     *
     * @param ns the namespace to be associated with the uuid.
     * @param uuid the uuid to associate with the namespace.
     */
    void assignCollectionUuid(StringData ns, const mongo::UUID& uuid);

    //
    // DBClientBase methods
    //
    rpc::UniqueReply runCommand(InstanceID id, const OpMsgRequest& request);

    /**
     * Finds documents from this mock server according to 'findRequest'.
     */
    mongo::BSONArray find(InstanceID id, const FindCommandRequest& findRequest);

    //
    // Getters
    //

    InstanceID getInstanceID() const;
    mongo::ConnectionString::ConnectionType type() const;
    double getSoTimeout() const;

    /**
     * @returns the value passed to hostAndPort parameter of the
     *     constructor. In other words, doesn't automatically append a
     *     'default' port if none is specified.
     */
    std::string getServerAddress() const;
    std::string toString();
    const HostAndPort& getServerHostAndPort() const;

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
        CircularBSONIterator(const std::vector<StatusWith<mongo::BSONObj>>& replyVector);
        StatusWith<mongo::BSONObj> next();

    private:
        std::vector<StatusWith<mongo::BSONObj>>::iterator _iter;
        std::vector<StatusWith<mongo::BSONObj>> _replyObjs;
    };

    /**
     * Checks whether the instance of the server is still up.
     *
     * @throws mongo::SocketException if this server is down
     */
    void checkIfUp(InstanceID id) const;

    /**
     * Creates a ProjectionExecutor to handle fieldsToReturn.
     */
    std::unique_ptr<projection_executor::ProjectionExecutor> createProjectionExecutor(
        const BSONObj& projectionSpec);

    /**
     * Projects the object, unless the projectionExecutor is null, in which case returns a
     * copy of the object.
     */
    BSONObj project(projection_executor::ProjectionExecutor* projectionExecutor, const BSONObj& o);

    /**
     * Logic shared between 'find()' and 'query()'. This can go away when the legacy 'query()' API
     * is removed.
     */
    mongo::BSONArray findImpl(InstanceID id,
                              const NamespaceStringOrUUID& nsOrUuid,
                              BSONObj projection);

    typedef stdx::unordered_map<std::string, std::shared_ptr<CircularBSONIterator>> CmdToReplyObj;
    typedef stdx::unordered_map<std::string, std::vector<BSONObj>> MockDataMgr;
    typedef stdx::unordered_map<mongo::UUID, std::string, UUID::Hash> UUIDMap;

    bool _isRunning;

    const HostAndPort _hostAndPort;
    long long _delayMilliSec;

    //
    // Mock replies
    //
    CmdToReplyObj _cmdMap;
    MockDataMgr _dataMgr;
    UUIDMap _uuidToNs;

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
}  // namespace mongo
