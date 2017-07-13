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

#include "mongo/client/dbclientcursor.h"

#include "mongo/client/connpool.h"
#include "mongo/db/client.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/db/query/query_request.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata.h"
#include "mongo/rpc/object_check.h"
#include "mongo/s/stale_exception.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/destructor_guard.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using std::unique_ptr;
using std::endl;
using std::string;
using std::vector;

namespace {
Message assembleCommandRequest(DBClientBase* cli,
                               StringData database,
                               int legacyQueryOptions,
                               BSONObj legacyQuery) {
    auto request = rpc::upconvertRequest(database, std::move(legacyQuery), legacyQueryOptions);

    if (cli->getRequestMetadataWriter()) {
        BSONObjBuilder bodyBob(std::move(request.body));
        auto opCtx = (haveClient() ? cc().getOperationContext() : nullptr);
        uassertStatusOK(cli->getRequestMetadataWriter()(opCtx, &bodyBob));
        request.body = bodyBob.obj();
    }

    return rpc::messageFromOpMsgRequest(
        cli->getClientRPCProtocols(), cli->getServerRPCProtocols(), std::move(request));
}

}  // namespace

int DBClientCursor::nextBatchSize() {
    if (nToReturn == 0)
        return batchSize;

    if (batchSize == 0)
        return nToReturn;

    return batchSize < nToReturn ? batchSize : nToReturn;
}

Message DBClientCursor::_assembleInit() {
    if (cursorId) {
        return _assembleGetMore();
    }

    // If we haven't gotten a cursorId yet, we need to issue a new query or command.
    if (_isCommand) {
        // HACK:
        // Unfortunately, this code is used by the shell to run commands,
        // so we need to allow the shell to send invalid options so that we can
        // test that the server rejects them. Thus, to allow generating commands with
        // invalid options, we validate them here, and fall back to generating an OP_QUERY
        // through assembleQueryRequest if the options are invalid.
        bool hasValidNToReturnForCommand = (nToReturn == 1 || nToReturn == -1);
        bool hasValidFlagsForCommand = !(opts & mongo::QueryOption_Exhaust);
        bool hasInvalidMaxTimeMs = query.hasField("$maxTimeMS");

        if (hasValidNToReturnForCommand && hasValidFlagsForCommand && !hasInvalidMaxTimeMs) {
            return assembleCommandRequest(_client, ns.db(), opts, query);
        }
    } else if (_useFindCommand) {
        auto qr = QueryRequest::fromLegacyQuery(ns,
                                                query,
                                                fieldsToReturn ? *fieldsToReturn : BSONObj(),
                                                nToSkip,
                                                nextBatchSize(),
                                                opts);
        if (qr.isOK() && !qr.getValue()->isExplain() && !qr.getValue()->isExhaust()) {
            BSONObj cmd = qr.getValue()->asFindCommand();
            if (auto readPref = query["$readPreference"]) {
                // QueryRequest doesn't handle $readPreference.
                cmd = BSONObjBuilder(std::move(cmd)).append(readPref).obj();
            }
            return assembleCommandRequest(_client, ns.db(), opts, std::move(cmd));
        }
        // else use legacy OP_QUERY request.
    }

    _useFindCommand = false;  // Make sure we handle the reply correctly.
    Message toSend;
    assembleQueryRequest(ns.ns(), query, nextBatchSize(), nToSkip, fieldsToReturn, opts, toSend);
    return toSend;
}

Message DBClientCursor::_assembleGetMore() {
    invariant(cursorId);
    if (_useFindCommand) {
        long long batchSize = nextBatchSize();
        auto gmr = GetMoreRequest(ns,
                                  cursorId,
                                  boost::make_optional(batchSize != 0, batchSize),
                                  boost::none,   // awaitDataTimeout
                                  boost::none,   // term
                                  boost::none);  // lastKnownCommittedOptime
        return assembleCommandRequest(_client, ns.db(), opts, gmr.toBSON());
    } else {
        // Assemble a legacy getMore request.
        return makeGetMoreMessage(ns.ns(), cursorId, nextBatchSize(), opts);
    }
}

