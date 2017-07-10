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
#include "mongo/bson/util/builder.h"
#include "mongo/client/authenticate.h"
#include "mongo/client/constants.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/config.h"
#include "mongo/db/auth/internal_user_auth.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/json.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/killcursors_request.h"
#include "mongo/db/server_options.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/s/stale_exception.h"  // for RecvStaleConfigException
#include "mongo/stdx/functional.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/log.h"
#include "mongo/util/net/message_port.h"
#include "mongo/util/net/socket_exception.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/password_digest.h"
#include "mongo/util/represent_as.h"
#include "mongo/util/time_support.h"
#include "mongo/util/version.h"

namespace mongo {

using std::unique_ptr;
using std::endl;
using std::list;
using std::map;
using std::string;
using std::stringstream;
using std::vector;

using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;

namespace {

#ifdef MONGO_CONFIG_SSL
static SimpleMutex s_mtx;
static SSLManagerInterface* s_sslMgr(NULL);

SSLManagerInterface* sslManager() {
    stdx::lock_guard<SimpleMutex> lk(s_mtx);
    if (s_sslMgr) {
        return s_sslMgr;
    }

    s_sslMgr = getSSLManager();
    return s_sslMgr;
}
#endif

}  // namespace

AtomicInt64 DBClientBase::ConnectionIdSequence;

/* --- dbclientcommands --- */

bool DBClientBase::isOk(const BSONObj& o) {
    return o["ok"].trueValue();
}

bool DBClientBase::isNotMasterErrorString(const BSONElement& e) {
    return e.type() == String && str::contains(e.valuestr(), "not master");
}


enum QueryOptions DBClientBase::availableOptions() {
    if (!_haveCachedAvailableOptions) {
        _cachedAvailableOptions = _lookupAvailableOptions();
        _haveCachedAvailableOptions = true;
    }
    return _cachedAvailableOptions;
}

enum QueryOptions DBClientBase::_lookupAvailableOptions() {
    BSONObj ret;
    if (runCommand("admin", BSON("availablequeryoptions" << 1), ret)) {
        return QueryOptions(ret.getIntField("options"));
    }
    return QueryOptions(0);
}

rpc::ProtocolSet DBClientBase::getClientRPCProtocols() const {
    return _clientRPCProtocols;
}

rpc::ProtocolSet DBClientBase::getServerRPCProtocols() const {
    return _serverRPCProtocols;
}

void DBClientBase::setClientRPCProtocols(rpc::ProtocolSet protocols) {
    _clientRPCProtocols = std::move(protocols);
}

void DBClientBase::_setServerRPCProtocols(rpc::ProtocolSet protocols) {
    _serverRPCProtocols = std::move(protocols);
}

void DBClientBase::setRequestMetadataWriter(rpc::RequestMetadataWriter writer) {
    _metadataWriter = std::move(writer);
}

const rpc::RequestMetadataWriter& DBClientBase::getRequestMetadataWriter() {
    return _metadataWriter;
}

void DBClientBase::setReplyMetadataReader(rpc::ReplyMetadataReader reader) {
    _metadataReader = std::move(reader);
}

const rpc::ReplyMetadataReader& DBClientBase::getReplyMetadataReader() {
    return _metadataReader;
}

std::pair<rpc::UniqueReply, DBClientBase*> DBClientBase::runCommandWithTarget(
    OpMsgRequest request) {
    // Make sure to reconnect if needed before building our request, since the request depends on
    // the negotiated protocol which can change due to a reconnect.
    checkConnection();

    // call() oddly takes this by pointer, so we need to put it on the stack.
    auto host = getServerAddress();

    auto opCtx = haveClient() ? cc().getOperationContext() : nullptr;
    if (_metadataWriter) {
        BSONObjBuilder metadataBob(std::move(request.body));
        uassertStatusOK(_metadataWriter(opCtx, &metadataBob));
        request.body = metadataBob.obj();
    }

    auto requestMsg =
        rpc::messageFromOpMsgRequest(getClientRPCProtocols(), getServerRPCProtocols(), request);

    Message replyMsg;

    // We always want to throw if there was a network error, we do it here
    // instead of passing 'true' for the 'assertOk' parameter so we can construct a
    // more helpful error message. Note that call() can itself throw a socket exception.
    uassert(ErrorCodes::HostUnreachable,
            str::stream() << "network error while attempting to run "
                          << "command '"
                          << request.getCommandName()
                          << "' "
                          << "on host '"
                          << host
                          << "' ",
            call(requestMsg, replyMsg, false, &host));

    auto commandReply = rpc::makeReply(&replyMsg);

    uassert(ErrorCodes::RPCProtocolNegotiationFailed,
            str::stream() << "Mismatched RPC protocols - request was '"
                          << networkOpToString(requestMsg.operation())
                          << "' '"
                          << " but reply was '"
                          << networkOpToString(replyMsg.operation())
                          << "' ",
            rpc::protocolForMessage(requestMsg) == commandReply->getProtocol());

    if (_metadataReader) {
        uassertStatusOK(_metadataReader(opCtx, commandReply->getMetadata(), host));
    }

    if (ErrorCodes::SendStaleConfig ==
        getStatusFromCommandResult(commandReply->getCommandReply())) {
        throw RecvStaleConfigException("stale config in runCommand",
                                       commandReply->getCommandReply());
    }

    return {rpc::UniqueReply(std::move(replyMsg), std::move(commandReply)), this};
}

std::tuple<bool, DBClientBase*> DBClientBase::runCommandWithTarget(const string& dbname,
                                                                   BSONObj cmd,
                                                                   BSONObj& info,
                                                                   int options) {
    // TODO: This will be downconverted immediately if the underlying
    // requestBuilder is a legacyRequest builder. Not sure what the best
    // way to get around that is without breaking the abstraction.
    auto result = runCommandWithTarget(rpc::upconvertRequest(dbname, std::move(cmd), options));

    info = result.first->getCommandReply().getOwned();
    return std::make_tuple(isOk(info), result.second);
}

bool DBClientBase::runCommand(const string& dbname, BSONObj cmd, BSONObj& info, int options) {
    auto res = runCommandWithTarget(dbname, std::move(cmd), info, options);
    return std::get<0>(res);
}


/* note - we build a bson obj here -- for something that is super common like getlasterror you
          should have that object prebuilt as that would be faster.
*/
bool DBClientBase::simpleCommand(const string& dbname, BSONObj* info, const string& command) {
    BSONObj o;
    if (info == 0)
        info = &o;
    BSONObjBuilder b;
    b.append(command, 1);
    return runCommand(dbname, b.done(), *info);
}

bool DBClientBase::runPseudoCommand(StringData db,
                                    StringData realCommandName,
                                    StringData pseudoCommandCol,
                                    const BSONObj& cmdArgs,
                                    BSONObj& info,
                                    int options) {
    BSONObjBuilder bob;
    bob.append(realCommandName, 1);
    bob.appendElements(cmdArgs);
    auto cmdObj = bob.done();

    bool success = false;

    if (!(success = runCommand(db.toString(), cmdObj, info, options))) {
        auto status = getStatusFromCommandResult(info);
        verify(!status.isOK());

        if (status == ErrorCodes::CommandResultSchemaViolation) {
            msgasserted(28624,
                        str::stream() << "Received bad " << realCommandName
                                      << " response from server: "
                                      << info);
        } else if (status == ErrorCodes::CommandNotFound) {
            NamespaceString pseudoCommandNss(db, pseudoCommandCol);
            // if this throws we just let it escape as that's how runCommand works.
            info = findOne(pseudoCommandNss.ns(), cmdArgs, nullptr, options);
            return true;
        }
    }

    return success;
}

unsigned long long DBClientBase::count(
    const string& myns, const BSONObj& query, int options, int limit, int skip) {
    BSONObj cmd = _countCmd(myns, query, options, limit, skip);
    BSONObj res;
    if (!runCommand(nsToDatabase(myns), cmd, res, options))
        uasserted(11010, string("count fails:") + res.toString());
    return res["n"].numberLong();
}

BSONObj DBClientBase::_countCmd(
    const string& myns, const BSONObj& query, int options, int limit, int skip) {
    NamespaceString ns(myns);
    BSONObjBuilder b;
    b.append("count", ns.coll());
    b.append("query", query);
    if (limit)
        b.append("limit", limit);
    if (skip)
        b.append("skip", skip);
    return b.obj();
}

BSONObj DBClientBase::getLastErrorDetailed(bool fsync, bool j, int w, int wtimeout) {
    return getLastErrorDetailed("admin", fsync, j, w, wtimeout);
}

BSONObj DBClientBase::getLastErrorDetailed(
    const std::string& db, bool fsync, bool j, int w, int wtimeout) {
    BSONObj info;
    BSONObjBuilder b;
    b.append("getlasterror", 1);

    if (fsync)
        b.append("fsync", 1);
    if (j)
        b.append("j", 1);

    // only affects request when greater than one node
    if (w >= 1)
        b.append("w", w);
    else if (w == -1)
        b.append("w", "majority");

    if (wtimeout > 0)
        b.append("wtimeout", wtimeout);

    runCommand(db, b.obj(), info);

    return info;
}

string DBClientBase::getLastError(bool fsync, bool j, int w, int wtimeout) {
    return getLastError("admin", fsync, j, w, wtimeout);
}

string DBClientBase::getLastError(const std::string& db, bool fsync, bool j, int w, int wtimeout) {
    BSONObj info = getLastErrorDetailed(db, fsync, j, w, wtimeout);
    return getLastErrorString(info);
}

string DBClientBase::getLastErrorString(const BSONObj& info) {
    if (info["ok"].trueValue()) {
        BSONElement e = info["err"];
        if (e.eoo())
            return "";
        if (e.type() == Object)
            return e.toString();
        return e.str();
    } else {
        // command failure
        BSONElement e = info["errmsg"];
        if (e.eoo())
            return "";
        if (e.type() == Object)
            return "getLastError command failed: " + e.toString();
        return "getLastError command failed: " + e.str();
    }
}

const BSONObj getpreverrorcmdobj = fromjson("{getpreverror:1}");

BSONObj DBClientBase::getPrevError() {
    BSONObj info;
    runCommand("admin", getpreverrorcmdobj, info);
    return info;
}

string DBClientBase::createPasswordDigest(const string& username, const string& clearTextPassword) {
    return mongo::createPasswordDigest(username, clearTextPassword);
}

namespace {
class ScopedMetadataWriterRemover {
    MONGO_DISALLOW_COPYING(ScopedMetadataWriterRemover);

public:
    ScopedMetadataWriterRemover(DBClientBase* cli)
        : _cli(cli), _oldWriter(cli->getRequestMetadataWriter()) {
        _cli->setRequestMetadataWriter(rpc::RequestMetadataWriter{});
    }
    ~ScopedMetadataWriterRemover() {
        _cli->setRequestMetadataWriter(_oldWriter);
    }

private:
    DBClientBase* const _cli;
    rpc::RequestMetadataWriter _oldWriter;
};
}  // namespace

void DBClientBase::_auth(const BSONObj& params) {
    ScopedMetadataWriterRemover remover{this};

    // We will only have a client name if SSL is enabled
    std::string clientName = "";
#ifdef MONGO_CONFIG_SSL
    if (sslManager() != nullptr) {
        clientName = sslManager()->getSSLConfiguration().clientSubjectName;
    }
#endif

    auth::authenticateClient(
        params,
        HostAndPort(getServerAddress()),
        clientName,
        [this](RemoteCommandRequest request, auth::AuthCompletionHandler handler) {
            BSONObj info;
            auto start = Date_t::now();

            try {
                auto reply = runCommand(
                    OpMsgRequest::fromDBAndBody(request.dbname, request.cmdObj, request.metadata));

                BSONObj data = reply->getCommandReply().getOwned();
                BSONObj metadata = reply->getMetadata().getOwned();
                Milliseconds millis(Date_t::now() - start);

                // Hand control back to authenticateClient()
                handler({data, metadata, millis});

            } catch (...) {
                handler(exceptionToStatus());
            }
        });
}

bool DBClientBase::authenticateInternalUser() {
    if (!isInternalAuthSet()) {
        if (!serverGlobalParams.quiet.load()) {
            log() << "ERROR: No authentication parameters set for internal user";
        }
        return false;
    }

    try {
        auth(getInternalUserAuthParams());
        return true;
    } catch (const UserException& ex) {
        if (!serverGlobalParams.quiet.load()) {
            log() << "can't authenticate to " << toString()
                  << " as internal user, error: " << ex.what();
        }
        return false;
    }
}

void DBClientBase::auth(const BSONObj& params) {
    _auth(params);
}

bool DBClientBase::auth(const string& dbname,
                        const string& username,
                        const string& password_text,
                        string& errmsg,
                        bool digestPassword) {
    try {
        const auto authParams =
            auth::buildAuthParams(dbname, username, password_text, digestPassword);
        auth(authParams);
        return true;
    } catch (const UserException& ex) {
        if (ex.getCode() != ErrorCodes::AuthenticationFailed)
            throw;
        errmsg = ex.what();
        return false;
    }
}

void DBClientBase::logout(const string& dbname, BSONObj& info) {
    runCommand(dbname, BSON("logout" << 1), info);
}

BSONObj ismastercmdobj = fromjson("{\"ismaster\":1}");

bool DBClientBase::isMaster(bool& isMaster, BSONObj* info) {
    BSONObj o;
    if (info == 0)
        info = &o;
    bool ok = runCommand("admin", ismastercmdobj, *info);
    isMaster = info->getField("ismaster").trueValue();
    return ok;
}

bool DBClientBase::createCollection(
    const string& ns, long long size, bool capped, int max, BSONObj* info) {
    verify(!capped || size);
    BSONObj o;
    if (info == 0)
        info = &o;
    BSONObjBuilder b;
    string db = nsToDatabase(ns);
    b.append("create", ns.c_str() + db.length() + 1);
    if (size)
        b.append("size", size);
    if (capped)
        b.append("capped", true);
    if (max)
        b.append("max", max);
    return runCommand(db.c_str(), b.done(), *info);
}

bool DBClientBase::copyDatabase(const string& fromdb,
                                const string& todb,
                                const string& fromhost,
                                BSONObj* info) {
    BSONObj o;
    if (info == 0)
        info = &o;
    BSONObjBuilder b;
    b.append("copydb", 1);
    b.append("fromhost", fromhost);
    b.append("fromdb", fromdb);
    b.append("todb", todb);
    return runCommand("admin", b.done(), *info);
}

bool DBClientBase::eval(const string& dbname,
                        const string& jscode,
                        BSONObj& info,
                        BSONElement& retValue,
                        BSONObj* args) {
    BSONObjBuilder b;
    b.appendCode("$eval", jscode);
    if (args)
        b.appendArray("args", *args);
    bool ok = runCommand(dbname, b.done(), info);
    if (ok)
        retValue = info.getField("retval");
    return ok;
}

bool DBClientBase::eval(const string& dbname, const string& jscode) {
    BSONObj info;
    BSONElement retValue;
    return eval(dbname, jscode, info, retValue);
}

list<BSONObj> DBClientBase::getCollectionInfos(const string& db, const BSONObj& filter) {
    list<BSONObj> infos;

    BSONObj res;
    if (runCommand(db,
                   BSON("listCollections" << 1 << "filter" << filter << "cursor" << BSONObj()),
                   res,
                   QueryOption_SlaveOk)) {
        BSONObj cursorObj = res["cursor"].Obj();
        BSONObj collections = cursorObj["firstBatch"].Obj();
        BSONObjIterator it(collections);
        while (it.more()) {
            BSONElement e = it.next();
            infos.push_back(e.Obj().getOwned());
        }

        const long long id = cursorObj["id"].Long();

        if (id != 0) {
            const std::string ns = cursorObj["ns"].String();
            unique_ptr<DBClientCursor> cursor = getMore(ns, id, 0, 0);
            while (cursor->more()) {
                infos.push_back(cursor->nextSafe().getOwned());
            }
        }

        return infos;
    }

    // command failed

    uasserted(18630, str::stream() << "listCollections failed: " << res);
}

bool DBClientBase::exists(const string& ns) {
    BSONObj filter = BSON("name" << nsToCollectionSubstring(ns));
    list<BSONObj> results = getCollectionInfos(nsToDatabase(ns), filter);
    return !results.empty();
}

/* --- dbclientconnection --- */

void DBClientConnection::_auth(const BSONObj& params) {
    if (autoReconnect) {
        /* note we remember the auth info before we attempt to auth -- if the connection is broken,
         * we will then have it for the next autoreconnect attempt.
         */
        authCache[params[auth::getSaslCommandUserDBFieldName()].str()] = params.getOwned();
    }

    DBClientBase::_auth(params);
}

/** query N objects from the database into an array.  makes sense mostly when you want a small
 * number of results.  if a huge number, use query() and iterate the cursor.
 */
void DBClientBase::findN(vector<BSONObj>& out,
                         const string& ns,
                         Query query,
                         int nToReturn,
                         int nToSkip,
                         const BSONObj* fieldsToReturn,
                         int queryOptions) {
    out.reserve(nToReturn);

    unique_ptr<DBClientCursor> c =
        this->query(ns, query, nToReturn, nToSkip, fieldsToReturn, queryOptions);

    uassert(10276,
            str::stream() << "DBClientBase::findN: transport error: " << getServerAddress()
                          << " ns: "
                          << ns
                          << " query: "
                          << query.toString(),
            c.get());

    if (c->hasResultFlag(ResultFlag_ShardConfigStale)) {
        BSONObj error;
        c->peekError(&error);
        throw RecvStaleConfigException("findN stale config", error);
    }

    for (int i = 0; i < nToReturn; i++) {
        if (!c->more())
            break;
        out.push_back(c->nextSafe());
    }
}

BSONObj DBClientBase::findOne(const string& ns,
                              const Query& query,
                              const BSONObj* fieldsToReturn,
                              int queryOptions) {
    vector<BSONObj> v;
    findN(v, ns, query, 1, 0, fieldsToReturn, queryOptions);
    return v.empty() ? BSONObj() : v[0];
}

namespace {

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

