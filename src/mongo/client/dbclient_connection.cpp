// dbclient.cpp - connect to a Mongo database as a database, from C++

/*    Copyright 2009 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/client/dbclientinterface.h"

#include <algorithm>
#include <utility>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/authenticate.h"
#include "mongo/client/constants.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/config.h"
#include "mongo/db/auth/internal_user_auth.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/json.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/killcursors_request.h"
#include "mongo/db/server_options.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/s/is_mongos.h"
#include "mongo/s/stale_exception.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/net/socket_exception.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/password_digest.h"
#include "mongo/util/time_support.h"
#include "mongo/util/version.h"

namespace mongo {

using std::unique_ptr;
using std::endl;
using std::map;
using std::string;

namespace {

MONGO_FAIL_POINT_DEFINE(turnOffDBClientIncompatibleWithUpgradedServerCheck);

/**
 * RAII class to force usage of OP_QUERY on a connection.
 */
class ScopedForceOpQuery {
public:
    ScopedForceOpQuery(DBClientBase* conn)
        : _conn(conn), _oldProtos(conn->getClientRPCProtocols()) {
        _conn->setClientRPCProtocols(rpc::supports::kOpQueryOnly);
    }

    ~ScopedForceOpQuery() {
        _conn->setClientRPCProtocols(_oldProtos);
    }

private:
    DBClientBase* const _conn;
    const rpc::ProtocolSet _oldProtos;
};

/**
* Initializes the wire version of conn, and returns the isMaster reply.
*/
executor::RemoteCommandResponse initWireVersion(DBClientConnection* conn,
                                                StringData applicationName) {
    try {
        // We need to force the usage of OP_QUERY on this command, even if we have previously
        // detected support for OP_COMMAND on a connection. This is necessary to handle the case
        // where we reconnect to an older version of MongoDB running at the same host/port.
        ScopedForceOpQuery forceOpQuery{conn};

        BSONObjBuilder bob;
        bob.append("isMaster", 1);

        if (getTestCommandsEnabled()) {
            // Only include the host:port of this process in the isMaster command request if test
            // commands are enabled. mongobridge uses this field to identify the process opening a
            // connection to it.
            StringBuilder sb;
            sb << getHostName() << ':' << serverGlobalParams.port;
            bob.append("hostInfo", sb.str());
        }

        auto versionString = VersionInfoInterface::instance().version();

        Status serializeStatus = ClientMetadata::serialize(
            "MongoDB Internal Client", versionString, applicationName, &bob);
        if (!serializeStatus.isOK()) {
            return serializeStatus;
        }

        conn->getCompressorManager().clientBegin(&bob);

        if (WireSpec::instance().isInternalClient) {
            WireSpec::appendInternalClientWireVersion(WireSpec::instance().outgoing, &bob);
        }

        Date_t start{Date_t::now()};
        auto result = conn->runCommand(OpMsgRequest::fromDBAndBody("admin", bob.obj()));
        Date_t finish{Date_t::now()};

        BSONObj isMasterObj = result->getCommandReply().getOwned();

        if (isMasterObj.hasField("minWireVersion") && isMasterObj.hasField("maxWireVersion")) {
            int minWireVersion = isMasterObj["minWireVersion"].numberInt();
            int maxWireVersion = isMasterObj["maxWireVersion"].numberInt();
            conn->setWireVersions(minWireVersion, maxWireVersion);
        }

        conn->getCompressorManager().clientFinish(isMasterObj);

        return executor::RemoteCommandResponse{
            std::move(isMasterObj), result->getMetadata().getOwned(), finish - start};

    } catch (...) {
        return exceptionToStatus();
    }
}

}  // namespace

void DBClientConnection::_auth(const BSONObj& params) {
    if (autoReconnect) {
        /* note we remember the auth info before we attempt to auth -- if the connection is broken,
         * we will then have it for the next autoreconnect attempt.
         */
        authCache[params[auth::getSaslCommandUserDBFieldName()].str()] = params.getOwned();
    }

    DBClientBase::_auth(params);
}