bool DBClientCursor::init() {
    invariant(!_connectionHasPendingReplies);
    Message toSend = _assembleInit();
    verify(_client);
    Message reply;
    if (!_client->call(toSend, reply, false, &_originalHost)) {
        // log msg temp?
        log() << "DBClientCursor::init call() failed" << endl;
        return false;
    }
    if (reply.empty()) {
        // log msg temp?
        log() << "DBClientCursor::init message from call() was empty" << endl;
        return false;
    }
    dataReceived(reply);
    return true;
}

void DBClientCursor::initLazy(bool isRetry) {
    massert(15875,
            "DBClientCursor::initLazy called on a client that doesn't support lazy",
            _client->lazySupported());
    Message toSend = _assembleInit();
    _client->say(toSend, isRetry, &_originalHost);
    _lastRequestId = toSend.header().getId();
    _connectionHasPendingReplies = true;
}

bool DBClientCursor::initLazyFinish(bool& retry) {
    invariant(_connectionHasPendingReplies);
    Message reply;
    bool recvd = _client->recv(reply, _lastRequestId);
    _connectionHasPendingReplies = false;

    // If we get a bad response, return false
    if (!recvd || reply.empty()) {
        if (!recvd)
            log() << "DBClientCursor::init lazy say() failed" << endl;
        if (reply.empty())
            log() << "DBClientCursor::init message from say() was empty" << endl;

        _client->checkResponse({}, true, &retry, &_lazyHost);

        return false;
    }

    dataReceived(reply, retry, _lazyHost);

    return !retry;
}

void DBClientCursor::requestMore() {
    invariant(!_connectionHasPendingReplies);
    verify(cursorId && batch.pos == batch.objs.size());

    if (haveLimit) {
        nToReturn -= batch.objs.size();
        verify(nToReturn > 0);
    }

    ON_BLOCK_EXIT([ this, origClient = _client ] { _client = origClient; });
    boost::optional<ScopedDbConnection> connHolder;
    if (!_client) {
        invariant(_scopedHost.size());
        connHolder.emplace(_scopedHost);
        _client = connHolder->get();
    }

    Message toSend = _assembleGetMore();
    Message response;
    _client->call(toSend, response);

    // If call() succeeds, the connection is clean so we can return it to the pool, even if
    // dataReceived() throws because the command reported failure. However, we can't return it yet,
    // because dataReceived() needs to get the metadata reader from the connection.
    ON_BLOCK_EXIT([&] {
        if (connHolder)
            connHolder->done();
    });

    dataReceived(response);
}

/** with QueryOption_Exhaust, the server just blasts data at us (marked at end with cursorid==0). */
void DBClientCursor::exhaustReceiveMore() {
    verify(cursorId && batch.pos == batch.objs.size());
    verify(!haveLimit);
    Message response;
    verify(_client);
    if (!_client->recv(response, _lastRequestId)) {
        uasserted(16465, "recv failed while exhausting cursor");
    }
    dataReceived(response);
}

BSONObj DBClientCursor::commandDataReceived(const Message& reply) {
    int op = reply.operation();
    invariant(op == opReply || op == dbCommandReply || op == dbMsg);

    auto commandReply = rpc::makeReply(&reply);
    auto commandStatus = getStatusFromCommandResult(commandReply->getCommandReply());

    if (ErrorCodes::SendStaleConfig == commandStatus) {
        throw RecvStaleConfigException("stale config in DBClientCursor::dataReceived()",
                                       commandReply->getCommandReply());
    } else if (!commandStatus.isOK()) {
        wasError = true;
    }

    auto opCtx = haveClient() ? cc().getOperationContext() : nullptr;
    if (_client->getReplyMetadataReader()) {
        uassertStatusOK(_client->getReplyMetadataReader()(
            opCtx, commandReply->getMetadata(), _client->getServerAddress()));
    }

    return commandReply->getCommandReply().getOwned();
}

