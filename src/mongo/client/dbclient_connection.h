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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/authenticate.h"
#include "mongo/client/client_api_version_parameters_gen.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/client/dbclient_session.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/executor/remote_command_response.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/net/ssl_types.h"
#include "mongo/util/time_support.h"

#include <functional>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {

namespace executor {
struct RemoteCommandResponse;
}

class DBClientCursor;

/**
 *  A basic connection to the database.
 *  This is the main entry point for talking to a simple Mongo setup
 */
class DBClientConnection : public DBClientSession {
public:
    DBClientConnection(bool autoReconnect = false,
                       double soTimeout = 0,
                       MongoURI uri = {},
                       const HandshakeValidationHook& hook = HandshakeValidationHook(),
                       const ClientAPIVersionParameters* apiParameters = nullptr);

    ~DBClientConnection() override {
        _numConnections.fetchAndAdd(-1);
    }

    /**
     * Logs out the connection for the given database.
     *
     * @param dbname the database to logout from.
     * @param info the result object for the logout command (provided for backwards
     *     compatibility with mongo shell)
     */
    void logout(const DatabaseName& dbName, BSONObj& info) override;

    using DBClientBase::runCommandWithTarget;
    std::pair<rpc::UniqueReply, DBClientBase*> runCommandWithTarget(OpMsgRequest request) override;
    std::pair<rpc::UniqueReply, std::shared_ptr<DBClientBase>> runCommandWithTarget(
        OpMsgRequest request, std::shared_ptr<DBClientBase> me) override;

    rpc::UniqueReply parseCommandReplyMessage(const std::string& host,
                                              const Message& replyMsg) override;

    static int getNumConnections() {
        return _numConnections.load();
    }

    /**
     * Set the name of the replica set that this connection is associated to.
     * Note: There is no validation on replSetName.
     */
    void setParentReplSetName(const std::string& replSetName);

    void authenticateInternalUser(
        auth::StepDownBehavior stepDownBehavior = auth::StepDownBehavior::kKillConnection) override;

#ifdef MONGO_CONFIG_SSL
    const SSLConfiguration* getSSLConfiguration() override;

    bool isUsingTransientSSLParams() const override;
#endif

protected:
    void _auth(const BSONObj& params) override;

    Backoff _autoReconnectBackoff;

    bool _internalAuthOnReconnect = false;

    auth::StepDownBehavior _internalAuthStepDownBehavior = auth::StepDownBehavior::kKillConnection;

    absl::flat_hash_map<DatabaseName, BSONObj> authCache;

    static AtomicWord<int> _numConnections;

private:
    StatusWith<std::shared_ptr<transport::Session>> _makeSession(
        const HostAndPort& host,
        transport::ConnectSSLMode sslMode,
        Milliseconds timeout,
        const boost::optional<TransientSSLParams>& transientSSLParams = boost::none) override;
    void _reconnectSession() override;
    void _killSession() override;

    /**
     * Inspects the contents of 'replyBody' and informs the replica set monitor that the host 'this'
     * is connected with is no longer the primary if a "not primary" error message or error code was
     * returned.
     */
    void handleNotPrimaryResponse(const BSONObj& replyBody, StringData errorMsgFieldName);

    // Contains the string for the replica set name of the host this is connected to.
    // Should be empty if this connection is not pointing to a replica set member.
    std::string _parentReplSetName;
};

}  // namespace mongo