        if (Command::testCommandsEnabled) {
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
        _failed = true;
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

    auto validateStatus =
        rpc::validateWireVersion(WireSpec::instance().outgoing, swProtocolSet.getValue().version);
    if (!validateStatus.isOK()) {
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
            _failed = true;
            _port.reset();
            return validationStatus;
        }
    }

    return Status::OK();
}

namespace {
const auto kMaxMillisCount = Milliseconds::max().count();
}  // namespace

Status DBClientConnection::connectSocketOnly(const HostAndPort& serverAddress) {
    _serverAddress = serverAddress;
    _failed = true;

    // We need to construct a SockAddr so we can resolve the address.
    SockAddr osAddr{serverAddress.host().c_str(),
                    serverAddress.port(),
                    static_cast<sa_family_t>(IPv6Enabled() ? AF_UNSPEC : AF_INET)};

    if (!osAddr.isValid()) {
        return Status(ErrorCodes::InvalidOptions,
                      str::stream() << "couldn't initialize connection to host "
                                    << serverAddress.host()
                                    << ", address is invalid");
    }

    _port.reset(new MessagingPort(_so_timeout, _logLevel));

    if (serverAddress.host().empty()) {
        return Status(ErrorCodes::InvalidOptions,
                      str::stream() << "couldn't connect to server " << _serverAddress.toString()
                                    << ", host is empty");
    }

    if (osAddr.getAddr() == "0.0.0.0") {
        return Status(ErrorCodes::InvalidOptions,
                      str::stream() << "couldn't connect to server " << _serverAddress.toString()
                                    << ", address resolved to 0.0.0.0");
    }

    _resolvedAddress = osAddr.getAddr();

    if (!_port->connect(osAddr)) {
        return Status(ErrorCodes::HostUnreachable,
                      str::stream() << "couldn't connect to server " << _serverAddress.toString()
                                    << ", connection attempt failed");
    }

#ifdef MONGO_CONFIG_SSL
    // Prefer to get SSL mode directly from our URI, but if it is not set, fall back to
    // checking global SSL params. DBClientConnections create through the shell will have a
    // meaningful URI set, but DBClientConnections created from within the server may not.
    int sslMode;
    auto options = _uri.getOptions();
    auto iter = options.find("ssl");
    if (iter != options.end()) {
        if (iter->second == "true") {
            sslMode = SSLParams::SSLMode_requireSSL;
        } else {
            sslMode = SSLParams::SSLMode_disabled;
        }
    } else {
        sslMode = sslGlobalParams.sslMode.load();
    }

    if (sslMode == SSLParams::SSLMode_preferSSL || sslMode == SSLParams::SSLMode_requireSSL) {
        uassert(40312, "SSL is not enabled; cannot create an SSL connection", sslManager());
        if (!_port->secure(sslManager(), serverAddress.host())) {
            return Status(ErrorCodes::SSLHandshakeFailed, "Failed to initialize SSL on connection");
        }
    }
#endif

    _failed = false;
    LOG(1) << "connected to server " << toString() << endl;
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
            handleNotMasterResponse(replyBody["errmsg"]);
        }
    }

    return out;
}