void DBClientCursor::dataReceived(const Message& reply, bool& retry, string& host) {
    batch.objs.clear();
    batch.pos = 0;

    // If this is a reply to our initial command request.
    if (_isCommand && cursorId == 0) {
        batch.objs.push_back(commandDataReceived(reply));
        return;
    }

    if (_useFindCommand) {
        cursorId = 0;  // Don't try to kill cursor if we get back an error.
        auto cr = uassertStatusOK(CursorResponse::parseFromBSON(commandDataReceived(reply)));
        cursorId = cr.getCursorId();
        ns = cr.getNSS();  // Unlike OP_REPLY, find command can change the ns to use for getMores.
        batch.objs = cr.releaseBatch();
        return;
    }

    QueryResult::View qr = reply.singleData().view2ptr();
    resultFlags = qr.getResultFlags();

    if (qr.getResultFlags() & ResultFlag_ErrSet) {
        wasError = true;
    }

    if (qr.getResultFlags() & ResultFlag_CursorNotFound) {
        // cursor id no longer valid at the server.
        invariant(qr.getCursorId() == 0);

        if (!(opts & QueryOption_CursorTailable)) {
            uasserted(13127,
                      str::stream() << "cursor id " << cursorId << " didn't exist on server.");
        }

        // 0 indicates no longer valid (dead)
        cursorId = 0;
    }

    if (cursorId == 0 || !(opts & QueryOption_CursorTailable)) {
        // only set initially: we don't want to kill it on end of data
        // if it's a tailable cursor
        cursorId = qr.getCursorId();
    }

    if (opts & QueryOption_Exhaust) {
        // With exhaust mode, each reply after the first claims to be a reply to the previous one
        // rather than the initial request.
        _connectionHasPendingReplies = (cursorId != 0);
        _lastRequestId = reply.header().getId();
    }

    batch.objs.reserve(qr.getNReturned());

    BufReader data(qr.data(), qr.dataLen());
    while (static_cast<int>(batch.objs.size()) < qr.getNReturned()) {
        if (serverGlobalParams.objcheck) {
            batch.objs.push_back(data.read<Validated<BSONObj>>());
        } else {
            batch.objs.push_back(data.read<BSONObj>());
        }
        batch.objs.back().shareOwnershipWith(reply.sharedBuffer());
    }
    uassert(ErrorCodes::InvalidBSON,
            "Got invalid reply from external server while reading from cursor",
            data.atEof());

    _client->checkResponse(batch.objs, false, &retry, &host);  // watches for "not master"

    if (qr.getResultFlags() & ResultFlag_ShardConfigStale) {
        BSONObj error;
        verify(peekError(&error));
        throw RecvStaleConfigException(
            (string) "stale config on lazy receive" + causedBy(getErrField(error)), error);
    }

    /* this assert would fire the way we currently work:
        verify( nReturned || cursorId == 0 );
    */
}

/** If true, safe to call next().  Requests more from server if necessary. */
bool DBClientCursor::more() {
    if (!_putBack.empty())
        return true;

    if (haveLimit && static_cast<int>(batch.pos) >= nToReturn)
        return false;

    if (batch.pos < batch.objs.size())
        return true;

    if (cursorId == 0)
        return false;

    requestMore();
    return batch.pos < batch.objs.size();
}

BSONObj DBClientCursor::next() {
    if (!_putBack.empty()) {
        BSONObj ret = _putBack.top();
        _putBack.pop();
        return ret;
    }

    uassert(
        13422, "DBClientCursor next() called but more() is false", batch.pos < batch.objs.size());

    /* todo would be good to make data null at end of batch for safety */
    return std::move(batch.objs[batch.pos++]);
}

BSONObj DBClientCursor::nextSafe() {
    BSONObj o = next();

    // Only convert legacy errors ($err) to exceptions. Otherwise, just return the response and the
    // caller will interpret it as a command error.
    if (wasError && strcmp(o.firstElementFieldName(), "$err") == 0) {
        auto code = o["code"].numberInt();
        if (!code) {
            code = ErrorCodes::UnknownError;
        }
        uasserted(code, o.firstElement().str());
    }

    return o;
}

