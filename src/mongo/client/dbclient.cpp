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

#include <algorithm>
#include <utility>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/authenticate.h"
#include "mongo/client/constants.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/config.h"
#include "mongo/db/auth/internal_user_auth.h"
#include "mongo/db/commands.h"
#include "mongo/db/json.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/server_options.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/rpc/request_builder_interface.h"
#include "mongo/s/stale_exception.h"  // for RecvStaleConfigException
#include "mongo/stdx/functional.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/log.h"
#include "mongo/util/net/asio_message_port.h"
#include "mongo/util/net/message_port.h"
#include "mongo/util/net/message_port_startup_param.h"
#include "mongo/util/net/socket_exception.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/password_digest.h"
#include "mongo/util/represent_as.h"
#include "mongo/util/time_support.h"

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

bool DBClientWithCommands::isOk(const BSONObj& o) {
    return o["ok"].trueValue();
}

bool DBClientWithCommands::isNotMasterErrorString(const BSONElement& e) {
    return e.type() == String && str::contains(e.valuestr(), "not master");
}


enum QueryOptions DBClientWithCommands::availableOptions() {
    if (!_haveCachedAvailableOptions) {
        _cachedAvailableOptions = _lookupAvailableOptions();
        _haveCachedAvailableOptions = true;
    }
    return _cachedAvailableOptions;
}

enum QueryOptions DBClientWithCommands::_lookupAvailableOptions() {
    BSONObj ret;
    if (runCommand("admin", BSON("availablequeryoptions" << 1), ret)) {
        return QueryOptions(ret.getIntField("options"));
    }
    return QueryOptions(0);
}

rpc::ProtocolSet DBClientWithCommands::getClientRPCProtocols() const {
    return _clientRPCProtocols;
}

rpc::ProtocolSet DBClientWithCommands::getServerRPCProtocols() const {
    return _serverRPCProtocols;
}

void DBClientWithCommands::setClientRPCProtocols(rpc::ProtocolSet protocols) {
    _clientRPCProtocols = std::move(protocols);
}

void DBClientWithCommands::_setServerRPCProtocols(rpc::ProtocolSet protocols) {
    _serverRPCProtocols = std::move(protocols);
}

void DBClientWithCommands::setRequestMetadataWriter(rpc::RequestMetadataWriter writer) {
    _metadataWriter = std::move(writer);
}

const rpc::RequestMetadataWriter& DBClientWithCommands::getRequestMetadataWriter() {
    return _metadataWriter;
}

void DBClientWithCommands::setReplyMetadataReader(rpc::ReplyMetadataReader reader) {
    _metadataReader = std::move(reader);
}

const rpc::ReplyMetadataReader& DBClientWithCommands::getReplyMetadataReader() {
    return _metadataReader;
}

rpc::UniqueReply DBClientWithCommands::runCommandWithMetadata(StringData database,
                                                              StringData command,
                                                              const BSONObj& metadata,
                                                              const BSONObj& commandArgs) {
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "Database name '" << database << "' is not valid.",
            NamespaceString::validDBName(database, NamespaceString::DollarInDbNameBehavior::Allow));

    // call() oddly takes this by pointer, so we need to put it on the stack.
    auto host = getServerAddress();

    BSONObjBuilder metadataBob;
    metadataBob.appendElements(metadata);

    if (_metadataWriter) {
        uassertStatusOK(_metadataWriter(&metadataBob, host));
    }

    auto requestBuilder = rpc::makeRequestBuilder(getClientRPCProtocols(), getServerRPCProtocols());

    requestBuilder->setDatabase(database);
    requestBuilder->setCommandName(command);
    requestBuilder->setCommandArgs(commandArgs);
    requestBuilder->setMetadata(metadataBob.done());
    auto requestMsg = requestBuilder->done();

    Message replyMsg;

    // We always want to throw if there was a network error, we do it here
    // instead of passing 'true' for the 'assertOk' parameter so we can construct a
    // more helpful error message. Note that call() can itself throw a socket exception.
    uassert(ErrorCodes::HostUnreachable,
            str::stream() << "network error while attempting to run "
                          << "command '"
                          << command
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
            requestBuilder->getProtocol() == commandReply->getProtocol());

    if (ErrorCodes::SendStaleConfig ==
        getStatusFromCommandResult(commandReply->getCommandReply())) {
        throw RecvStaleConfigException("stale config in runCommand",
                                       commandReply->getCommandReply());
    }

    if (_metadataReader) {
        uassertStatusOK(_metadataReader(commandReply->getMetadata(), host));
    }

    return rpc::UniqueReply(std::move(replyMsg), std::move(commandReply));
}