void DBClientConnection::_checkConnection() {
    if (!_failed)
        return;

    if (!autoReconnect)
        throw SocketException(SocketException::FAILED_STATE, toString());

    // Don't hammer reconnects, backoff if needed
    autoReconnectBackoff.nextSleepMillis();

    LOG(_logLevel) << "trying reconnect to " << toString() << endl;
    string errmsg;
    _failed = false;
    auto connectStatus = connect(_serverAddress, _applicationName);
    if (!connectStatus.isOK()) {
        _failed = true;
        LOG(_logLevel) << "reconnect " << toString() << " failed " << errmsg << endl;
        if (connectStatus == ErrorCodes::IncompatibleCatalogManager) {
            uassertStatusOK(connectStatus);  // Will always throw
        } else {
            throw SocketException(SocketException::CONNECT_ERROR, connectStatus.reason());
        }
    }

    LOG(_logLevel) << "reconnect " << toString() << " ok" << endl;
    for (map<string, BSONObj>::const_iterator i = authCache.begin(); i != authCache.end(); i++) {
        try {
            DBClientConnection::_auth(i->second);
        } catch (UserException& ex) {
            if (ex.getCode() != ErrorCodes::AuthenticationFailed)
                throw;
            LOG(_logLevel) << "reconnect: auth failed "
                           << i->second[auth::getSaslCommandUserDBFieldName()]
                           << i->second[auth::getSaslCommandUserFieldName()] << ' ' << ex.what()
                           << std::endl;
        }
    }
}

