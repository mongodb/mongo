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

#include <boost/none.hpp>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/client/authenticate.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/client/sasl_client_session.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/auth/sasl_command_constants.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/net/socket_exception.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/net/ssl_peer_info.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/version.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {

using std::endl;
using std::map;
using std::string;
using std::unique_ptr;

DBClientConnection::DBClientConnection(bool autoReconnect,
                                       double soTimeout,
                                       MongoURI uri,
                                       const HandshakeValidationHook& hook,
                                       const ClientAPIVersionParameters* apiParameters)
    : DBClientSession(autoReconnect, soTimeout, uri, hook, apiParameters),
      _autoReconnectBackoff(Seconds(1), Seconds(2)) {
    _numConnections.fetchAndAdd(1);
}

StatusWith<std::shared_ptr<transport::Session>> DBClientConnection::_makeSession(
    const HostAndPort& host,
    transport::ConnectSSLMode sslMode,
    Milliseconds timeout,
    boost::optional<TransientSSLParams> transientSSLParams) {
    auto swSession =
        getGlobalServiceContext()->getTransportLayerManager()->getEgressLayer()->connect(
            host,
            transientSSLParams ? transport::kEnableSSL : getURI().getSSLMode(),
            _socketTimeout.value_or(Milliseconds(5000)),
            transientSSLParams);
    return swSession;
}

void DBClientConnection::_auth(const BSONObj& params) {
    if (_autoReconnect) {
        /* note we remember the auth info before we attempt to auth -- if the connection is broken,
         * we will then have it for the next autoreconnect attempt.
         */
        const DatabaseName dbName = AuthDatabaseNameUtil::deserialize(
            params[auth::getSaslCommandUserDBFieldName()].valueStringDataSafe());
        authCache[dbName] = params.getOwned();
    }

    DBClientBase::_auth(params);
}

void DBClientConnection::authenticateInternalUser(auth::StepDownBehavior stepDownBehavior) {
    if (_autoReconnect) {
        _internalAuthOnReconnect = true;
        _internalAuthStepDownBehavior = stepDownBehavior;
    }

    return DBClientBase::authenticateInternalUser(stepDownBehavior);
}

void DBClientConnection::logout(const DatabaseName& dbName, BSONObj& info) {
    authCache.erase(dbName);
    _internalAuthOnReconnect = false;
    runCommand(dbName, BSON("logout" << 1), info);
}

std::pair<rpc::UniqueReply, DBClientBase*> DBClientConnection::runCommandWithTarget(
    OpMsgRequest request) {
    auto out = DBClientBase::runCommandWithTarget(std::move(request));
    if (!_parentReplSetName.empty()) {
        const auto replyBody = out.first->getCommandReply();
        if (!isOk(replyBody)) {
            handleNotPrimaryResponse(replyBody, "errmsg");
        }
    }

    return out;
}

std::pair<rpc::UniqueReply, std::shared_ptr<DBClientBase>> DBClientConnection::runCommandWithTarget(
    OpMsgRequest request, std::shared_ptr<DBClientBase> me) {
    auto out = DBClientBase::runCommandWithTarget(std::move(request), std::move(me));
    if (!_parentReplSetName.empty()) {
        const auto replyBody = out.first->getCommandReply();
        if (!isOk(replyBody)) {
            handleNotPrimaryResponse(replyBody, "errmsg");
        }
    }

    return out;
}

rpc::UniqueReply DBClientConnection::parseCommandReplyMessage(const std::string& host,
                                                              const Message& replyMsg) {
    try {
        return DBClientBase::parseCommandReplyMessage(host, replyMsg);
    } catch (const DBException& ex) {
        if (ErrorCodes::isConnectionFatalMessageParseError(ex.code())) {
            _markFailed(kKillSession);
        }
        throw;
    }
}

void DBClientConnection::_reconnectSession() {
    // Don't hammer reconnects, backoff if needed
    sleepFor(_autoReconnectBackoff.nextSleep());

    LOGV2_DEBUG(20120, _logLevel.toInt(), "Trying to reconnect", "connString"_attr = toString());

    try {
        connect(_serverAddress, _applicationName, _transientSSLParams);
    } catch (const DBException& e) {
        _markFailed(kSetFlag);
        LOGV2_DEBUG(20121,
                    _logLevel.toInt(),
                    "Reconnect attempt failed",
                    "connString"_attr = toString(),
                    "error"_attr = e.toStatus());
        if (e.code() == ErrorCodes::IncompatibleCatalogManager) {
            throw;
        } else {
            throwSocketError(SocketErrorKind::CONNECT_ERROR, e.reason());
        }
    }

    LOGV2_DEBUG(20122, _logLevel.toInt(), "Reconnected", "connString"_attr = toString());
    if (_internalAuthOnReconnect) {
        authenticateInternalUser(_internalAuthStepDownBehavior);
    } else {
        for (const auto& kv : authCache) {
            try {
                DBClientConnection::_auth(kv.second);
            } catch (ExceptionFor<ErrorCodes::AuthenticationFailed>& ex) {
                LOGV2_DEBUG(20123,
                            _logLevel.toInt(),
                            "Reconnect: auth failed",
                            "db"_attr = kv.second[auth::getSaslCommandUserDBFieldName()],
                            "user"_attr = kv.second[auth::getSaslCommandUserFieldName()],
                            "error"_attr = ex.what());
            }
        }
    }
}

void DBClientConnection::_killSession() {
    if (!_session) {
        return;
    }
    _session->end();
}

void DBClientConnection::setParentReplSetName(const string& replSetName) {
    _parentReplSetName = replSetName;
}

void DBClientConnection::handleNotPrimaryResponse(const BSONObj& replyBody,
                                                  StringData errorMsgFieldName) {
    const BSONElement errorMsgElem = replyBody[errorMsgFieldName];
    const BSONElement codeElem = replyBody["code"];

    if (!isNotPrimaryErrorString(errorMsgElem) &&
        !ErrorCodes::isNotPrimaryError(ErrorCodes::Error(codeElem.numberInt()))) {
        return;
    }

    auto monitor = ReplicaSetMonitor::get(_parentReplSetName);
    if (monitor) {
        monitor->failedHost(_serverAddress,
                            {ErrorCodes::NotWritablePrimary,
                             str::stream() << "got not primary from: " << _serverAddress
                                           << " of repl set: " << _parentReplSetName});
    }

    _markFailed(kSetFlag);
}

#ifdef MONGO_CONFIG_SSL
const SSLConfiguration* DBClientConnection::getSSLConfiguration() {
    auto& sslManager = _session->getSSLManager();
    if (!sslManager) {
        return nullptr;
    }
    return &sslManager->getSSLConfiguration();
}

bool DBClientConnection::isUsingTransientSSLParams() const {
    return _transientSSLParams.has_value();
}

bool DBClientConnection::isTLS() {
    return SSLPeerInfo::forSession(_session).isTLS();
}

#endif

AtomicWord<int> DBClientConnection::_numConnections;

}  // namespace mongo
