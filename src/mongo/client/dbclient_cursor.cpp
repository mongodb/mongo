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

#include "mongo/client/dbclient_cursor.h"

#include <memory>

#include "mongo/client/connpool.h"
#include "mongo/db/client.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/getmore_command_gen.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/destructor_guard.h"
#include "mongo/util/exit.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {

using std::endl;
using std::string;
using std::unique_ptr;
using std::vector;

namespace {
BSONObj addMetadata(DBClientBase* client, BSONObj command) {
    if (client->getRequestMetadataWriter()) {
        BSONObjBuilder builder(command);
        auto opCtx = (haveClient() ? cc().getOperationContext() : nullptr);
        uassertStatusOK(client->getRequestMetadataWriter()(opCtx, &builder));
        return builder.obj();
    } else {
        return command;
    }
}

Message assembleCommandRequest(DBClientBase* cli,
                               StringData database,
                               int legacyQueryOptions,
                               BSONObj legacyQuery) {
    auto request = rpc::upconvertRequest(database, std::move(legacyQuery), legacyQueryOptions);
    request.body = addMetadata(cli, std::move(request.body));
    return request.serialize();
}

Message assembleFromFindCommandRequest(DBClientBase* client,
                                       StringData database,
                                       const FindCommandRequest& request,
                                       const ReadPreferenceSetting& readPref) {
    BSONObj findCmd = request.toBSON(BSONObj());

    // Add the $readPreference field to the request.
    {
        BSONObjBuilder builder{findCmd};
        readPref.toContainingBSON(&builder);
        findCmd = builder.obj();
    }

    findCmd = addMetadata(client, std::move(findCmd));
    auto opMsgRequest = OpMsgRequest::fromDBAndBody(database, findCmd);
    return opMsgRequest.serialize();
}

std::unique_ptr<FindCommandRequest> fromLegacyQuery(NamespaceStringOrUUID nssOrUuid,
                                                    const BSONObj& filter,
                                                    const client_deprecated::Query& querySettings,
                                                    const BSONObj& proj,
                                                    int ntoskip,
                                                    int queryOptions) {
    auto findCommand = std::make_unique<FindCommandRequest>(std::move(nssOrUuid));

    client_deprecated::initFindFromLegacyOptions(
        querySettings.getFullSettingsDeprecated(), queryOptions, findCommand.get());

    findCommand->setFilter(filter.getOwned());

    if (!proj.isEmpty()) {
        findCommand->setProjection(proj.getOwned());
    }
    if (ntoskip) {
        findCommand->setSkip(ntoskip);
    }

    uassertStatusOK(query_request_helper::validateFindCommandRequest(*findCommand));

    return findCommand;
}

int queryOptionsFromFindCommand(const FindCommandRequest& findCmd,
                                const ReadPreferenceSetting& readPref,
                                bool isExhaust) {
    int queryOptions = 0;
    if (readPref.canRunOnSecondary()) {
        queryOptions = queryOptions | QueryOption_SecondaryOk;
    }
    if (findCmd.getTailable()) {
        queryOptions = queryOptions | QueryOption_CursorTailable;
    }
    if (findCmd.getNoCursorTimeout()) {
        queryOptions = queryOptions | QueryOption_NoCursorTimeout;
    }
    if (findCmd.getAwaitData()) {
        queryOptions = queryOptions | QueryOption_AwaitData;
    }
    if (findCmd.getAllowPartialResults()) {
        queryOptions = queryOptions | QueryOption_PartialResults;
    }
    if (isExhaust) {
        queryOptions = queryOptions | QueryOption_Exhaust;
    }
    return queryOptions;
}

}  // namespace