void DBClientConnection::setSoTimeout(double timeout) {
    _so_timeout = timeout;
    if (_port) {
        // `timeout` is in seconds.
        auto ms = representAs<int64_t>(std::floor(timeout * 1000)).value_or(kMaxMillisCount);
        _port->setTimeout(ms > kMaxMillisCount ? Milliseconds::max() : Milliseconds(ms));
    }
}

uint64_t DBClientConnection::getSockCreationMicroSec() const {
    if (_port) {
        return _port->getSockCreationMicroSec();
    } else {
        return INVALID_SOCK_CREATION_TIME;
    }
}

const uint64_t DBClientBase::INVALID_SOCK_CREATION_TIME =
    static_cast<uint64_t>(0xFFFFFFFFFFFFFFFFULL);

unique_ptr<DBClientCursor> DBClientBase::query(const string& ns,
                                               Query query,
                                               int nToReturn,
                                               int nToSkip,
                                               const BSONObj* fieldsToReturn,
                                               int queryOptions,
                                               int batchSize) {
    unique_ptr<DBClientCursor> c(new DBClientCursor(
        this, ns, query.obj, nToReturn, nToSkip, fieldsToReturn, queryOptions, batchSize));
    if (c->init())
        return c;
    return nullptr;
}

unique_ptr<DBClientCursor> DBClientBase::getMore(const string& ns,
                                                 long long cursorId,
                                                 int nToReturn,
                                                 int options) {
    unique_ptr<DBClientCursor> c(new DBClientCursor(this, ns, cursorId, nToReturn, options));
    if (c->init())
        return c;
    return nullptr;
}