bool DBClientConnection::connect(const HostAndPort& server,
                                 StringData applicationName,
                                 std::string& errmsg) {
    auto connectStatus = connect(server, applicationName);
    if (!connectStatus.isOK()) {
        errmsg = connectStatus.reason();
        return false;
    }
    return true;
}

Status DBClientConnection::connect(const HostAndPort& serverAddress, StringData applicationName) {
    auto connectStatus = connectSocketOnly(serverAddress);
    if (!connectStatus.isOK()) {
        return connectStatus;
    }

    // NOTE: If the 'applicationName' parameter is a view of the '_applicationName' member, as
    // happens, for instance, in the call to DBClientConnection::connect from
    // DBClientConnection::_checkConnection then the following line will invalidate the
    // 'applicationName' parameter, since the memory that it views within _applicationName will be
    // freed. Do not reference the 'applicationName' parameter after this line. If you need to
    // access the application name, do it through the _applicationName member.
    _applicationName = applicationName.toString();

    auto swIsMasterReply = initWireVersion(this, _applicationName);
    if (!swIsMasterReply.isOK()) {
        _markFailed(kSetFlag);
        return swIsMasterReply.status;
    }

    // Ensure that the isMaster response is "ok:1".
    auto isMasterStatus = getStatusFromCommandResult(swIsMasterReply.data);
    if (!isMasterStatus.isOK()) {
        return isMasterStatus;
    }

    auto swProtocolSet = rpc::parseProtocolSetFromIsMasterReply(swIsMasterReply.data);
    if (!swProtocolSet.isOK()) {
        return swProtocolSet.getStatus();
    }

    {
        // The Server Discovery and Monitoring (SDAM) specification identifies a replica set member
        // as either (a) having a "setName" field in the isMaster response, or (b) having
        // "isreplicaset: true" in the isMaster response.
        //
        // https://github.com/mongodb/specifications/blob/c386e23724318e2fa82f4f7663d77581b755b2c3/
        // source/server-discovery-and-monitoring/server-discovery-and-monitoring.rst#type
        const bool hasSetNameField = swIsMasterReply.data.hasField("setName");
        const bool isReplicaSetField = swIsMasterReply.data.getBoolField("isreplicaset");
        _isReplicaSetMember = hasSetNameField || isReplicaSetField;
    }

    {
        std::string msgField;
        auto msgFieldExtractStatus = bsonExtractStringField(swIsMasterReply.data, "msg", &msgField);

        if (msgFieldExtractStatus == ErrorCodes::NoSuchKey) {
            _isMongos = false;
        } else if (!msgFieldExtractStatus.isOK()) {
            return msgFieldExtractStatus;
        } else {
            _isMongos = (msgField == "isdbgrid");
        }
    }

    auto validateStatus =
        rpc::validateWireVersion(WireSpec::instance().outgoing, swProtocolSet.getValue().version);
    if (!validateStatus.isOK()) {
        if (mongo::isMongos() && validateStatus == ErrorCodes::IncompatibleWithUpgradedServer &&
            !MONGO_FAIL_POINT(turnOffDBClientIncompatibleWithUpgradedServerCheck)) {
            severe() << "This mongos server must be upgraded. It is attempting to communicate with "
                        "an upgraded cluster with which it is incompatible. Error: '"
                     << validateStatus.toString()
                     << "' Crashing in order to bring attention to the incompatibility, rather "
                        "than erroring endlessly.";
            fassertNoTrace(50709, false);
        }

        warning() << "remote host has incompatible wire version: " << validateStatus;

        return validateStatus;
    }

    _setServerRPCProtocols(swProtocolSet.getValue().protocolSet);

    auto negotiatedProtocol = rpc::negotiate(
        getServerRPCProtocols(), rpc::computeProtocolSet(WireSpec::instance().outgoing));

    if (!negotiatedProtocol.isOK()) {
        return negotiatedProtocol.getStatus();
    }

    if (_hook) {
        auto validationStatus = _hook(swIsMasterReply);
        if (!validationStatus.isOK()) {
            // Disconnect and mark failed.
            _markFailed(kReleaseSession);
            return validationStatus;
        }
    }

    return Status::OK();
}

