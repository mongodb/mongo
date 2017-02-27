/**
 *    Copyright (C) 2010 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/commands/strategy.h"

#include "mongo/base/data_cursor.h"
#include "mongo/base/owned_pointer_vector.h"
#include "mongo/base/status.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/db/query/query_request.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/parallel.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/commands/cluster_explain.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/query/cluster_find.h"
#include "mongo/s/stale_exception.h"
#include "mongo/s/write_ops/batch_upconvert.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/timer.h"

namespace mongo {

using std::unique_ptr;
using std::shared_ptr;
using std::set;
using std::string;
using std::stringstream;

namespace {

void runAgainstRegistered(OperationContext* txn,
                          const char* ns,
                          BSONObj& jsobj,
                          BSONObjBuilder& anObjBuilder,
                          int queryOptions) {
    // It should be impossible for this uassert to fail since there should be no way to get
    // into this function with any other collection name.
    uassert(16618,
            "Illegal attempt to run a command against a namespace other than $cmd.",
            nsToCollectionSubstring(ns) == "$cmd");

    BSONElement e = jsobj.firstElement();
    std::string commandName = e.fieldName();
    Command* c = e.type() ? Command::findCommand(commandName) : NULL;
    if (!c) {
        Command::appendCommandStatus(
            anObjBuilder, false, str::stream() << "no such cmd: " << commandName);
        anObjBuilder.append("code", ErrorCodes::CommandNotFound);
        Command::unknownCommands.increment();
        return;
    }

    Command::execCommandClient(txn, c, queryOptions, ns, jsobj, anObjBuilder);
}

}  // namespace

void Strategy::queryOp(OperationContext* txn, const NamespaceString& nss, DbMessage* dbm) {
    globalOpCounters.gotQuery();

    const QueryMessage q(*dbm);

    Client* const client = txn->getClient();
    AuthorizationSession* const authSession = AuthorizationSession::get(client);

    Status status = authSession->checkAuthForFind(nss, false);
    audit::logQueryAuthzCheck(client, nss, q.query, status.code());
    uassertStatusOK(status);

    LOG(3) << "query: " << q.ns << " " << redact(q.query) << " ntoreturn: " << q.ntoreturn
           << " options: " << q.queryOptions;

    if (q.queryOptions & QueryOption_Exhaust) {
        uasserted(18526,
                  str::stream() << "The 'exhaust' query option is invalid for mongos queries: "
                                << nss.ns()
                                << " "
                                << q.query.toString());
    }

    // Determine the default read preference mode based on the value of the slaveOk flag.
    const ReadPreferenceSetting readPreference = [&]() {
        BSONElement rpElem;
        auto readPrefExtractStatus = bsonExtractTypedField(
            q.query, QueryRequest::kWrappedReadPrefField, mongo::Object, &rpElem);
        if (readPrefExtractStatus == ErrorCodes::NoSuchKey) {
            return ReadPreferenceSetting(q.queryOptions & QueryOption_SlaveOk
                                             ? ReadPreference::SecondaryPreferred
                                             : ReadPreference::PrimaryOnly);
        }

        uassertStatusOK(readPrefExtractStatus);

        return uassertStatusOK(ReadPreferenceSetting::fromBSON(rpElem.Obj()));
    }();

    auto canonicalQuery =
        uassertStatusOK(CanonicalQuery::canonicalize(txn, q, ExtensionsCallbackNoop()));

    // If the $explain flag was set, we must run the operation on the shards as an explain command
    // rather than a find command.
    const QueryRequest& queryRequest = canonicalQuery->getQueryRequest();
    if (queryRequest.isExplain()) {
        const BSONObj findCommand = queryRequest.asFindCommand();

        // We default to allPlansExecution verbosity.
        const auto verbosity = ExplainCommon::EXEC_ALL_PLANS;

        const bool secondaryOk = (readPreference.pref != ReadPreference::PrimaryOnly);
        const rpc::ServerSelectionMetadata metadata(secondaryOk, readPreference);

        BSONObjBuilder explainBuilder;
        uassertStatusOK(Strategy::explainFind(
            txn, findCommand, queryRequest, verbosity, metadata, &explainBuilder));

        BSONObj explainObj = explainBuilder.done();
        replyToQuery(0,  // query result flags
                     client->session(),
                     dbm->msg(),
                     static_cast<const void*>(explainObj.objdata()),
                     explainObj.objsize(),
                     1,  // numResults
                     0,  // startingFrom
                     CursorId(0));
        return;
    }

    // Do the work to generate the first batch of results. This blocks waiting to get responses from
    // the shard(s).
    std::vector<BSONObj> batch;

    // 0 means the cursor is exhausted. Otherwise we assume that a cursor with the returned id can
    // be retrieved via the ClusterCursorManager.
    auto cursorId =
        ClusterFind::runQuery(txn,
                              *canonicalQuery,
                              readPreference,
                              &batch,
                              nullptr /*Argument is for views which OP_QUERY doesn't support*/);

    if (!cursorId.isOK() &&
        cursorId.getStatus() == ErrorCodes::CommandOnShardedViewNotSupportedOnMongod) {
        uasserted(40247, "OP_QUERY not supported on views");
    }

    uassertStatusOK(cursorId.getStatus());

    // Fill out the response buffer.
    int numResults = 0;
    OpQueryReplyBuilder reply;
    for (auto&& obj : batch) {
        obj.appendSelfToBufBuilder(reply.bufBuilderForResults());
        numResults++;
    }

    reply.send(client->session(),
               0,  // query result flags
               dbm->msg(),
               numResults,
               0,  // startingFrom
               cursorId.getValue());
}