struct DBClientFunConvertor {
    void operator()(DBClientCursorBatchIterator& i) {
        while (i.moreInCurrentBatch()) {
            _f(i.nextSafe());
        }
    }
    stdx::function<void(const BSONObj&)> _f;
};

unsigned long long DBClientBase::query(stdx::function<void(const BSONObj&)> f,
                                       const string& ns,
                                       Query query,
                                       const BSONObj* fieldsToReturn,
                                       int queryOptions) {
    DBClientFunConvertor fun;
    fun._f = f;
    stdx::function<void(DBClientCursorBatchIterator&)> ptr(fun);
    return this->query(ptr, ns, query, fieldsToReturn, queryOptions);
}

unsigned long long DBClientBase::query(stdx::function<void(DBClientCursorBatchIterator&)> f,
                                       const string& ns,
                                       Query query,
                                       const BSONObj* fieldsToReturn,
                                       int queryOptions) {
    // mask options
    queryOptions &= (int)(QueryOption_NoCursorTimeout | QueryOption_SlaveOk);

    unique_ptr<DBClientCursor> c(this->query(ns, query, 0, 0, fieldsToReturn, queryOptions));
    uassert(16090, "socket error for mapping query", c.get());

    unsigned long long n = 0;

    while (c->more()) {
        DBClientCursorBatchIterator i(*c);
        f(i);
        n += i.n();
    }
    return n;
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
        _failed = true;
        _port->shutdown();
        throw;
    }

    return n;
}