Message DBClientCursor::initFromLegacyRequest() {
    auto findCommand = fromLegacyQuery(_nsOrUuid,
                                       _filter,
                                       _querySettings,
                                       _fieldsToReturn ? *_fieldsToReturn : BSONObj(),
                                       _nToSkip,
                                       _opts);

    if (_limit) {
        findCommand->setLimit(_limit);
    }
    if (_batchSize) {
        findCommand->setBatchSize(_batchSize);
    }

    const BSONObj querySettings = _querySettings.getFullSettingsDeprecated();
    // We prioritize the readConcern parsed from the query object over '_readConcernObj'.
    if (!findCommand->getReadConcern()) {
        if (_readConcernObj) {
            findCommand->setReadConcern(_readConcernObj);
        } else {
            // If no readConcern was specified, initialize it to an empty readConcern object, ie.
            // equivalent to `readConcern: {}`. This ensures that mongos passes this empty
            // readConcern to shards.
            findCommand->setReadConcern(BSONObj());
        }
    }

    BSONObj cmd = findCommand->toBSON(BSONObj());
    if (auto readPref = querySettings["$readPreference"]) {
        // FindCommandRequest doesn't handle $readPreference.
        cmd = BSONObjBuilder(std::move(cmd)).append(readPref).obj();
    }

    return assembleCommandRequest(_client, _ns.db(), _opts, std::move(cmd));
}

Message DBClientCursor::assembleInit() {
    if (_cursorId) {
        return assembleGetMore();
    }

    // We haven't gotten a cursorId yet so we need to issue the initial find command.
    if (_findRequest) {
        // The caller described their find command using the modern 'FindCommandRequest' API.
        return assembleFromFindCommandRequest(_client, _ns.db(), *_findRequest, _readPref);
    } else {
        // The caller used a legacy API to describe the find operation, which may include $-prefixed
        // directives in the format previously expected for an OP_QUERY. We need to upconvert this
        // OP_QUERY-inspired format to a find command.
        return initFromLegacyRequest();
    }
}

Message DBClientCursor::assembleGetMore() {
    invariant(_cursorId);
    auto getMoreRequest = GetMoreCommandRequest(_cursorId, _ns.coll().toString());
    getMoreRequest.setBatchSize(
        boost::make_optional(_batchSize != 0, static_cast<int64_t>(_batchSize)));
    getMoreRequest.setMaxTimeMS(boost::make_optional(
        tailableAwaitData(),
        static_cast<std::int64_t>(durationCount<Milliseconds>(_awaitDataTimeout))));
    if (_term) {
        getMoreRequest.setTerm(static_cast<std::int64_t>(*_term));
    }
    getMoreRequest.setLastKnownCommittedOpTime(_lastKnownCommittedOpTime);
    auto msg = assembleCommandRequest(_client, _ns.db(), _opts, getMoreRequest.toBSON({}));

    // Set the exhaust flag if needed.
    if (_opts & QueryOption_Exhaust && msg.operation() == dbMsg) {
        OpMsg::setFlag(&msg, OpMsg::kExhaustSupported);
    }
    return msg;
}

bool DBClientCursor::init() {
    invariant(!_connectionHasPendingReplies);
    Message toSend = assembleInit();
    verify(_client);
    Message reply;
    try {
        _client->call(toSend, reply, true, &_originalHost);
    } catch (const DBException&) {
        // log msg temp?
        LOGV2(20127, "DBClientCursor::init call() failed");
        // We always want to throw on network exceptions.
        throw;
    }
    if (reply.empty()) {
        // log msg temp?
        LOGV2(20128, "DBClientCursor::init message from call() was empty");
        return false;
    }
    dataReceived(reply);
    return true;
}

void DBClientCursor::requestMore() {
    // For exhaust queries, once the stream has been initiated we get data blasted to us
    // from the remote server, without a need to send any more 'getMore' requests.
    const auto isExhaust = _opts & QueryOption_Exhaust;
    if (isExhaust && _connectionHasPendingReplies) {
        return exhaustReceiveMore();
    }

    invariant(!_connectionHasPendingReplies);
    verify(_cursorId && _batch.pos == _batch.objs.size());

    auto doRequestMore = [&] {
        Message toSend = assembleGetMore();
        Message response;
        _client->call(toSend, response);
        dataReceived(response);
    };
    if (_client)
        return doRequestMore();

    invariant(_scopedHost.size());
    DBClientBase::withConnection_do_not_use(_scopedHost, [&](DBClientBase* conn) {
        ON_BLOCK_EXIT([&, origClient = _client] { _client = origClient; });
        _client = conn;
        doRequestMore();
    });
}