Status DBClientConnection::connectSocketOnly(const HostAndPort& serverAddress) {
    _serverAddress = serverAddress;
    _markFailed(kReleaseSession);

    if (serverAddress.host().empty()) {
        return Status(ErrorCodes::InvalidOptions,
                      str::stream() << "couldn't connect to server " << _serverAddress.toString()
                                    << ", host is empty");
    }

    if (serverAddress.host() == "0.0.0.0") {
        return Status(ErrorCodes::InvalidOptions,
                      str::stream() << "couldn't connect to server " << _serverAddress.toString()
                                    << ", address resolved to 0.0.0.0");
    }

    transport::ConnectSSLMode sslMode = transport::kGlobalSSLMode;
#ifdef MONGO_CONFIG_SSL
    // Prefer to get SSL mode directly from our URI, but if it is not set, fall back to
    // checking global SSL params. DBClientConnections create through the shell will have a
    // meaningful URI set, but DBClientConnections created from within the server may not.
    auto options = _uri.getOptions();
    auto iter = options.find("ssl");
    if (iter != options.end()) {
        if (iter->second == "true") {
            sslMode = transport::kEnableSSL;
        } else {
            sslMode = transport::kDisableSSL;
        }
    }

#endif

    auto tl = getGlobalServiceContext()->getTransportLayer();
    auto sws = tl->connect(serverAddress, sslMode, _socketTimeout.value_or(Milliseconds{5000}));
    if (!sws.isOK()) {
        return Status(ErrorCodes::HostUnreachable,
                      str::stream() << "couldn't connect to server " << _serverAddress.toString()
                                    << ", connection attempt failed: "
                                    << sws.getStatus());
    }

    _session = std::move(sws.getValue());
    _sessionCreationMicros = curTimeMicros64();
    _lastConnectivityCheck = Date_t::now();
    _session->setTimeout(_socketTimeout);
    _session->setTags(_tagMask);
    _failed = false;
    LOG(1) << "connected to server " << toString();
    return Status::OK();
}

void DBClientConnection::logout(const string& dbname, BSONObj& info) {
    authCache.erase(dbname);
    runCommand(dbname, BSON("logout" << 1), info);
}

std::pair<rpc::UniqueReply, DBClientBase*> DBClientConnection::runCommandWithTarget(
    OpMsgRequest request) {
    auto out = DBClientBase::runCommandWithTarget(std::move(request));
    if (!_parentReplSetName.empty()) {
        const auto replyBody = out.first->getCommandReply();
        if (!isOk(replyBody)) {
            handleNotMasterResponse(replyBody, "errmsg");
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
            handleNotMasterResponse(replyBody, "errmsg");
        }
    }

    return out;
}

rpc::UniqueReply DBClientConnection::parseCommandReplyMessage(const std::string& host,
                                                              const Message& replyMsg) {
    try {
        return DBClientBase::parseCommandReplyMessage(host, std::move(replyMsg));
    } catch (const DBException& ex) {
        if (ErrorCodes::isConnectionFatalMessageParseError(ex.code())) {
            _markFailed(kEndSession);
        }
        throw;
    }
}

void DBClientConnection::_markFailed(FailAction action) {
    _failed = true;
    if (_session) {
        if (action == kEndSession) {
            _session->end();
        } else if (action == kReleaseSession) {
            _session.reset();
        }
    }
}

bool DBClientConnection::isStillConnected() {
    // This method tries to figure out whether the connection is still open, but with several
    // caveats.

    // If we don't have a _session then we may have hit an error, or we may just not have
    // connected yet - the _failed flag should indicate which.
    //
    // Otherwise, return false if we know we've had an error (_failed is true)
    if (!_session) {
        return !_failed;
    } else if (_failed) {
        return false;
    }

    // Checking whether the socket actually has an error by calling _session->isConnected()
    // is actually pretty expensive, so we cache the result for 5 seconds
    auto now = getGlobalServiceContext()->getFastClockSource()->now();
    if (now - _lastConnectivityCheck < Seconds{5}) {
        return true;
    }

    _lastConnectivityCheck = now;

    // This will poll() the underlying socket and do a 1 byte recv to see if the connection
    // has been closed.
    return _session->isConnected();
}