void DBClientBase::insert(const string& ns, BSONObj obj, int flags) {
    insert(ns, std::vector<BSONObj>{obj}, flags);
}

void DBClientBase::insert(const string& ns, const vector<BSONObj>& v, int flags) {
    bool ordered = !(flags & InsertOption_ContinueOnError);
    auto nss = NamespaceString(ns);
    auto request =
        OpMsgRequest::fromDBAndBody(nss.db(), BSON("insert" << nss.coll() << "ordered" << ordered));
    request.sequences.push_back({"documents", v});

    // Ignoring reply to match fire-and-forget OP_INSERT behavior.
    runCommand(std::move(request));
}

void DBClientBase::remove(const string& ns, Query obj, int flags) {
    int limit = (flags & RemoveOption_JustOne) ? 1 : 0;
    auto nss = NamespaceString(ns);

    auto request = OpMsgRequest::fromDBAndBody(nss.db(), BSON("delete" << nss.coll()));
    request.sequences.push_back({"deletes", {BSON("q" << obj.obj << "limit" << limit)}});

    // Ignoring reply to match fire-and-forget OP_REMOVE behavior.
    runCommand(std::move(request));
}

void DBClientBase::update(const string& ns, Query query, BSONObj obj, bool upsert, bool multi) {
    auto nss = NamespaceString(ns);

    auto request = OpMsgRequest::fromDBAndBody(nss.db(), BSON("update" << nss.coll()));
    request.sequences.push_back(
        {"updates",
         {BSON("q" << query.obj << "u" << obj << "upsert" << upsert << "multi" << multi)}});

    // Ignoring reply to match fire-and-forget OP_UPDATE behavior.
    runCommand(std::move(request));
}

