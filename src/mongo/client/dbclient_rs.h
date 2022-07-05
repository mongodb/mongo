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

/**
 * Connect to a Replica Set, from C++.
 */

#include <utility>

#include "mongo/client/dbclient_connection.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/config.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

class ReplicaSetMonitor;
class TagSet;
struct ReadPreferenceSetting;
typedef std::shared_ptr<ReplicaSetMonitor> ReplicaSetMonitorPtr;

/** Use this class to connect to a replica set of servers.  The class will manage
   checking for which server in a replica set is primary, and do failover automatically.

   This can also be used to connect to replica pairs since pairs are a subset of sets

   On a failover situation, expect at least one operation to return an error (throw
   an exception) before the failover is complete.  Operations are not retried.
*/
class DBClientReplicaSet : public DBClientBase {
public:
    using DBClientBase::find;

    /** Call connect() after constructing. autoReconnect is always on for DBClientReplicaSet
     * connections. */
    DBClientReplicaSet(const std::string& name,
                       const std::vector<HostAndPort>& servers,
                       StringData applicationName,
                       double so_timeout = 0,
                       MongoURI uri = {},
                       const ClientAPIVersionParameters* apiParameters = nullptr);

    /**
     * Returns a non-OK status if no member of the set were reachable. This object
     * can still be used even when non-OK status was returned as it will try to
     * reconnect when you use it later.
     */
    Status connect();

    Status authenticateInternalUser(auth::StepDownBehavior stepDownBehavior) override;

    /**
     * Logs out the connection for the given database.
     *
     * @param dbname the database to logout from.
     * @param info the result object for the logout command (provided for backwards
     *     compatibility with mongo shell)
     */
    void logout(const std::string& dbname, BSONObj& info) override;

    // ----------- simple functions --------------

    std::unique_ptr<DBClientCursor> find(FindCommandRequest findRequest,
                                         const ReadPreferenceSetting& readPref,
                                         ExhaustMode exhaustMode) override;

    void insert(const std::string& ns,
                BSONObj obj,
                bool ordered = true,
                boost::optional<BSONObj> writeConcernObj = boost::none) override;

    /** insert multiple objects.  Note that single object insert is asynchronous, so this version
        is only nominally faster and not worth a special effort to try to use.  */
    void insert(const std::string& ns,
                const std::vector<BSONObj>& v,
                bool ordered = true,
                boost::optional<BSONObj> writeConcernObj = boost::none) override;

    void remove(const std::string& ns,
                const BSONObj& filter,
                bool removeMany = true,
                boost::optional<BSONObj> writeConcernObj = boost::none) override;

    void killCursor(const NamespaceString& ns, long long cursorID) override;

    // ---- access raw connections ----

    /**
     * WARNING: this method is very dangerous - this object can decide to free the
     *     returned primary connection any time.
     *
     * @return the reference to the address that points to the primary connection.
     */
    DBClientConnection& primaryConn();

    /**
     * WARNING: this method is very dangerous - this object can decide to free the
     *     returned primary connection any time. This can also unpin the cached
     *     secondaryOk/read preference connection.
     *
     * @return the reference to the address that points to a secondary connection.
     */
    DBClientConnection& secondaryConn();

    // ---- callback pieces -------

    void say(Message& toSend, bool isRetry = false, std::string* actualServer = nullptr) override;
    Status recv(Message& toRecv, int lastRequestId) override;

    /* this is the callback from our underlying connections to notify us that we got a "not primary"
     * error.
     */
    void isNotPrimary();

    /* this is used to indicate we got a "not primary or secondary" error from a secondary.
     */
    void isntSecondary();

    // ----- status ------

    bool isFailed() const override {
        return !_primary || _primary->isFailed();
    }
    bool isStillConnected() override;

    // ----- informational ----

    /**
     * Gets the replica set name of the set we are connected to.
     */
    const std::string& getSetName() const {
        return _setName;
    }

    /**
     * Returns the HostAndPort of the server this connection believes belongs to the primary,
     * or returns an empty HostAndPort if it doesn't know about a current primary.
     */
    HostAndPort getSuspectedPrimaryHostAndPort() const;

    double getSoTimeout() const override {
        return _so_timeout;
    }

    std::string toString() const override {
        return getServerAddress();
    }

    std::string getServerAddress() const override;

    ConnectionString::ConnectionType type() const override {
        return ConnectionString::ConnectionType::kReplicaSet;
    }

