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

/**
 * Connect to a Mongo database as a database, from C++.
 */


#include "mongo/client/dbclient_base.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/client/authenticate.h"
#include "mongo/client/client_api_version_parameters_gen.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/client/internal_auth.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/api_parameters_gen.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/client.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/kill_cursors_gen.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata.h"
#include "mongo/rpc/protocol.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <limits>
#include <ostream>
#include <utility>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {

using std::list;
using std::string;
using std::stringstream;
using std::unique_ptr;
using std::vector;

AtomicWord<long long> DBClientBase::ConnectionIdSequence;

void (*DBClientBase::withConnection_do_not_use)(std::string host,
                                                std::function<void(DBClientBase*)>) = nullptr;

/* --- dbclientcommands --- */

bool DBClientBase::isOk(const BSONObj& o) {
    return o["ok"].trueValue();
}

bool DBClientBase::isNotPrimaryErrorString(const BSONElement& e) {
    return e.type() == BSONType::string &&
        (str::contains(e.valueStringData(), "not primary") ||
         str::contains(e.valueStringData(), "not master"));
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

rpc::UniqueReply DBClientBase::parseCommandReplyMessage(const std::string& host,
                                                        const Message& replyMsg) {
    auto commandReply = rpc::makeReply(&replyMsg);

    if (_metadataReader) {
        auto opCtx = haveClient() ? cc().getOperationContext() : nullptr;
        uassertStatusOK(_metadataReader(opCtx, commandReply->getCommandReply(), host));
    }

    if (_shouldThrowOnStaleConfigError) {
        auto status = getStatusFromCommandResult(commandReply->getCommandReply());
        if (status == ErrorCodes::StaleConfig) {
            uassertStatusOK(status.withContext("stale config in runCommand"));
        }
    }

    return rpc::UniqueReply(replyMsg, std::move(commandReply));
}

namespace {
void appendMetadata(OperationContext* opCtx,
                    const rpc::RequestMetadataWriter& metadataWriter,
                    const ClientAPIVersionParameters& apiParameters,
                    OpMsgRequest& request) {

    if (!metadataWriter && !apiParameters.getVersion()) {
        return;
    }

    BSONObjBuilder bob(std::move(request.body));
    if (metadataWriter) {
        uassertStatusOK(metadataWriter(opCtx, &bob));
    }

    if (apiParameters.getVersion()) {
        bool hasVersion = false, hasStrict = false, hasDeprecationErrors = false;
        auto i = bob.iterator();
        while (i.more()) {
            auto elem = i.next();
            if (elem.fieldNameStringData() == APIParametersFromClient::kApiVersionFieldName) {
                hasVersion = true;
            } else if (elem.fieldNameStringData() == APIParametersFromClient::kApiStrictFieldName) {
                hasStrict = true;
            } else if (elem.fieldNameStringData() ==
                       APIParametersFromClient::kApiDeprecationErrorsFieldName) {
                hasDeprecationErrors = true;
            }
        }

        if (!hasVersion) {
            bob.append(APIParametersFromClient::kApiVersionFieldName, *apiParameters.getVersion());
        }

        // Include apiStrict/apiDeprecationErrors if they are not boost::none.
        if (!hasStrict && apiParameters.getStrict()) {
            bob.append(APIParametersFromClient::kApiStrictFieldName, *apiParameters.getStrict());
        }

        if (!hasDeprecationErrors && apiParameters.getDeprecationErrors()) {
            bob.append(APIParametersFromClient::kApiDeprecationErrorsFieldName,
                       *apiParameters.getDeprecationErrors());
        }
    }

    request.body = bob.obj();
}
}  // namespace

auth::ValidatedTenancyScope DBClientBase::_createInnerRequestVTS(
    const boost::optional<TenantId>& tenantId) const {
    if (tenantId) {
        return auth::ValidatedTenancyScopeFactory::create(
            tenantId.get(), auth::ValidatedTenancyScopeFactory::TrustedForInnerOpMsgRequestTag{});
    }
    return auth::ValidatedTenancyScope::kNotRequired;
}

DBClientBase* DBClientBase::runFireAndForgetCommand(OpMsgRequest request) {
    // Make sure to reconnect if needed before building our request.
    ensureConnection();

    auto opCtx = haveClient() ? cc().getOperationContext() : nullptr;
    appendMetadata(opCtx, _metadataWriter, _apiParameters, request);
    auto requestMsg = request.serialize();
    OpMsg::setFlag(&requestMsg, OpMsg::kMoreToCome);
    say(requestMsg);
    return this;
}

std::pair<rpc::UniqueReply, DBClientBase*> DBClientBase::runCommandWithTarget(
    OpMsgRequest request) {
    // Make sure to reconnect if needed before building our request.
    ensureConnection();

    // call() oddly takes this by pointer, so we need to put it on the stack.
    auto host = getServerAddress();

    auto opCtx = haveClient() ? cc().getOperationContext() : nullptr;
    appendMetadata(opCtx, _metadataWriter, _apiParameters, request);

    auto requestMsg = request.serialize();
    Message replyMsg;

    try {
        replyMsg = call(requestMsg, &host);
    } catch (DBException& e) {
        e.addContext(str::stream() << str::stream() << "network error while attempting to run "
                                   << "command '" << request.getCommandName() << "' "
                                   << "on host '" << host << "' ");
        throw;
    }

    auto commandReply = parseCommandReplyMessage(host, replyMsg);

    uassert(ErrorCodes::RPCProtocolNegotiationFailed,
            str::stream() << "Mismatched RPC protocols - request was '"
                          << networkOpToString(requestMsg.operation()) << "' '"
                          << " but reply was '" << networkOpToString(replyMsg.operation()) << "' ",
            rpc::protocolForMessage(requestMsg) == commandReply->getProtocol());

    return {std::move(commandReply), this};
}

std::pair<rpc::UniqueReply, std::shared_ptr<DBClientBase>> DBClientBase::runCommandWithTarget(
    OpMsgRequest request, std::shared_ptr<DBClientBase> me) {

    auto out = runCommandWithTarget(std::move(request));
    return {std::move(out.first), std::move(me)};
}

std::tuple<bool, DBClientBase*> DBClientBase::runCommandWithTarget(const DatabaseName& dbName,
                                                                   BSONObj cmd,
                                                                   BSONObj& info,
                                                                   int options) {
    // TODO: This will be downconverted immediately if the underlying
    // requestBuilder is a legacyRequest builder. Not sure what the best
    // way to get around that is without breaking the abstraction.
    auto request =
        rpc::upconvertRequest(dbName, cmd, options, _createInnerRequestVTS(dbName.tenantId()));
    auto result = runCommandWithTarget(std::move(request));

    info = result.first->getCommandReply().getOwned();
    return std::make_tuple(isOk(info), result.second);
}

bool DBClientBase::runCommand(const DatabaseName& dbName, BSONObj cmd, BSONObj& info, int options) {
    auto res = runCommandWithTarget(dbName, std::move(cmd), info, options);
    return std::get<0>(res);
}

StatusWith<std::list<BSONObj>> DBClientBase::runExhaustiveCursorCommand(const DatabaseName& dbName,
                                                                        const BSONObj& cmd,
                                                                        int options) {
    list<BSONObj> docs;

    BSONObj res;
    if (runCommand(dbName, cmd, res, options)) {
        BSONObj cursorObj = res["cursor"].Obj();
        BSONObjIterator it(cursorObj["firstBatch"].Obj());
        while (it.more()) {
            BSONElement e = it.next();
            docs.push_back(e.Obj().getOwned());
        }

        if (res.hasField(LogicalTime::kOperationTimeFieldName)) {
            setOperationTime(LogicalTime::fromOperationTime(res).asTimestamp());
        }

        const long long id = cursorObj["id"].Long();

        if (id != 0) {
            const NamespaceString nss = NamespaceStringUtil::deserialize(
                dbName.tenantId(), cursorObj["ns"].String(), SerializationContext::stateDefault());
            unique_ptr<DBClientCursor> cursor = getMore(nss, id);
            while (cursor->more()) {
                docs.push_back(cursor->nextSafe().getOwned());
            }

            if (cursor->getOperationTime()) {
                setOperationTime(*(cursor->getOperationTime()));
            }
        }

        return docs;
    }

    // command failed
    auto status = getStatusFromCommandResult(res);
    return status.withContext(str::stream() << cmd.firstElementFieldName() << " fails:");
}

long long DBClientBase::count(const NamespaceStringOrUUID& nsOrUuid,
                              const BSONObj& query,
                              int options,
                              int limit,
                              int skip,
                              const boost::optional<repl::ReadConcernArgs>& readConcern) {
    const auto& dbName = nsOrUuid.dbName();

    BSONObj cmd = _countCmd(nsOrUuid, query, options, limit, skip, readConcern);
    BSONObj res;
    if (!runCommand(dbName, cmd, res, options)) {
        auto status = getStatusFromCommandResult(res);
        uassertStatusOK(status.withContext("count fails:"));
    }
    uassert(ErrorCodes::NoSuchKey, "Missing 'n' field for count command.", res.hasField("n"));
    return res["n"].numberLong();
}

BSONObj DBClientBase::_countCmd(const NamespaceStringOrUUID& nsOrUuid,
                                const BSONObj& query,
                                int options,
                                int limit,
                                int skip,
                                const boost::optional<repl::ReadConcernArgs>& readConcern) {
    BSONObjBuilder b;
    if (nsOrUuid.isUUID()) {
        const auto uuid = nsOrUuid.uuid();
        uuid.appendToBuilder(&b, "count");
    } else {
        b.append("count", nsOrUuid.nss().coll());
    }
    b.append("query", query);
    if (limit)
        b.append("limit", limit);
    if (skip)
        b.append("skip", skip);
    if (readConcern) {
        b.append(repl::ReadConcernArgs::kReadConcernFieldName, readConcern->toBSONInner());
    }

    return b.obj();
}

namespace {
class ScopedMetadataWriterRemover {
    ScopedMetadataWriterRemover(const ScopedMetadataWriterRemover&) = delete;
    ScopedMetadataWriterRemover& operator=(const ScopedMetadataWriterRemover&) = delete;

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

auth::RunCommandHook DBClientBase::_makeAuthRunCommandHook() {
    return [this](OpMsgRequest request) -> Future<BSONObj> {
        try {
            auto ret = runCommand(std::move(request));
            auto status = getStatusFromCommandResult(ret->getCommandReply());
            if (!status.isOK()) {
                return status;
            }
            return Future<BSONObj>::makeReady(ret->getCommandReply());
        } catch (const DBException& e) {
            return Future<BSONObj>::makeReady(e.toStatus());
        }
    };
}

void DBClientBase::_auth(const BSONObj& params) {
    ScopedMetadataWriterRemover remover{this};

    // We will only have a client name if SSL is enabled
    std::string clientName = "";
#ifdef MONGO_CONFIG_SSL
    auto sslConfiguration = getSSLConfiguration();
    if (sslConfiguration) {
        clientName = sslConfiguration->clientSubjectName.toString();
    }
#endif

    HostAndPort remote(getServerAddress());
    auth::authenticateClient(params, remote, clientName, _makeAuthRunCommandHook()).get();
}

void DBClientBase::authenticateInternalUser(auth::StepDownBehavior stepDownBehavior) {
    ScopedMetadataWriterRemover remover{this};
    if (!auth::isInternalAuthSet()) {
        if (!serverGlobalParams.quiet.load()) {
            LOGV2(20116, "ERROR: No authentication parameters set for internal user");
        }
        uasserted(ErrorCodes::AuthenticationFailed,
                  "No authentication parameters set for internal user");
    }

    // We will only have a client name if SSL is enabled
    std::string clientName = "";
#ifdef MONGO_CONFIG_SSL
    auto sslConfiguration = getSSLConfiguration();
    if (sslConfiguration) {
        clientName = sslConfiguration->clientSubjectName.toString();
    }
#endif

    auto authProvider = auth::createDefaultInternalAuthProvider();
    try {
        auth::authenticateInternalClient(clientName,
                                         HostAndPort(getServerAddress()),
                                         boost::none,
                                         stepDownBehavior,
                                         _makeAuthRunCommandHook(),
                                         authProvider)
            .get();
    } catch (const DBException& e) {
        if (!serverGlobalParams.quiet.load()) {
            LOGV2(20117,
                  "Can't authenticate as internal user",
                  "connString"_attr = toString(),
                  "error"_attr = e.toStatus());
        }
        throw;
    }
}

void DBClientBase::auth(const BSONObj& params) {
    _auth(params);
}

void DBClientBase::auth(const DatabaseName& dbname, StringData username, StringData password_text) {
    UserName user{username, dbname};

    StatusWith<string> mechResult =
        auth::negotiateSaslMechanism(_makeAuthRunCommandHook(),
                                     user,
                                     boost::none,
                                     auth::StepDownBehavior::kKeepConnectionOpen)
            .getNoThrow();

    // To prevent unexpected behavior for existing clients, default to SCRAM-SHA-1 if the SASL
    // negotiation does not succeeed for some reason.
    StringData mech = mechResult.isOK() ? mechResult.getValue() : "SCRAM-SHA-1"_sd;

    const auto authParams = auth::buildAuthParams(dbname, username, password_text, mech);
    auth(authParams);
}

void DBClientBase::logout(const DatabaseName& dbName, BSONObj& info) {
    runCommand(dbName, BSON("logout" << 1), info);
}

bool DBClientBase::isPrimary(bool& isPrimary, BSONObj* info) {
    BSONObjBuilder bob;
    bob.append("hello", 1);
    ServiceContext* sc = haveClient() ? cc().getServiceContext() : getGlobalServiceContext();
    WireSpec::getWireSpec(sc).appendInternalClientWireVersionIfNeeded(&bob);

    BSONObj o;
    if (info == nullptr)
        info = &o;
    bool ok = runCommand(DatabaseName::kAdmin, bob.obj(), *info);
    isPrimary = info->getField("isWritablePrimary").trueValue();
    return ok;
}

bool DBClientBase::createCollection(const NamespaceString& nss,
                                    long long size,
                                    bool capped,
                                    int max,
                                    BSONObj* info,
                                    boost::optional<BSONObj> writeConcernObj) {
    MONGO_verify(!capped || size);
    BSONObj o;
    if (info == nullptr)
        info = &o;
    BSONObjBuilder b;
    b.append("create", nss.coll());
    if (size)
        b.append("size", size);
    if (capped)
        b.append("capped", true);
    if (max)
        b.append("max", max);
    if (writeConcernObj) {
        b.append(WriteConcernOptions::kWriteConcernField, *writeConcernObj);
    }
    return runCommand(nss.dbName(), b.done(), *info);
}

list<BSONObj> DBClientBase::getCollectionInfos(const DatabaseName& dbName,
                                               const BSONObj& filter,
                                               bool secondaryOk) {
    BSONObj cmdObj = BSON("listCollections" << 1 << "filter" << filter << "cursor" << BSONObj());
    return uassertStatusOK(
        runExhaustiveCursorCommand(dbName, cmdObj, secondaryOk ? QueryOption_SecondaryOk : 0));
}

vector<BSONObj> DBClientBase::getDatabaseInfos(const BSONObj& filter,
                                               const bool nameOnly,
                                               const bool authorizedDatabases,
                                               const bool useListDatabasesForAllTenants) {
    vector<BSONObj> infos;

    BSONObjBuilder bob;
    useListDatabasesForAllTenants ? bob.append("listDatabasesForAllTenants", 1)
                                  : bob.append("listDatabases", 1);
    bob.append("filter", filter);

    if (nameOnly) {
        bob.append("nameOnly", 1);
    }
    if (authorizedDatabases) {
        bob.appendBool("authorizedDatabases", 1);
    }

    BSONObj cmd = bob.done();

    BSONObj res;
    if (runCommand(DatabaseName::kAdmin, cmd, res, QueryOption_SecondaryOk)) {
        BSONObj dbs = res["databases"].Obj();
        BSONObjIterator it(dbs);
        while (it.more()) {
            BSONElement e = it.next();
            infos.push_back(e.Obj().getOwned());
        }

        if (res.hasField(LogicalTime::kOperationTimeFieldName)) {
            setOperationTime(LogicalTime::fromOperationTime(res).asTimestamp());
        }

        return infos;
    }

    uassertStatusOKWithContext(getStatusFromCommandResult(res),
                               str::stream()
                                   << "Command 'listDatabases' failed. Full command: " << cmd);
    MONGO_UNREACHABLE;
}

const uint64_t DBClientBase::INVALID_SOCK_CREATION_TIME = std::numeric_limits<uint64_t>::max();

std::unique_ptr<DBClientCursor> DBClientBase::find(FindCommandRequest findRequest,
                                                   const ReadPreferenceSetting& readPref,
                                                   ExhaustMode exhaustMode) {
    auto cursor = std::make_unique<DBClientCursor>(
        this, std::move(findRequest), readPref, exhaustMode == ExhaustMode::kOn);
    if (cursor->init()) {
        return cursor;
    }
    return nullptr;
}

void DBClientBase::find(FindCommandRequest findRequest,
                        const ReadPreferenceSetting& readPref,
                        ExhaustMode exhaustMode,
                        std::function<void(const BSONObj&)> callback) {
    auto cursor = this->find(std::move(findRequest), readPref, exhaustMode);
    while (cursor->more()) {
        callback(cursor->nextSafe());
    }
}

BSONObj DBClientBase::findOne(FindCommandRequest findRequest,
                              const ReadPreferenceSetting& readPref) {
    tassert(5951200,
            "caller cannot provide a limit when calling DBClientBase::findOne()",
            !findRequest.getLimit());
    findRequest.setLimit(1);
    auto cursor = this->find(std::move(findRequest), readPref, ExhaustMode::kOff);

    uassert(5951201, "DBClientBase::findOne() could not produce cursor", cursor);

    return cursor->more() ? cursor->nextSafe() : BSONObj{};
}

BSONObj DBClientBase::findOne(const NamespaceStringOrUUID& nssOrUuid, BSONObj filter) {
    FindCommandRequest findRequest{nssOrUuid};
    findRequest.setFilter(std::move(filter));
    return findOne(std::move(findRequest));
}

unique_ptr<DBClientCursor> DBClientBase::getMore(const NamespaceString& nss, long long cursorId) {
    unique_ptr<DBClientCursor> c(new DBClientCursor(this, nss, cursorId, false /*isExhaust*/));
    if (c->init())
        return c;
    return nullptr;
}

namespace {
OpMsgRequest createInsertRequest(const auth::ValidatedTenancyScope& vts,
                                 const NamespaceString& nss,
                                 const vector<BSONObj>& v,
                                 bool ordered,
                                 boost::optional<BSONObj> writeConcernObj) {
    BSONObjBuilder cmdBuilder;
    cmdBuilder.append("insert", nss.coll());
    cmdBuilder.append("ordered", ordered);
    if (writeConcernObj) {
        cmdBuilder.append(WriteConcernOptions::kWriteConcernField, *writeConcernObj);
    }
    auto request = OpMsgRequestBuilder::create(vts, nss.dbName(), cmdBuilder.obj());
    request.sequences.push_back({"documents", v});

    return request;
}

OpMsgRequest createUpdateRequest(const auth::ValidatedTenancyScope& vts,
                                 const NamespaceString& nss,
                                 const BSONObj& filter,
                                 BSONObj updateSpec,
                                 bool upsert,
                                 bool multi,
                                 boost::optional<BSONObj> writeConcernObj) {
    BSONObjBuilder cmdBuilder;
    cmdBuilder.append("update", nss.coll());
    if (writeConcernObj) {
        cmdBuilder.append(WriteConcernOptions::kWriteConcernField, *writeConcernObj);
    }
    auto request = OpMsgRequestBuilder::create(vts, nss.dbName(), cmdBuilder.obj());
    request.sequences.push_back(
        {"updates",
         {BSON("q" << filter << "u" << updateSpec << "upsert" << upsert << "multi" << multi)}});

    return request;
}

OpMsgRequest createRemoveRequest(const auth::ValidatedTenancyScope& vts,
                                 const NamespaceString& nss,
                                 const BSONObj& filter,
                                 bool removeMany,
                                 boost::optional<BSONObj> writeConcernObj) {
    const int limit = removeMany ? 0 : 1;

    BSONObjBuilder cmdBuilder;
    cmdBuilder.append("delete", nss.coll());
    if (writeConcernObj) {
        cmdBuilder.append(WriteConcernOptions::kWriteConcernField, *writeConcernObj);
    }
    auto request = OpMsgRequestBuilder::create(vts, nss.dbName(), cmdBuilder.obj());
    request.sequences.push_back({"deletes", {BSON("q" << filter << "limit" << limit)}});

    return request;
}
}  // namespace

BSONObj DBClientBase::insertAcknowledged(const NamespaceString& nss,
                                         const vector<BSONObj>& v,
                                         bool ordered,
                                         boost::optional<BSONObj> writeConcernObj) {
    OpMsgRequest request = createInsertRequest(
        _createInnerRequestVTS(nss.tenantId()), nss, v, ordered, writeConcernObj);
    rpc::UniqueReply reply = runCommand(std::move(request));
    return reply->getCommandReply();
}

void DBClientBase::insert(const NamespaceString& nss,
                          BSONObj obj,
                          bool ordered,
                          boost::optional<BSONObj> writeConcernObj) {
    insert(nss, std::vector<BSONObj>{obj}, ordered, writeConcernObj);
}

void DBClientBase::insert(const NamespaceString& nss,
                          const vector<BSONObj>& v,
                          bool ordered,
                          boost::optional<BSONObj> writeConcernObj) {
    auto request = createInsertRequest(
        _createInnerRequestVTS(nss.tenantId()), nss, v, ordered, writeConcernObj);
    runFireAndForgetCommand(std::move(request));
}

BSONObj DBClientBase::removeAcknowledged(const NamespaceString& nss,
                                         const BSONObj& filter,
                                         bool removeMany,
                                         boost::optional<BSONObj> writeConcernObj) {
    OpMsgRequest request = createRemoveRequest(
        _createInnerRequestVTS(nss.tenantId()), nss, filter, removeMany, writeConcernObj);
    rpc::UniqueReply reply = runCommand(std::move(request));
    return reply->getCommandReply();
}

void DBClientBase::remove(const NamespaceString& nss,
                          const BSONObj& filter,
                          bool removeMany,
                          boost::optional<BSONObj> writeConcernObj) {
    auto request = createRemoveRequest(
        _createInnerRequestVTS(nss.tenantId()), nss, filter, removeMany, writeConcernObj);
    runFireAndForgetCommand(std::move(request));
}

BSONObj DBClientBase::updateAcknowledged(const NamespaceString& nss,
                                         const BSONObj& filter,
                                         BSONObj updateSpec,
                                         bool upsert,
                                         bool multi,
                                         boost::optional<BSONObj> writeConcernObj) {
    auto request = createUpdateRequest(_createInnerRequestVTS(nss.tenantId()),
                                       nss,
                                       filter,
                                       updateSpec,
                                       upsert,
                                       multi,
                                       writeConcernObj);
    rpc::UniqueReply reply = runCommand(std::move(request));
    return reply->getCommandReply();
}

void DBClientBase::update(const NamespaceString& nss,
                          const BSONObj& filter,
                          BSONObj updateSpec,
                          bool upsert,
                          bool multi,
                          boost::optional<BSONObj> writeConcernObj) {
    auto request = createUpdateRequest(_createInnerRequestVTS(nss.tenantId()),
                                       nss,
                                       filter,
                                       updateSpec,
                                       upsert,
                                       multi,
                                       writeConcernObj);
    runFireAndForgetCommand(std::move(request));
}

void DBClientBase::killCursor(const NamespaceString& nss, long long cursorId) {
    runFireAndForgetCommand(
        OpMsgRequestBuilder::create(_createInnerRequestVTS(nss.tenantId()),
                                    nss.dbName(),
                                    KillCursorsCommandRequest(nss, {cursorId}).toBSON()));
}

namespace {

/**
 * Constructs command object for listIndexes.
 */
BSONObj makeListIndexesCommand(const NamespaceStringOrUUID& nsOrUuid, bool includeBuildUUIDs) {
    BSONObjBuilder bob;
    if (nsOrUuid.isNamespaceString()) {
        bob.append("listIndexes", nsOrUuid.nss().coll());
        bob.append("cursor", BSONObj());
    } else {
        const auto& uuid = nsOrUuid.uuid();
        uuid.appendToBuilder(&bob, "listIndexes");
        bob.append("cursor", BSONObj());
    }
    if (includeBuildUUIDs) {
        bob.appendBool("includeBuildUUIDs", true);
    }

    return bob.obj();
}

}  // namespace

std::list<BSONObj> DBClientBase::getIndexSpecs(const NamespaceStringOrUUID& nsOrUuid,
                                               bool includeBuildUUIDs,
                                               int options) {
    auto cmd = makeListIndexesCommand(nsOrUuid, includeBuildUUIDs);
    auto res = runExhaustiveCursorCommand(nsOrUuid.dbName(), cmd, options);

    // "NamespaceNotFound" is an error for UUID but returns an empty list for NamespaceString; this
    // matches the behavior for other commands such as 'find' and 'count'.
    if (nsOrUuid.isNamespaceString() && res == ErrorCodes::NamespaceNotFound) {
        return {};
    }

    return uassertStatusOK(res);
}

void DBClientBase::dropIndex(const NamespaceString& nss,
                             BSONObj keys,
                             boost::optional<BSONObj> writeConcernObj) {
    dropIndex(nss, genIndexName(keys), writeConcernObj);
}


void DBClientBase::dropIndex(const NamespaceString& nss,
                             const string& indexName,
                             boost::optional<BSONObj> writeConcernObj) {
    BSONObjBuilder cmdBuilder;
    cmdBuilder.append("dropIndexes", nss.coll());
    cmdBuilder.append("index", indexName);
    if (writeConcernObj) {
        cmdBuilder.append(WriteConcernOptions::kWriteConcernField, *writeConcernObj);
    }

    BSONObj info;
    if (!runCommand(nss.dbName(), cmdBuilder.obj(), info)) {
        LOGV2_DEBUG(20118, _logLevel.toInt(), "dropIndex failed", "info"_attr = info);
        uassert(10007, "dropIndex failed", 0);
    }
}

void DBClientBase::dropIndexes(const NamespaceString& nss,
                               boost::optional<BSONObj> writeConcernObj) {
    BSONObjBuilder cmdBuilder;
    cmdBuilder.append("dropIndexes", nss.coll());
    cmdBuilder.append("index", "*");
    if (writeConcernObj) {
        cmdBuilder.append(WriteConcernOptions::kWriteConcernField, *writeConcernObj);
    }

    BSONObj info;
    uassert(10008, "dropIndexes failed", runCommand(nss.dbName(), cmdBuilder.obj(), info));
}

void DBClientBase::reIndex(const NamespaceString& nss) {
    BSONObj info;
    uassert(18908,
            str::stream() << "reIndex failed: " << info,
            runCommand(nss.dbName(), BSON("reIndex" << nss.coll()), info));
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

void DBClientBase::createIndexes(const NamespaceString& nss,
                                 const std::vector<const IndexSpec*>& descriptors,
                                 boost::optional<BSONObj> writeConcernObj) {
    BSONObjBuilder command;
    command.append("createIndexes", nss.coll());

    {
        BSONArrayBuilder indexes(command.subarrayStart("indexes"));
        for (const auto& desc : descriptors) {
            indexes.append(desc->toBSON());
        }
    }
    if (writeConcernObj) {
        command.append(WriteConcernOptions::kWriteConcernField, *writeConcernObj);
    }
    const BSONObj commandObj = command.done();

    BSONObj infoObj;
    if (!runCommand(nss.dbName(), commandObj, infoObj)) {
        Status runCommandStatus = getStatusFromCommandResult(infoObj);
        invariant(!runCommandStatus.isOK());
        uassertStatusOK(runCommandStatus);
    }
}

void DBClientBase::createIndexes(const NamespaceString& nss,
                                 const std::vector<BSONObj>& specs,
                                 boost::optional<BSONObj> writeConcernObj) {
    BSONObjBuilder command;
    command.append("createIndexes", nss.coll());

    {
        BSONArrayBuilder indexes(command.subarrayStart("indexes"));
        for (const auto& spec : specs) {
            indexes.append(spec);
        }
    }
    if (writeConcernObj) {
        command.append(WriteConcernOptions::kWriteConcernField, *writeConcernObj);
    }
    const BSONObj commandObj = command.done();

    BSONObj infoObj;
    if (!runCommand(nss.dbName(), commandObj, infoObj)) {
        Status runCommandStatus = getStatusFromCommandResult(infoObj);
        invariant(!runCommandStatus.isOK());
        uassertStatusOK(runCommandStatus);
    }
}

BSONElement getErrField(const BSONObj& o) {
    return o["$err"];
}

bool hasErrField(const BSONObj& o) {
    return !getErrField(o).eoo();
}

/** @return the database name portion of an ns string */
string nsGetDB(const string& ns) {
    string::size_type pos = ns.find('.');
    if (pos == string::npos)
        return ns;

    return ns.substr(0, pos);
}

/** @return the collection name portion of an ns string */
string nsGetCollection(const string& ns) {
    string::size_type pos = ns.find('.');
    if (pos == string::npos)
        return "";

    return ns.substr(pos + 1);
}

Timestamp DBClientBase::getOperationTime() {
    return _lastOperationTime;
}

void DBClientBase::setOperationTime(Timestamp operationTime) {
    _lastOperationTime = operationTime;
}

}  // namespace mongo