void DBClientBase::update(const string& ns, Query query, BSONObj obj, int flags) {
    update(ns,
           std::move(query),
           std::move(obj),
           flags & UpdateOption_Upsert,
           flags & UpdateOption_Multi);
}

void DBClientBase::killCursor(const NamespaceString& ns, long long cursorId) {
    // Ignoring reply to match fire-and-forget OP_KILLCURSORS behavior.
    runCommand(OpMsgRequest::fromDBAndBody(ns.db(), KillCursorsRequest(ns, {cursorId}).toBSON()));
}

list<BSONObj> DBClientBase::getIndexSpecs(const string& ns, int options) {
    list<BSONObj> specs;

    BSONObj cmd = BSON("listIndexes" << nsToCollectionSubstring(ns) << "cursor" << BSONObj());

    BSONObj res;
    if (runCommand(nsToDatabase(ns), cmd, res, options)) {
        BSONObj cursorObj = res["cursor"].Obj();
        BSONObjIterator i(cursorObj["firstBatch"].Obj());
        while (i.more()) {
            specs.push_back(i.next().Obj().getOwned());
        }

        const long long id = cursorObj["id"].Long();

        if (id != 0) {
            invariant(ns == cursorObj["ns"].String());
            unique_ptr<DBClientCursor> cursor = getMore(ns, id, 0, 0);
            while (cursor->more()) {
                specs.push_back(cursor->nextSafe().getOwned());
            }
        }

        return specs;
    }
    int code = res["code"].numberInt();

    if (code == ErrorCodes::NamespaceNotFound) {
        return specs;
    }
    uasserted(18631, str::stream() << "listIndexes failed: " << res);
}


void DBClientBase::dropIndex(const string& ns, BSONObj keys) {
    dropIndex(ns, genIndexName(keys));
}


void DBClientBase::dropIndex(const string& ns, const string& indexName) {
    BSONObj info;
    if (!runCommand(nsToDatabase(ns),
                    BSON("deleteIndexes" << nsToCollectionSubstring(ns) << "index" << indexName),
                    info)) {
        LOG(_logLevel) << "dropIndex failed: " << info << endl;
        uassert(10007, "dropIndex failed", 0);
    }
}

void DBClientBase::dropIndexes(const string& ns) {
    BSONObj info;
    uassert(10008,
            "dropIndexes failed",
            runCommand(nsToDatabase(ns),
                       BSON("deleteIndexes" << nsToCollectionSubstring(ns) << "index"
                                            << "*"),
                       info));
}

void DBClientBase::reIndex(const string& ns) {
    BSONObj info;
    uassert(18908,
            str::stream() << "reIndex failed: " << info,
            runCommand(nsToDatabase(ns), BSON("reIndex" << nsToCollectionSubstring(ns)), info));
}


string DBClientBase::genIndexName(const BSONObj& keys) {
    stringstream ss;

    bool first = 1;
    for (BSONObjIterator i(keys); i.more();) {
        BSONElement f = i.next();

        if (first)
            first = 0;
        else
            ss << "_";

        ss << f.fieldName() << "_";
        if (f.isNumber())
            ss << f.numberInt();
        else
            ss << f.str();  // this should match up with shell command
    }
    return ss.str();
}