    using DBClientBase::runCommandWithTarget;
    std::pair<rpc::UniqueReply, DBClientBase*> runCommandWithTarget(OpMsgRequest request) final;
    std::pair<rpc::UniqueReply, std::shared_ptr<DBClientBase>> runCommandWithTarget(
        OpMsgRequest request, std::shared_ptr<DBClientBase> me) final;
    DBClientBase* runFireAndForgetCommand(OpMsgRequest request) final;

    void setRequestMetadataWriter(rpc::RequestMetadataWriter writer) final;

    void setReplyMetadataReader(rpc::ReplyMetadataReader reader) final;

    int getMinWireVersion() final;
    int getMaxWireVersion() final;
    // ---- low level ------

    /**
     * Performs a "soft reset" by clearing all states relating to secondary nodes and
     * returning secondary connections to the pool.
     */
    void reset() override;

    bool isReplicaSetMember() const override {
        return true;
    }

    bool isMongos() const override {
        return false;
    }

    /**
     * @bool setting if true, DBClientReplicaSet connections will make sure that secondary
     *    connections are authenticated and log them before returning them to the pool.
     */
    static void setAuthPooledSecondaryConn(bool setting);

#ifdef MONGO_CONFIG_SSL
    const SSLConfiguration* getSSLConfiguration() override;

    bool isTLS() override;
#endif

protected:
    /** Authorize.  Authorizes all nodes as needed
     */
    void _auth(const BSONObj& params) override;

private:
    /**
     * Used to simplify secondary-handling logic on errors
     *
     * @return back the passed cursor
     * @throws DBException if the directed node cannot accept the query because it
     *     is not a primary
     */
    std::unique_ptr<DBClientCursor> checkSecondaryQueryResult(
        std::unique_ptr<DBClientCursor> result);

    DBClientConnection* checkPrimary();

    void _call(Message& toSend, Message& response, std::string* actualServer) override;

    template <typename Authenticate>
    Status _runAuthLoop(Authenticate authCb);

    /**
     * Helper method for selecting a node based on the read preference. Will advance
     * the tag tags object if it cannot find a node that matches the current tag.
     *
     * @param readPref the preference to use for selecting a node.
     *
     * @return a pointer to the new connection object if it can find a good connection.
     *     Otherwise it returns NULL.
     *
     * @throws DBException when an error occurred either when trying to connect to
     *     a node that was thought to be ok or when an assertion happened.
     */
    DBClientConnection* selectNodeUsingTags(std::shared_ptr<ReadPreferenceSetting> readPref);

    /**
     * @return true if the last host used in the last secondaryOk query is still in the
     * set and can be used for the given read preference.
     */
    bool checkLastHost(const ReadPreferenceSetting* readPref);

    /**
     * Destroys all cached information about the last secondaryOk operation and reports the host as
     * failed in the replica set monitor with the specified 'status'.
     */
    void _invalidateLastSecondaryOkCache(const Status& status);

    void _authConnection(DBClientConnection* conn);

    /**
     * Calls logout on the connection for all known database this DBClientRS instance has
     * logged in.
     */
    void logoutAll(DBClientConnection* conn);

    /**
     * Clears the primary connection.
     */
    void resetPrimary();

    /**
     * Clears the secondaryOk connection and returns it to the pool if not the same as _primary.
     */
    void resetSecondaryOkConn();

    // TODO: remove this when processes other than mongos uses the driver version.
    static bool _authPooledSecondaryConn;

    // Throws a DBException if the monitor doesn't exist and there isn't a cached seed to use.
    ReplicaSetMonitorPtr _getMonitor();

    std::string _setName;
    std::string _applicationName;
    std::shared_ptr<ReplicaSetMonitor> _rsm;

    HostAndPort _primaryHost;
    std::shared_ptr<DBClientConnection> _primary;

    // Last used host in a secondaryOk query (can be a primary).
    HostAndPort _lastSecondaryOkHost;
    // Last used connection in a secondaryOk query (can be a primary).
    // Connection can either be owned here or returned to the connection pool. Note that
    // if connection is primary, it is owned by _primary so it is incorrect to return
    // it to the pool.
    std::shared_ptr<DBClientConnection> _lastSecondaryOkConn;
    std::shared_ptr<ReadPreferenceSetting> _lastReadPref;

    double _so_timeout;

    // we need to store so that when we connect to a new node on failure
    // we can re-auth
    // this could be a security issue, as the password is stored in memory
    // not sure if/how we should handle
    bool _internalAuthRequested = false;
    std::map<std::string, BSONObj> _auths;  // dbName -> auth parameters

    MongoURI _uri;

protected:
    DBClientConnection* _lastClient = nullptr;
};
}  // namespace mongo