bool DBClientWithCommands::runCommand(const string& dbname,
                                      const BSONObj& cmd,
                                      BSONObj& info,
                                      int options) {
    BSONObj upconvertedCmd;
    BSONObj upconvertedMetadata;

    // TODO: This will be downconverted immediately if the underlying
    // requestBuilder is a legacyRequest builder. Not sure what the best
    // way to get around that is without breaking the abstraction.
    std::tie(upconvertedCmd, upconvertedMetadata) =
        uassertStatusOK(rpc::upconvertRequestMetadata(cmd, options));

    auto commandName = upconvertedCmd.firstElementFieldName();

    auto result = runCommandWithMetadata(dbname, commandName, upconvertedMetadata, upconvertedCmd);

    info = result->getCommandReply().getOwned();

    return isOk(info);
}

/* note - we build a bson obj here -- for something that is super common like getlasterror you
          should have that object prebuilt as that would be faster.
*/
bool DBClientWithCommands::simpleCommand(const string& dbname,
                                         BSONObj* info,
                                         const string& command) {
    BSONObj o;
    if (info == 0)
        info = &o;
    BSONObjBuilder b;
    b.append(command, 1);
    return runCommand(dbname, b.done(), *info);
}

bool DBClientWithCommands::runPseudoCommand(StringData db,
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

unsigned long long DBClientWithCommands::count(
    const string& myns, const BSONObj& query, int options, int limit, int skip) {
    BSONObj cmd = _countCmd(myns, query, options, limit, skip);
    BSONObj res;
    if (!runCommand(nsToDatabase(myns), cmd, res, options))
        uasserted(11010, string("count fails:") + res.toString());
    return res["n"].numberLong();
}

BSONObj DBClientWithCommands::_countCmd(
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

BSONObj DBClientWithCommands::getLastErrorDetailed(bool fsync, bool j, int w, int wtimeout) {
    return getLastErrorDetailed("admin", fsync, j, w, wtimeout);
}

BSONObj DBClientWithCommands::getLastErrorDetailed(
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

string DBClientWithCommands::getLastError(bool fsync, bool j, int w, int wtimeout) {
    return getLastError("admin", fsync, j, w, wtimeout);
}

string DBClientWithCommands::getLastError(
    const std::string& db, bool fsync, bool j, int w, int wtimeout) {
    BSONObj info = getLastErrorDetailed(db, fsync, j, w, wtimeout);
    return getLastErrorString(info);
}

string DBClientWithCommands::getLastErrorString(const BSONObj& info) {
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

BSONObj DBClientWithCommands::getPrevError() {
    BSONObj info;
    runCommand("admin", getpreverrorcmdobj, info);
    return info;
}

string DBClientWithCommands::createPasswordDigest(const string& username,
                                                  const string& clearTextPassword) {
    return mongo::createPasswordDigest(username, clearTextPassword);
}

namespace {
class ScopedMetadataWriterRemover {
    MONGO_DISALLOW_COPYING(ScopedMetadataWriterRemover);

public:
    ScopedMetadataWriterRemover(DBClientWithCommands* cli)
        : _cli(cli), _oldWriter(cli->getRequestMetadataWriter()) {
        _cli->setRequestMetadataWriter(rpc::RequestMetadataWriter{});
    }
    ~ScopedMetadataWriterRemover() {
        _cli->setRequestMetadataWriter(_oldWriter);
    }

private:
    DBClientWithCommands* const _cli;
    rpc::RequestMetadataWriter _oldWriter;
};
}  // namespace

void DBClientWithCommands::_auth(const BSONObj& params) {
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
        HostAndPort(getServerAddress()).host(),
        clientName,
        [this](RemoteCommandRequest request, auth::AuthCompletionHandler handler) {
            BSONObj info;
            auto start = Date_t::now();

            auto commandName = request.cmdObj.firstElementFieldName();

            try {
                auto reply = runCommandWithMetadata(
                    request.dbname, commandName, request.metadata, request.cmdObj);

                BSONObj data = reply->getCommandReply().getOwned();
                BSONObj metadata = reply->getMetadata().getOwned();
                Milliseconds millis(Date_t::now() - start);

                // Hand control back to authenticateClient()
                handler(StatusWith<RemoteCommandResponse>(
                    RemoteCommandResponse(data, metadata, millis)));

            } catch (...) {
                handler(exceptionToStatus());
            }
        });
}

bool DBClientWithCommands::authenticateInternalUser() {
    if (!isInternalAuthSet()) {
        if (!serverGlobalParams.quiet) {
            log() << "ERROR: No authentication parameters set for internal user";
        }
        return false;
    }

    try {
        auth(getInternalUserAuthParamsWithFallback());
        return true;
    } catch (const UserException& ex) {
        if (!serverGlobalParams.quiet) {
            log() << "can't authenticate to " << toString()
                  << " as internal user, error: " << ex.what();
        }
        return false;
    }
}

void DBClientWithCommands::auth(const BSONObj& params) {
    _auth(params);
}

bool DBClientWithCommands::auth(const string& dbname,
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

void DBClientWithCommands::logout(const string& dbname, BSONObj& info) {
    runCommand(dbname, BSON("logout" << 1), info);
}

BSONObj ismastercmdobj = fromjson("{\"ismaster\":1}");

bool DBClientWithCommands::isMaster(bool& isMaster, BSONObj* info) {
    BSONObj o;
    if (info == 0)
        info = &o;
    bool ok = runCommand("admin", ismastercmdobj, *info);
    isMaster = info->getField("ismaster").trueValue();
    return ok;
}

bool DBClientWithCommands::createCollection(
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

bool DBClientWithCommands::copyDatabase(const string& fromdb,
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

bool DBClientWithCommands::eval(const string& dbname,
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

bool DBClientWithCommands::eval(const string& dbname, const string& jscode) {
    BSONObj info;
    BSONElement retValue;
    return eval(dbname, jscode, info, retValue);
}

list<string> DBClientWithCommands::getDatabaseNames() {
    BSONObj info;
    uassert(10005,
            "listdatabases failed",
            runCommand("admin", BSON("listDatabases" << 1), info, QueryOption_SlaveOk));
    uassert(10006, "listDatabases.databases not array", info["databases"].type() == Array);

    list<string> names;

    BSONObjIterator i(info["databases"].embeddedObjectUserCheck());
    while (i.more()) {
        names.push_back(i.next().embeddedObjectUserCheck()["name"].valuestr());
    }

    return names;
}

list<string> DBClientWithCommands::getCollectionNames(const string& db) {
    list<BSONObj> infos = getCollectionInfos(db);
    list<string> names;
    for (list<BSONObj>::iterator it = infos.begin(); it != infos.end(); ++it) {
        names.push_back(db + "." + (*it)["name"].valuestr());
    }
    return names;
}

list<BSONObj> DBClientWithCommands::getCollectionInfos(const string& db, const BSONObj& filter) {
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

bool DBClientWithCommands::exists(const string& ns) {
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
void DBClientInterface::findN(vector<BSONObj>& out,
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
        out.push_back(c->nextSafeOwned());
    }
}

BSONObj DBClientInterface::findOne(const string& ns,
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
StatusWith<executor::RemoteCommandResponse> initWireVersion(DBClientConnection* conn) {
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

        Date_t start{Date_t::now()};
        auto result =
            conn->runCommandWithMetadata("admin", "isMaster", rpc::makeEmptyMetadata(), bob.done());
        Date_t finish{Date_t::now()};

        BSONObj isMasterObj = result->getCommandReply().getOwned();

        if (isMasterObj.hasField("minWireVersion") && isMasterObj.hasField("maxWireVersion")) {
            int minWireVersion = isMasterObj["minWireVersion"].numberInt();
            int maxWireVersion = isMasterObj["maxWireVersion"].numberInt();
            conn->setWireVersions(minWireVersion, maxWireVersion);
        }

        return executor::RemoteCommandResponse{
            std::move(isMasterObj), result->getMetadata().getOwned(), finish - start};

    } catch (...) {
        return exceptionToStatus();
    }
}

}  // namespace

bool DBClientConnection::connect(const HostAndPort& server, std::string& errmsg) {
    auto connectStatus = connect(server);
    if (!connectStatus.isOK()) {
        errmsg = connectStatus.reason();
        return false;
    }
    return true;
}

Status DBClientConnection::connect(const HostAndPort& serverAddress) {
    auto connectStatus = connectSocketOnly(serverAddress);
    if (!connectStatus.isOK()) {
        return connectStatus;
    }

    auto swIsMasterReply = initWireVersion(this);
    if (!swIsMasterReply.isOK()) {
        _failed = true;
        return swIsMasterReply.getStatus();
    }

    // Ensure that the isMaster response is "ok:1".
    auto isMasterStatus = getStatusFromCommandResult(swIsMasterReply.getValue().data);
    if (!isMasterStatus.isOK()) {
        return isMasterStatus;
    }

    auto swProtocolSet = rpc::parseProtocolSetFromIsMasterReply(swIsMasterReply.getValue().data);
    if (!swProtocolSet.isOK()) {
        return swProtocolSet.getStatus();
    }

    _setServerRPCProtocols(swProtocolSet.getValue());

    auto negotiatedProtocol =
        rpc::negotiate(getServerRPCProtocols(),
                       rpc::computeProtocolSet(WireSpec::instance().minWireVersionOutgoing,
                                               WireSpec::instance().maxWireVersionOutgoing));

    if (!negotiatedProtocol.isOK()) {
        return negotiatedProtocol.getStatus();
    }

    if (_hook) {
        auto validationStatus = _hook(swIsMasterReply.getValue());
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
    SockAddr osAddr{serverAddress.host().c_str(), serverAddress.port()};

    if (!osAddr.isValid()) {
        return Status(ErrorCodes::InvalidOptions,
                      str::stream() << "couldn't initialize connection to host "
                                    << serverAddress.host()
                                    << ", address is invalid");
    }

    if (isMessagePortImplASIO()) {
        // `_so_timeout` is in seconds.
        auto ms = representAs<int64_t>(std::floor(_so_timeout * 1000)).value_or(kMaxMillisCount);
        _port.reset(new ASIOMessagingPort(
            ms > kMaxMillisCount ? Milliseconds::max() : Milliseconds(ms), _logLevel));
    } else {
        _port.reset(new MessagingPort(_so_timeout, _logLevel));
    }

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
    int sslModeVal = sslGlobalParams.sslMode.load();
    if (sslModeVal == SSLParams::SSLMode_preferSSL || sslModeVal == SSLParams::SSLMode_requireSSL) {
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

bool DBClientConnection::runCommand(const string& dbname,
                                    const BSONObj& cmd,
                                    BSONObj& info,
                                    int options) {
    if (DBClientWithCommands::runCommand(dbname, cmd, info, options))
        return true;

    if (!_parentReplSetName.empty()) {
        handleNotMasterResponse(info["errmsg"]);
    }

    return false;
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
    auto connectStatus = connect(_serverAddress);
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
    BufBuilder b;

    int reservedFlags = 0;
    if (flags & InsertOption_ContinueOnError)
        reservedFlags |= Reserved_InsertOption_ContinueOnError;

    b.appendNum(reservedFlags);
    b.appendStr(ns);
    obj.appendSelfToBufBuilder(b);

    Message toSend;
    toSend.setData(dbInsert, b.buf(), b.len());

    say(toSend);
}

// TODO: Merge with other insert implementation?
void DBClientBase::insert(const string& ns, const vector<BSONObj>& v, int flags) {
    BufBuilder b;

    int reservedFlags = 0;
    if (flags & InsertOption_ContinueOnError)
        reservedFlags |= Reserved_InsertOption_ContinueOnError;

    b.appendNum(reservedFlags);
    b.appendStr(ns);
    for (vector<BSONObj>::const_iterator i = v.begin(); i != v.end(); ++i)
        i->appendSelfToBufBuilder(b);

    Message toSend;
    toSend.setData(dbInsert, b.buf(), b.len());

    say(toSend);
}

void DBClientBase::remove(const string& ns, Query obj, int flags) {
    BufBuilder b;

    const int reservedFlags = 0;
    b.appendNum(reservedFlags);
    b.appendStr(ns);
    b.appendNum(flags);

    obj.obj.appendSelfToBufBuilder(b);

    Message toSend;
    toSend.setData(dbDelete, b.buf(), b.len());

    say(toSend);
}

void DBClientBase::update(const string& ns, Query query, BSONObj obj, bool upsert, bool multi) {
    int flags = 0;
    if (upsert)
        flags |= UpdateOption_Upsert;
    if (multi)
        flags |= UpdateOption_Multi;
    update(ns, query, obj, flags);
}

void DBClientBase::update(const string& ns, Query query, BSONObj obj, int flags) {
    BufBuilder b;

    const int reservedFlags = 0;
    b.appendNum(reservedFlags);
    b.appendStr(ns);
    b.appendNum(flags);

    query.obj.appendSelfToBufBuilder(b);
    obj.appendSelfToBufBuilder(b);

    Message toSend;
    toSend.setData(dbUpdate, b.buf(), b.len());

    say(toSend);
}

void DBClientBase::killCursor(long long cursorId) {
    StackBufBuilder b;
    b.appendNum((int)0);  // reserved
    b.appendNum((int)1);  // number
    b.appendNum(cursorId);

    Message m;
    m.setData(dbKillCursors, b.buf(), b.len());
    say(m);
}

list<BSONObj> DBClientWithCommands::getIndexSpecs(const string& ns, int options) {
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
            const std::string ns = cursorObj["ns"].String();
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


void DBClientWithCommands::dropIndex(const string& ns, BSONObj keys) {
    dropIndex(ns, genIndexName(keys));
}


void DBClientWithCommands::dropIndex(const string& ns, const string& indexName) {
    BSONObj info;
    if (!runCommand(nsToDatabase(ns),
                    BSON("deleteIndexes" << nsToCollectionSubstring(ns) << "index" << indexName),
                    info)) {
        LOG(_logLevel) << "dropIndex failed: " << info << endl;
        uassert(10007, "dropIndex failed", 0);
    }
}

void DBClientWithCommands::dropIndexes(const string& ns) {
    BSONObj info;
    uassert(10008,
            "dropIndexes failed",
            runCommand(nsToDatabase(ns),
                       BSON("deleteIndexes" << nsToCollectionSubstring(ns) << "index"
                                            << "*"),
                       info));
}

void DBClientWithCommands::reIndex(const string& ns) {
    BSONObj info;
    uassert(18908,
            str::stream() << "reIndex failed: " << info,
            runCommand(nsToDatabase(ns), BSON("reIndex" << nsToCollectionSubstring(ns)), info));
}


string DBClientWithCommands::genIndexName(const BSONObj& keys) {
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

void DBClientWithCommands::createIndex(StringData ns, const IndexSpec& descriptor) {
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
                                       const HandshakeValidationHook& hook)
    : _failed(false),
      autoReconnect(_autoReconnect),
      autoReconnectBackoff(1000, 2000),
      _so_timeout(so_timeout),
      _hook(hook) {
    _numConnections.fetchAndAdd(1);
}

void DBClientConnection::say(Message& toSend, bool isRetry, string* actualServer) {
    checkConnection();
    try {
        port().say(toSend);
    } catch (SocketException&) {
        _failed = true;
        throw;
    }
}

bool DBClientConnection::recv(Message& m) {
    if (port().recv(m)) {
        return true;
    }

    _failed = true;
    return false;
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
        if (!port().call(toSend, response)) {
            _failed = true;
            if (assertOk)
                uasserted(10278,
                          str::stream() << "dbclient error communicating with server: "
                                        << getServerAddress());
            return false;
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

void DBClientConnection::checkResponse(const char* data, int nReturned, bool* retry, string* host) {
    /* check for errors.  the only one we really care about at
     * this stage is "not master"
    */

    *retry = false;
    *host = _serverAddress.toString();

    if (!_parentReplSetName.empty() && nReturned) {
        verify(data);
        BSONObj bsonView(data);
        handleNotMasterResponse(getErrField(bsonView));
    }
}

void DBClientConnection::setParentReplSetName(const string& replSetName) {
    _parentReplSetName = replSetName;
}

void DBClientConnection::handleNotMasterResponse(const BSONElement& elemToCheck) {
    if (!isNotMasterErrorString(elemToCheck)) {
        return;
    }

    MONGO_LOG_COMPONENT(1, logger::LogComponent::kReplication)
        << "got not master from: " << _serverAddress << " of repl set: " << _parentReplSetName;

    ReplicaSetMonitorPtr monitor = ReplicaSetMonitor::get(_parentReplSetName);
    if (monitor) {
        monitor->failedHost(_serverAddress);
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