void Strategy::clientCommandOp(OperationContext* txn, const NamespaceString& nss, DbMessage* dbm) {
    const QueryMessage q(*dbm);

    Client* const client = txn->getClient();

    LOG(3) << "command: " << q.ns << " " << redact(q.query) << " ntoreturn: " << q.ntoreturn
           << " options: " << q.queryOptions;

    if (q.queryOptions & QueryOption_Exhaust) {
        uasserted(18527,
                  str::stream() << "The 'exhaust' query option is invalid for mongos commands: "
                                << nss.ns()
                                << " "
                                << q.query.toString());
    }

    uassert(16978,
            str::stream() << "Bad numberToReturn (" << q.ntoreturn
                          << ") for $cmd type ns - can only be 1 or -1",
            q.ntoreturn == 1 || q.ntoreturn == -1);

    // Handle the $cmd.sys pseudo-commands
    if (nss.isSpecialCommand()) {
        const auto upgradeToRealCommand = [&](StringData commandName) {
            BSONObjBuilder cmdBob;
            cmdBob.append(commandName, 1);
            cmdBob.appendElements(q.query);  // fields are validated by Commands
            auto interposedCmd = cmdBob.done();

            // Rewrite upgraded pseudoCommands to run on the 'admin' database.
            const NamespaceString interposedNss("admin", "$cmd");
            BSONObjBuilder reply;
            runAgainstRegistered(
                txn, interposedNss.ns().c_str(), interposedCmd, reply, q.queryOptions);
            replyToQuery(0, client->session(), dbm->msg(), reply.done());
        };

        if (nss.coll() == "$cmd.sys.inprog") {
            upgradeToRealCommand("currentOp");
            return;
        } else if (nss.coll() == "$cmd.sys.killop") {
            upgradeToRealCommand("killOp");
            return;
        } else if (nss.coll() == "$cmd.sys.unlock") {
            replyToQuery(0,
                         client->session(),
                         dbm->msg(),
                         BSON("err"
                              << "can't do unlock through mongos"));
            return;
        }

        // No pseudo-command found, fall through to execute as a regular query
    }

    BSONObj cmdObj = q.query;

    {
        BSONElement e = cmdObj.firstElement();
        if (e.type() == Object && (e.fieldName()[0] == '$' ? str::equals("query", e.fieldName() + 1)
                                                           : str::equals("query", e.fieldName()))) {
            // Extract the embedded query object.
            if (cmdObj.hasField(Query::ReadPrefField.name())) {
                // The command has a read preference setting. We don't want to lose this information
                // so we copy this to a new field called $queryOptions.$readPreference
                BSONObjBuilder finalCmdObjBuilder;
                finalCmdObjBuilder.appendElements(e.embeddedObject());

                BSONObjBuilder queryOptionsBuilder(finalCmdObjBuilder.subobjStart("$queryOptions"));
                queryOptionsBuilder.append(cmdObj[Query::ReadPrefField.name()]);
                queryOptionsBuilder.done();

                cmdObj = finalCmdObjBuilder.obj();
            } else {
                cmdObj = e.embeddedObject();
            }
        }
    }

    // Handle command option maxTimeMS.
    uassert(ErrorCodes::InvalidOptions,
            "no such command option $maxTimeMs; use maxTimeMS instead",
            cmdObj[QueryRequest::queryOptionMaxTimeMS].eoo());

    const int maxTimeMS =
        uassertStatusOK(QueryRequest::parseMaxTimeMS(cmdObj[QueryRequest::cmdOptionMaxTimeMS]));
    if (maxTimeMS > 0) {
        txn->setDeadlineAfterNowBy(Milliseconds{maxTimeMS});
    }

    int loops = 5;

    while (true) {
        try {
            OpQueryReplyBuilder reply;
            {
                BSONObjBuilder builder(reply.bufBuilderForResults());
                runAgainstRegistered(txn, q.ns, cmdObj, builder, q.queryOptions);
            }
            reply.sendCommandReply(client->session(), dbm->msg());
            return;
        } catch (const StaleConfigException& e) {
            if (loops <= 0)
                throw e;

            loops--;

            log() << "Retrying command " << redact(q.query) << causedBy(e);

            // For legacy reasons, ns may not actually be set in the exception
            const std::string staleNS(e.getns().empty() ? std::string(q.ns) : e.getns());

            ShardConnection::checkMyConnectionVersions(txn, staleNS);
            if (loops < 4) {
                // This throws out the entire database cache entry in response to
                // StaleConfigException instead of just the collection which encountered it. There
                // is no good reason for it other than the lack of lower-granularity cache
                // invalidation.
                Grid::get(txn)->catalogCache()->invalidate(NamespaceString(staleNS).db());
            }
        } catch (const DBException& e) {
            OpQueryReplyBuilder reply;
            {
                BSONObjBuilder builder(reply.bufBuilderForResults());
                Command::appendCommandStatus(builder, e.toStatus());
            }
            reply.sendCommandReply(client->session(), dbm->msg());
            return;
        }
    }
}