void DBClientConnection::setTags(transport::Session::TagMask tags) {
    _tagMask = tags;
    if (!_session)
        return;
    _session->setTags(tags);
}

void DBClientConnection::shutdown() {
    _markFailed(kEndSession);
}

void DBClientConnection::_checkConnection() {
    if (!_failed)
        return;

    if (!autoReconnect)
        throwSocketError(SocketErrorKind::FAILED_STATE, toString());

    // Don't hammer reconnects, backoff if needed
    autoReconnectBackoff.nextSleepMillis();

    LOG(_logLevel) << "trying reconnect to " << toString() << endl;
    string errmsg;
    _failed = false;
    auto connectStatus = connect(_serverAddress, _applicationName);
    if (!connectStatus.isOK()) {
        _markFailed(kSetFlag);
        LOG(_logLevel) << "reconnect " << toString() << " failed " << errmsg << endl;
        if (connectStatus == ErrorCodes::IncompatibleCatalogManager) {
            uassertStatusOK(connectStatus);  // Will always throw
        } else {
            throwSocketError(SocketErrorKind::CONNECT_ERROR, connectStatus.reason());
        }
    }

    LOG(_logLevel) << "reconnect " << toString() << " ok" << endl;
    for (map<string, BSONObj>::const_iterator i = authCache.begin(); i != authCache.end(); i++) {
        try {
            DBClientConnection::_auth(i->second);
        } catch (AssertionException& ex) {
            if (ex.code() != ErrorCodes::AuthenticationFailed)
                throw;
            LOG(_logLevel) << "reconnect: auth failed "
                           << i->second[auth::getSaslCommandUserDBFieldName()]
                           << i->second[auth::getSaslCommandUserFieldName()] << ' ' << ex.what()
                           << std::endl;
        }
    }
}

void DBClientConnection::setSoTimeout(double timeout) {
    Milliseconds::rep timeoutMs = std::floor(timeout * 1000);
    if (timeout <= 0) {
        _socketTimeout = boost::none;
    } else if (timeoutMs >= Milliseconds::max().count()) {
        _socketTimeout = Milliseconds::max();
    } else {
        _socketTimeout = Milliseconds{timeoutMs};
    }

    if (_session) {
        _session->setTimeout(_socketTimeout);
    }
}

uint64_t DBClientConnection::getSockCreationMicroSec() const {
    if (_session) {
        return _sessionCreationMicros;
    } else {
        return INVALID_SOCK_CREATION_TIME;
    }
}

unsigned long long DBClientConnection::query(stdx::function<void(DBClientCursorBatchIterator&)> f,
                                             const string& ns,
                                             Query query,
                                             const BSONObj* fieldsToReturn,
                                             int queryOptions) {
    if (!(availableOptions() & QueryOption_Exhaust)) {
        return DBClientBase::query(f, ns, query, fieldsToReturn, queryOptions);
    }

    // mask options
    queryOptions &= (int)(QueryOption_NoCursorTimeout | QueryOption_SlaveOk);
    queryOptions |= (int)QueryOption_Exhaust;

    unique_ptr<DBClientCursor> c(this->query(ns, query, 0, 0, fieldsToReturn, queryOptions));
    uassert(13386, "socket error for mapping query", c.get());

    unsigned long long n = 0;

    try {
        while (1) {
            while (c->moreInCurrentBatch()) {
                DBClientCursorBatchIterator i(*c);
                f(i);
                n += i.n();
            }

            if (c->getCursorId() == 0)
                break;

            c->exhaustReceiveMore();
        }
    } catch (std::exception&) {
        /* connection CANNOT be used anymore as more data may be on the way from the server.
           we have to reconnect.
           */
        _markFailed(kEndSession);
        throw;
    }

    return n;
}