/**
 * With QueryOption_Exhaust, the server just blasts data at us. The end of a stream is marked with a
 * cursor id of 0.
 */
void DBClientCursor::exhaustReceiveMore() {
    verify(_cursorId);
    verify(_batch.pos == _batch.objs.size());
    Message response;
    verify(_client);
    uassertStatusOK(
        _client->recv(response, _lastRequestId).withContext("recv failed while exhausting cursor"));
    dataReceived(response);
}

BSONObj DBClientCursor::commandDataReceived(const Message& reply) {
    int op = reply.operation();
    invariant(op == opReply || op == dbMsg);

    // Check if the reply indicates that it is part of an exhaust stream.
    const auto isExhaust = OpMsg::isFlagSet(reply, OpMsg::kMoreToCome);
    _connectionHasPendingReplies = isExhaust;
    if (isExhaust) {
        _lastRequestId = reply.header().getId();
    }

    auto commandReply = _client->parseCommandReplyMessage(_client->getServerAddress(), reply);
    auto commandStatus = getStatusFromCommandResult(commandReply->getCommandReply());

    if (commandStatus == ErrorCodes::StaleConfig) {
        uassertStatusOK(
            commandStatus.withContext("stale config in DBClientCursor::dataReceived()"));
    } else if (!commandStatus.isOK()) {
        _wasError = true;
    }

    return commandReply->getCommandReply().getOwned();
}

void DBClientCursor::dataReceived(const Message& reply, bool& retry, string& host) {
    _batch.objs.clear();
    _batch.pos = 0;

    const auto replyObj = commandDataReceived(reply);
    _cursorId = 0;  // Don't try to kill cursor if we get back an error.
    auto cr = uassertStatusOK(CursorResponse::parseFromBSON(replyObj));
    _cursorId = cr.getCursorId();
    uassert(50935,
            "Received a getMore response with a cursor id of 0 and the moreToCome flag set.",
            !(_connectionHasPendingReplies && _cursorId == 0));

    _ns = cr.getNSS();  // find command can change the ns to use for getMores.
    // Store the resume token, if we got one.
    _postBatchResumeToken = cr.getPostBatchResumeToken();
    _batch.objs = cr.releaseBatch();

    if (replyObj.hasField(LogicalTime::kOperationTimeFieldName)) {
        _operationTime = LogicalTime::fromOperationTime(replyObj).asTimestamp();
    }
}

/** If true, safe to call next().  Requests more from server if necessary. */
bool DBClientCursor::more() {
    if (!_putBack.empty())
        return true;

    if (_batch.pos < _batch.objs.size())
        return true;

    if (_cursorId == 0)
        return false;

    requestMore();
    return _batch.pos < _batch.objs.size();
}

BSONObj DBClientCursor::next() {
    if (!_putBack.empty()) {
        BSONObj ret = _putBack.top();
        _putBack.pop();
        return ret;
    }

    uassert(
        13422, "DBClientCursor next() called but more() is false", _batch.pos < _batch.objs.size());

    return std::move(_batch.objs[_batch.pos++]);
}

BSONObj DBClientCursor::nextSafe() {
    BSONObj o = next();

    // Only convert legacy errors ($err) to exceptions. Otherwise, just return the response and the
    // caller will interpret it as a command error.
    if (_wasError && strcmp(o.firstElementFieldName(), "$err") == 0) {
        uassertStatusOK(getStatusFromCommandResult(o));
    }

    return o;
}

void DBClientCursor::peek(vector<BSONObj>& v, int atMost) {
    auto end = atMost >= static_cast<int>(_batch.objs.size() - _batch.pos)
        ? _batch.objs.end()
        : _batch.objs.begin() + _batch.pos + atMost;
    v.insert(v.end(), _batch.objs.begin() + _batch.pos, end);
}