void Strategy::commandOp(OperationContext* txn,
                         const string& db,
                         const BSONObj& command,
                         int options,
                         const string& versionedNS,
                         const BSONObj& targetingQuery,
                         const BSONObj& targetingCollation,
                         std::vector<CommandResult>* results) {
    QuerySpec qSpec(db + ".$cmd", command, BSONObj(), 0, 1, options);

    ParallelSortClusteredCursor cursor(
        qSpec, CommandInfo(versionedNS, targetingQuery, targetingCollation));

    // Initialize the cursor
    cursor.init(txn);

    set<ShardId> shardIds;
    cursor.getQueryShardIds(shardIds);

    for (const ShardId& shardId : shardIds) {
        CommandResult result;
        result.shardTargetId = shardId;

        result.target = fassertStatusOK(
            34417, ConnectionString::parse(cursor.getShardCursor(shardId)->originalHost()));
        result.result = cursor.getShardCursor(shardId)->peekFirst().getOwned();
        results->push_back(result);
    }
}

void Strategy::getMore(OperationContext* txn, const NamespaceString& nss, DbMessage* dbm) {
    const int ntoreturn = dbm->pullInt();
    uassert(
        34424, str::stream() << "Invalid ntoreturn for OP_GET_MORE: " << ntoreturn, ntoreturn >= 0);
    const long long cursorId = dbm->pullInt64();

    globalOpCounters.gotGetMore();

    Client* const client = txn->getClient();

    // TODO: Handle stale config exceptions here from coll being dropped or sharded during op for
    // now has same semantics as legacy request.

    auto statusGetDb = Grid::get(txn)->catalogCache()->getDatabase(txn, nss.db());
    if (statusGetDb == ErrorCodes::NamespaceNotFound) {
        replyToQuery(ResultFlag_CursorNotFound, client->session(), dbm->msg(), 0, 0, 0);
        return;
    }
    uassertStatusOK(statusGetDb);

    boost::optional<long long> batchSize;
    if (ntoreturn) {
        batchSize = ntoreturn;
    }

    GetMoreRequest getMoreRequest(nss, cursorId, batchSize, boost::none, boost::none, boost::none);

    auto cursorResponse = ClusterFind::runGetMore(txn, getMoreRequest);
    if (cursorResponse == ErrorCodes::CursorNotFound) {
        replyToQuery(ResultFlag_CursorNotFound, client->session(), dbm->msg(), 0, 0, 0);
        return;
    }
    uassertStatusOK(cursorResponse.getStatus());

    // Build the response document.
    BufBuilder buffer(FindCommon::kInitReplyBufferSize);

    int numResults = 0;
    for (const auto& obj : cursorResponse.getValue().getBatch()) {
        buffer.appendBuf((void*)obj.objdata(), obj.objsize());
        ++numResults;
    }

    replyToQuery(0,
                 client->session(),
                 dbm->msg(),
                 buffer.buf(),
                 buffer.len(),
                 numResults,
                 cursorResponse.getValue().getNumReturnedSoFar().value_or(0),
                 cursorResponse.getValue().getCursorId());
}