DBClientConnection::DBClientConnection(bool _autoReconnect,
                                       double so_timeout,
                                       MongoURI uri,
                                       const HandshakeValidationHook& hook)
    : _failed(false),
      autoReconnect(_autoReconnect),
      autoReconnectBackoff(1000, 2000),
      _hook(hook),
      _uri(std::move(uri)) {
    _numConnections.fetchAndAdd(1);
}

void DBClientConnection::say(Message& toSend, bool isRetry, string* actualServer) {
    checkConnection();
    auto killSessionOnError = MakeGuard([this] { _markFailed(kEndSession); });

    toSend.header().setId(nextMessageId());
    toSend.header().setResponseToMsgId(0);
    uassertStatusOK(
        _session->sinkMessage(uassertStatusOK(_compressorManager.compressMessage(toSend))));
    killSessionOnError.Dismiss();
}

bool DBClientConnection::recv(Message& m, int lastRequestId) {
    auto killSessionOnError = MakeGuard([this] { _markFailed(kEndSession); });
    auto swm = _session->sourceMessage();
    if (!swm.isOK()) {
        return false;
    }

    m = std::move(swm.getValue());
    uassert(40570,
            "Response ID did not match the sent message ID.",
            m.header().getResponseToMsgId() == lastRequestId);

    if (m.operation() == dbCompressed) {
        m = uassertStatusOK(_compressorManager.decompressMessage(m));
    }

    killSessionOnError.Dismiss();
    return true;
}

bool DBClientConnection::call(Message& toSend,
                              Message& response,
                              bool assertOk,
                              string* actualServer) {
    checkConnection();
    auto killSessionOnError = MakeGuard([this] { _markFailed(kEndSession); });
    auto maybeThrow = [&](const auto& errStatus) {
        if (assertOk)
            uasserted(10278,
                      str::stream() << "dbclient error communicating with server "
                                    << getServerAddress()
                                    << ": "
                                    << redact(errStatus));
        return false;
    };

    toSend.header().setId(nextMessageId());
    toSend.header().setResponseToMsgId(0);
    auto swm = _compressorManager.compressMessage(toSend);
    uassertStatusOK(swm.getStatus());

    auto sinkStatus = _session->sinkMessage(swm.getValue());
    if (!sinkStatus.isOK()) {
        return maybeThrow(sinkStatus);
    }

    swm = _session->sourceMessage();
    if (swm.isOK()) {
        response = std::move(swm.getValue());
    } else {
        return maybeThrow(swm.getStatus());
    }

    if (response.operation() == dbCompressed) {
        response = uassertStatusOK(_compressorManager.decompressMessage(response));
    }

    killSessionOnError.Dismiss();
    return true;
}

void DBClientConnection::checkResponse(const std::vector<BSONObj>& batch,
                                       bool networkError,
                                       bool* retry,
                                       string* host) {
    /* check for errors.  the only one we really care about at
     * this stage is "not master"
    */

    *retry = false;
    *host = _serverAddress.toString();

    if (!_parentReplSetName.empty() && !batch.empty()) {
        handleNotMasterResponse(batch[0], "$err");
    }
}

void DBClientConnection::setParentReplSetName(const string& replSetName) {
    _parentReplSetName = replSetName;
}

void DBClientConnection::handleNotMasterResponse(const BSONObj& replyBody,
                                                 StringData errorMsgFieldName) {
    const BSONElement errorMsgElem = replyBody[errorMsgFieldName];
    const BSONElement codeElem = replyBody["code"];

    if (!isNotMasterErrorString(errorMsgElem) &&
        !ErrorCodes::isNotMasterError(ErrorCodes::Error(codeElem.numberInt()))) {
        return;
    }

    ReplicaSetMonitorPtr monitor = ReplicaSetMonitor::get(_parentReplSetName);
    if (monitor) {
        monitor->failedHost(_serverAddress,
                            {ErrorCodes::NotMaster,
                             str::stream() << "got not master from: " << _serverAddress
                                           << " of repl set: "
                                           << _parentReplSetName});
    }

    _markFailed(kSetFlag);
}

AtomicInt32 DBClientConnection::_numConnections;

}  // namespace mongo