void DBClientCursor::peek(vector<BSONObj>& v, int atMost) {
    auto end = atMost >= static_cast<int>(batch.objs.size() - batch.pos)
        ? batch.objs.end()
        : batch.objs.begin() + batch.pos + atMost;
    v.insert(v.end(), batch.objs.begin() + batch.pos, end);
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
    if (!wasError)
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

    if (conn->get()->type() == ConnectionString::SET) {
        if (_lazyHost.size() > 0)
            _scopedHost = _lazyHost;
        else if (_client)
            _scopedHost = _client->getServerAddress();
        else
            massert(14821,
                    "No client or lazy client specified, cannot store multi-host connection.",
                    false);
    } else {
        _scopedHost = conn->getHost();
    }

    conn->done();
    _client = 0;
    _lazyHost = "";
}

DBClientCursor::DBClientCursor(DBClientBase* client,
                               const std::string& ns,
                               const BSONObj& query,
                               int nToReturn,
                               int nToSkip,
                               const BSONObj* fieldsToReturn,
                               int queryOptions,
                               int batchSize)
    : DBClientCursor(client,
                     ns,
                     query,
                     0,  // cursorId
                     nToReturn,
                     nToSkip,
                     fieldsToReturn,
                     queryOptions,
                     batchSize) {}

DBClientCursor::DBClientCursor(DBClientBase* client,
                               const std::string& ns,
                               long long cursorId,
                               int nToReturn,
                               int queryOptions)
    : DBClientCursor(client,
                     ns,
                     BSONObj(),  // query
                     cursorId,
                     nToReturn,
                     0,        // nToSkip
                     nullptr,  // fieldsToReturn
                     queryOptions,
                     0) {}  // batchSize

DBClientCursor::DBClientCursor(DBClientBase* client,
                               const std::string& ns,
                               const BSONObj& query,
                               long long cursorId,
                               int nToReturn,
                               int nToSkip,
                               const BSONObj* fieldsToReturn,
                               int queryOptions,
                               int batchSize)
    : _client(client),
      _originalHost(_client->getServerAddress()),
      ns(ns),
      _isCommand(nsIsFull(ns) ? nsToCollectionSubstring(ns) == "$cmd" : false),
      query(query),
      nToReturn(nToReturn),
      haveLimit(nToReturn > 0 && !(queryOptions & QueryOption_CursorTailable)),
      nToSkip(nToSkip),
      fieldsToReturn(fieldsToReturn),
      opts(queryOptions & ~QueryOptionLocal_forceOpQuery),
      batchSize(batchSize == 1 ? 2 : batchSize),
      resultFlags(0),
      cursorId(cursorId),
      _ownCursor(true),
      wasError(false),
      _enabledBSONVersion(Validator<BSONObj>::enabledBSONVersion()) {
    if (queryOptions & QueryOptionLocal_forceOpQuery)
        _useFindCommand = false;
}

DBClientCursor::~DBClientCursor() {
    kill();
}

void DBClientCursor::kill() {
    DESTRUCTOR_GUARD({
        if (cursorId && _ownCursor && !globalInShutdownDeprecated()) {
            auto killCursor = [&](auto& conn) {
                if (_useFindCommand) {
                    conn->killCursor(ns, cursorId);
                } else {
                    auto toSend = makeKillCursorsMessage(cursorId);
                    conn->say(toSend);
                }
            };

            if (_client && !_connectionHasPendingReplies) {
                killCursor(_client);
            } else {
                // Use a side connection to send the kill cursor request.
                verify(_scopedHost.size() || (_client && _connectionHasPendingReplies));
                ScopedDbConnection conn(_client ? _client->getServerAddress() : _scopedHost);
                killCursor(conn);
                conn.done();
            }
        }
    });

    // Mark this cursor as dead since we can't do any getMores.
    cursorId = 0;
}


}  // namespace mongo