BSONObj DBClientCursor::peekFirst() {
    vector<BSONObj> v;
    peek(v, 1);

    if (v.size() > 0)
        return v[0];
    else
        return BSONObj();
}

bool DBClientCursor::peekError(BSONObj* error) {
    if (!_wasError)
        return false;

    vector<BSONObj> v;
    peek(v, 1);

    verify(v.size() == 1);
    // We check both the legacy error format, and the new error format. hasErrField checks for
    // $err, and getStatusFromCommandResult checks for modern errors of the form '{ok: 0.0, code:
    // <...>, errmsg: ...}'.
    verify(hasErrField(v[0]) || !getStatusFromCommandResult(v[0]).isOK());

    if (error)
        *error = v[0].getOwned();
    return true;
}

void DBClientCursor::attach(AScopedConnection* conn) {
    verify(_scopedHost.size() == 0);
    verify(conn);
    verify(conn->get());

    if (conn->get()->type() == ConnectionString::ConnectionType::kReplicaSet) {
        if (_client)
            _scopedHost = _client->getServerAddress();
        else
            massert(14821, "No client specified, cannot store multi-host connection.", false);
    } else {
        _scopedHost = conn->getHost();
    }

    conn->done();
    _client = nullptr;
}

DBClientCursor::DBClientCursor(DBClientBase* client,
                               const NamespaceStringOrUUID& nsOrUuid,
                               const BSONObj& filter,
                               const client_deprecated::Query& querySettings,
                               int limit,
                               int nToSkip,
                               const BSONObj* fieldsToReturn,
                               int queryOptions,
                               int batchSize,
                               boost::optional<BSONObj> readConcernObj)
    : DBClientCursor(client,
                     nsOrUuid,
                     filter,
                     querySettings,
                     0,  // cursorId
                     limit,
                     nToSkip,
                     fieldsToReturn,
                     queryOptions,
                     batchSize,
                     {},
                     readConcernObj,
                     boost::none) {}

DBClientCursor::DBClientCursor(DBClientBase* client,
                               const NamespaceStringOrUUID& nsOrUuid,
                               long long cursorId,
                               int limit,
                               int queryOptions,
                               std::vector<BSONObj> initialBatch,
                               boost::optional<Timestamp> operationTime,
                               boost::optional<BSONObj> postBatchResumeToken)
    : DBClientCursor(client,
                     nsOrUuid,
                     BSONObj(),  // filter
                     client_deprecated::Query(),
                     cursorId,
                     limit,
                     0,        // nToSkip
                     nullptr,  // fieldsToReturn
                     queryOptions,
                     0,
                     std::move(initialBatch),  // batchSize
                     boost::none,
                     operationTime,
                     postBatchResumeToken) {}

DBClientCursor::DBClientCursor(DBClientBase* client,
                               const NamespaceStringOrUUID& nsOrUuid,
                               const BSONObj& filter,
                               const client_deprecated::Query& querySettings,
                               long long cursorId,
                               int limit,
                               int nToSkip,
                               const BSONObj* fieldsToReturn,
                               int queryOptions,
                               int batchSize,
                               std::vector<BSONObj> initialBatch,
                               boost::optional<BSONObj> readConcernObj,
                               boost::optional<Timestamp> operationTime,
                               boost::optional<BSONObj> postBatchResumeToken)
    : _batch{std::move(initialBatch)},
      _client(client),
      _originalHost(_client->getServerAddress()),
      _nsOrUuid(nsOrUuid),
      _ns(nsOrUuid.nss() ? *nsOrUuid.nss() : NamespaceString(nsOrUuid.dbname())),
      _cursorId(cursorId),
      _batchSize(batchSize == 1 ? 2 : batchSize),
      _limit(limit),
      _filter(filter),
      _querySettings(querySettings),
      _nToSkip(nToSkip),
      _fieldsToReturn(fieldsToReturn),
      _readConcernObj(readConcernObj),
      _opts(queryOptions),
      _operationTime(operationTime),
      _postBatchResumeToken(postBatchResumeToken) {
    tassert(5746103, "DBClientCursor limit must be non-negative", _limit >= 0);
}