void DBClientBase::createIndex(StringData ns, const IndexSpec& descriptor) {
    const BSONObj descriptorObj = descriptor.toBSON();

    BSONObjBuilder command;
    command.append("createIndexes", nsToCollectionSubstring(ns));
    {
        BSONArrayBuilder indexes(command.subarrayStart("indexes"));
        indexes.append(descriptorObj);
    }
    const BSONObj commandObj = command.done();

    BSONObj infoObj;
    if (!runCommand(nsToDatabase(ns), commandObj, infoObj)) {
        Status runCommandStatus = getStatusFromCommandResult(infoObj);
        invariant(!runCommandStatus.isOK());
        uassertStatusOK(runCommandStatus);
    }
}

/* -- DBClientCursor ---------------------------------------------- */

DBClientConnection::DBClientConnection(bool _autoReconnect,
                                       double so_timeout,
                                       MongoURI uri,
                                       const HandshakeValidationHook& hook)
    : _failed(false),
      autoReconnect(_autoReconnect),
      autoReconnectBackoff(1000, 2000),
      _so_timeout(so_timeout),
      _hook(hook),
      _uri(std::move(uri)) {
    _numConnections.fetchAndAdd(1);
}

void DBClientConnection::say(Message& toSend, bool isRetry, string* actualServer) {
    checkConnection();
    try {
        toSend.header().setId(nextMessageId());
        toSend.header().setResponseToMsgId(0);
        auto swm = _compressorManager.compressMessage(toSend);
        uassertStatusOK(swm.getStatus());
        port().say(swm.getValue());
    } catch (SocketException&) {
        _failed = true;
        throw;
    }
}

bool DBClientConnection::recv(Message& m, int lastRequestId) {
    if (!port().recv(m)) {
        _failed = true;
        return false;
    }

    uassert(40570,
            "Response ID did not match the sent message ID.",
            m.header().getResponseToMsgId() == lastRequestId);

    if (m.operation() == dbCompressed) {
        auto swm = _compressorManager.decompressMessage(m);
        uassertStatusOK(swm.getStatus());
        m = std::move(swm.getValue());
    }

    return true;
}

bool DBClientConnection::call(Message& toSend,
                              Message& response,
                              bool assertOk,
                              string* actualServer) {
    /* todo: this is very ugly messagingport::call returns an error code AND can throw
             an exception.  we should make it return void and just throw an exception anytime
             it fails
    */
    checkConnection();
    try {
        toSend.header().setId(nextMessageId());
        toSend.header().setResponseToMsgId(0);
        auto swm = _compressorManager.compressMessage(toSend);
        uassertStatusOK(swm.getStatus());

        if (!port().call(swm.getValue(), response)) {
            _failed = true;
            if (assertOk)
                uasserted(10278,
                          str::stream() << "dbclient error communicating with server: "
                                        << getServerAddress());
            return false;
        }

        if (response.operation() == dbCompressed) {
            auto swm = _compressorManager.decompressMessage(response);
            uassertStatusOK(swm.getStatus());
            response = std::move(swm.getValue());
        }
    } catch (SocketException&) {
        _failed = true;
        throw;
    }
    return true;
}

BSONElement getErrField(const BSONObj& o) {
    BSONElement first = o.firstElement();
    if (strcmp(first.fieldName(), "$err") == 0)
        return first;

    // temp - will be DEV only later
    /*DEV*/
    if (1) {
        BSONElement e = o["$err"];
        if (!e.eoo()) {
            wassert(false);
        }
        return e;
    }

    return BSONElement();
}

bool hasErrField(const BSONObj& o) {
    return !getErrField(o).eoo();
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
        handleNotMasterResponse(getErrField(batch[0]));
    }
}

void DBClientConnection::setParentReplSetName(const string& replSetName) {
    _parentReplSetName = replSetName;
}

void DBClientConnection::handleNotMasterResponse(const BSONElement& elemToCheck) {
    if (!isNotMasterErrorString(elemToCheck)) {
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

    _failed = true;
}

AtomicInt32 DBClientConnection::_numConnections;

/** @return the database name portion of an ns string */
string nsGetDB(const string& ns) {
    string::size_type pos = ns.find(".");
    if (pos == string::npos)
        return ns;

    return ns.substr(0, pos);
}

/** @return the collection name portion of an ns string */
string nsGetCollection(const string& ns) {
    string::size_type pos = ns.find(".");
    if (pos == string::npos)
        return "";

    return ns.substr(pos + 1);
}


}  // namespace mongo