void Strategy::killCursors(OperationContext* txn, DbMessage* dbm) {
    const int numCursors = dbm->pullInt();
    massert(34425,
            str::stream() << "Invalid killCursors message. numCursors: " << numCursors
                          << ", message size: "
                          << dbm->msg().dataSize()
                          << ".",
            dbm->msg().dataSize() == 8 + (8 * numCursors));
    uassert(28794,
            str::stream() << "numCursors must be between 1 and 29999.  numCursors: " << numCursors
                          << ".",
            numCursors >= 1 && numCursors < 30000);

    globalOpCounters.gotOp(dbKillCursors, false);

    ConstDataCursor cursors(dbm->getArray(numCursors));

    Client* const client = txn->getClient();
    AuthorizationSession* const authSession = AuthorizationSession::get(client);
    ClusterCursorManager* const manager = Grid::get(txn)->getCursorManager();

    for (int i = 0; i < numCursors; ++i) {
        const CursorId cursorId = cursors.readAndAdvance<LittleEndian<int64_t>>();

        boost::optional<NamespaceString> nss = manager->getNamespaceForCursorId(cursorId);
        if (!nss) {
            LOG(3) << "Can't find cursor to kill.  Cursor id: " << cursorId << ".";
            continue;
        }

        Status authorizationStatus = authSession->checkAuthForKillCursors(*nss, cursorId);
        audit::logKillCursorsAuthzCheck(client,
                                        *nss,
                                        cursorId,
                                        authorizationStatus.isOK() ? ErrorCodes::OK
                                                                   : ErrorCodes::Unauthorized);
        if (!authorizationStatus.isOK()) {
            LOG(3) << "Not authorized to kill cursor.  Namespace: '" << *nss
                   << "', cursor id: " << cursorId << ".";
            continue;
        }

        Status killCursorStatus = manager->killCursor(*nss, cursorId);
        if (!killCursorStatus.isOK()) {
            LOG(3) << "Can't find cursor to kill.  Namespace: '" << *nss
                   << "', cursor id: " << cursorId << ".";
            continue;
        }

        LOG(3) << "Killed cursor.  Namespace: '" << *nss << "', cursor id: " << cursorId << ".";
    }
}

void Strategy::writeOp(OperationContext* txn, DbMessage* dbm) {
    OwnedPointerVector<BatchedCommandRequest> commandRequestsOwned;
    std::vector<BatchedCommandRequest*>& commandRequests = commandRequestsOwned.mutableVector();

    msgToBatchRequests(dbm->msg(), &commandRequests);

    auto& clientLastError = LastError::get(txn->getClient());

    for (auto it = commandRequests.begin(); it != commandRequests.end(); ++it) {
        // Multiple commands registered to last error as multiple requests
        if (it != commandRequests.begin()) {
            clientLastError.startRequest();
        }

        BatchedCommandRequest* const commandRequest = *it;

        BatchedCommandResponse commandResponse;

        {
            // Disable the last error object for the duration of the write cmd
            LastError::Disabled disableLastError(&clientLastError);

            // Adjust namespace for command
            const NamespaceString& fullNS(commandRequest->getNS());
            const std::string cmdNS = fullNS.getCommandNS();

            BSONObj commandBSON = commandRequest->toBSON();

            BSONObjBuilder builder;
            runAgainstRegistered(txn, cmdNS.c_str(), commandBSON, builder, 0);

            bool parsed = commandResponse.parseBSON(builder.done(), nullptr);
            (void)parsed;  // for compile
            dassert(parsed && commandResponse.isValid(nullptr));
        }

        // Populate the lastError object based on the write response
        clientLastError.reset();

        const bool hadError =
            batchErrorToLastError(*commandRequest, commandResponse, &clientLastError);

        // Check if this is an ordered batch and we had an error which should stop processing
        if (commandRequest->getOrdered() && hadError) {
            break;
        }
    }
}

Status Strategy::explainFind(OperationContext* txn,
                             const BSONObj& findCommand,
                             const QueryRequest& qr,
                             ExplainCommon::Verbosity verbosity,
                             const rpc::ServerSelectionMetadata& serverSelectionMetadata,
                             BSONObjBuilder* out) {
    BSONObjBuilder explainCmdBob;
    int options = 0;
    ClusterExplain::wrapAsExplain(
        findCommand, verbosity, serverSelectionMetadata, &explainCmdBob, &options);

    // We will time how long it takes to run the commands on the shards.
    Timer timer;

    std::vector<Strategy::CommandResult> shardResults;
    Strategy::commandOp(txn,
                        qr.nss().db().toString(),
                        explainCmdBob.obj(),
                        options,
                        qr.nss().toString(),
                        qr.getFilter(),
                        qr.getCollation(),
                        &shardResults);

    long long millisElapsed = timer.millis();

    if (shardResults.size() == 1 &&
        ResolvedView::isResolvedViewErrorResponse(shardResults[0].result)) {
        out->append("resolvedView", shardResults[0].result.getObjectField("resolvedView"));
        return getStatusFromCommandResult(shardResults[0].result);
    }

    const char* mongosStageName = ClusterExplain::getStageNameForReadOp(shardResults, findCommand);

    return ClusterExplain::buildExplainResult(
        txn, shardResults, mongosStageName, millisElapsed, out);
}

}  // namespace mongo