DBClientCursor::DBClientCursor(DBClientBase* client,
                               FindCommandRequest findRequest,
                               const ReadPreferenceSetting& readPref,
                               bool isExhaust)
    : _client(client),
      _originalHost(_client->getServerAddress()),
      _nsOrUuid(findRequest.getNamespaceOrUUID()),
      _ns(_nsOrUuid.nss() ? *_nsOrUuid.nss() : NamespaceString(_nsOrUuid.dbname())),
      _batchSize(findRequest.getBatchSize().value_or(0)),
      _limit(findRequest.getLimit().value_or(0)),
      _findRequest(std::move(findRequest)),
      _readPref(readPref),
      _opts(queryOptionsFromFindCommand(*_findRequest, _readPref, isExhaust)) {
    // Internal clients should always pass an explicit readConcern. If the caller did not already
    // pass a readConcern than we must explicitly initialize an empty readConcern so that it ends up
    // in the serialized version of the find command which will be sent across the wire.
    if (!_findRequest->getReadConcern()) {
        _findRequest->setReadConcern(BSONObj{});
    }
}

StatusWith<std::unique_ptr<DBClientCursor>> DBClientCursor::fromAggregationRequest(
    DBClientBase* client, AggregateCommandRequest aggRequest, bool secondaryOk, bool useExhaust) {
    BSONObj ret;
    try {
        if (!client->runCommand(aggRequest.getNamespace().db().toString(),
                                aggregation_request_helper::serializeToCommandObj(aggRequest),
                                ret,
                                secondaryOk ? QueryOption_SecondaryOk : 0)) {
            return getStatusFromCommandResult(ret);
        }
    } catch (...) {
        return exceptionToStatus();
    }
    long long cursorId = ret["cursor"].Obj()["id"].Long();
    std::vector<BSONObj> firstBatch;
    for (BSONElement elem : ret["cursor"].Obj()["firstBatch"].Array()) {
        firstBatch.emplace_back(elem.Obj().getOwned());
    }
    boost::optional<BSONObj> postBatchResumeToken;
    if (auto postBatchResumeTokenElem = ret["cursor"].Obj()["postBatchResumeToken"];
        postBatchResumeTokenElem.type() == BSONType::Object) {
        postBatchResumeToken = postBatchResumeTokenElem.Obj().getOwned();
    } else if (ret["cursor"].Obj().hasField("postBatchResumeToken")) {
        return Status(ErrorCodes::Error(5761702),
                      "Expected field 'postbatchResumeToken' to be of object type");
    }

    boost::optional<Timestamp> operationTime = boost::none;
    if (ret.hasField(LogicalTime::kOperationTimeFieldName)) {
        operationTime = LogicalTime::fromOperationTime(ret).asTimestamp();
    }

    return {std::make_unique<DBClientCursor>(client,
                                             aggRequest.getNamespace(),
                                             cursorId,
                                             0,
                                             useExhaust ? QueryOption_Exhaust : 0,
                                             firstBatch,
                                             operationTime,
                                             postBatchResumeToken)};
}

DBClientCursor::~DBClientCursor() {
    kill();
}

void DBClientCursor::kill() {
    DESTRUCTOR_GUARD({
        if (_cursorId && !globalInShutdownDeprecated()) {
            auto killCursor = [&](auto&& conn) { conn->killCursor(_ns, _cursorId); };

            // We only need to kill the cursor if there aren't pending replies. Pending replies
            // indicates that this is an exhaust cursor, so the connection must be closed and the
            // cursor will automatically be cleaned up by the upstream server.
            if (_client && !_connectionHasPendingReplies) {
                killCursor(_client);
            }
        }
    });

    // Mark this cursor as dead since we can't do any getMores.
    _cursorId = 0;
}


}  // namespace mongo
