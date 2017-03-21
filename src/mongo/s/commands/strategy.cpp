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
#include "mongo/base/init.h"
#include "mongo/base/owned_pointer_vector.h"
#include "mongo/base/status.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_time_tracker.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/db/query/query_request.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/logical_time_metadata.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/rpc/metadata/tracking_metadata.h"
#include "mongo/s/catalog_cache.h"
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
#include "mongo/util/scopeguard.h"
#include "mongo/util/timer.h"

namespace mongo {

using std::unique_ptr;
using std::shared_ptr;
using std::set;
using std::string;
using std::stringstream;

namespace {

const std::string kOperationTime = "operationTime";

void runAgainstRegistered(OperationContext* opCtx,
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

    execCommandClient(opCtx, c, queryOptions, ns, jsobj, anObjBuilder);
}

/**
 * Called into by the web server. For now we just translate the parameters to their old style
 * equivalents.
 */
void execCommandHandler(OperationContext* opCtx,
                        Command* command,
                        const rpc::RequestInterface& request,
                        rpc::ReplyBuilderInterface* replyBuilder) {
    int queryFlags = 0;
    BSONObj cmdObj;

    std::tie(cmdObj, queryFlags) = uassertStatusOK(
        rpc::downconvertRequestMetadata(request.getCommandArgs(), request.getMetadata()));

    std::string db = request.getDatabase().rawData();
    BSONObjBuilder result;

    execCommandClient(opCtx, command, queryFlags, request.getDatabase().rawData(), cmdObj, result);

    replyBuilder->setCommandReply(result.done()).setMetadata(rpc::makeEmptyMetadata());
}

/**
 * Extract and process metadata from the command request body.
 */
Status processCommandMetadata(OperationContext* opCtx, const BSONObj& cmdObj) {
    auto logicalClock = LogicalClock::get(opCtx);
    invariant(logicalClock);

    auto logicalTimeMetadata = rpc::LogicalTimeMetadata::readFromMetadata(cmdObj);
    if (!logicalTimeMetadata.isOK()) {
        return logicalTimeMetadata.getStatus();
    }

    auto authSession = AuthorizationSession::get(opCtx->getClient());
    if (authSession->getAuthorizationManager().isAuthEnabled()) {
        auto advanceClockStatus =
            logicalClock->advanceClusterTime(logicalTimeMetadata.getValue().getSignedTime());

        if (!advanceClockStatus.isOK()) {
            return advanceClockStatus;
        }
    } else {
        auto advanceClockStatus = logicalClock->advanceClusterTimeFromTrustedSource(
            logicalTimeMetadata.getValue().getSignedTime());

        if (!advanceClockStatus.isOK()) {
            return advanceClockStatus;
        }
    }

    return Status::OK();
}

/**
 * Append required fields to command response.
 */
void appendRequiredFieldsToResponse(OperationContext* opCtx, BSONObjBuilder* responseBuilder) {
    rpc::LogicalTimeMetadata logicalTimeMetadata(LogicalClock::get(opCtx)->getClusterTime());
    logicalTimeMetadata.writeToMetadata(responseBuilder);
    auto tracker = OperationTimeTracker::get(opCtx);
    if (tracker) {
        auto operationTime = OperationTimeTracker::get(opCtx)->getMaxOperationTime();
        responseBuilder->append(kOperationTime, operationTime.asTimestamp());
    }
}

MONGO_INITIALIZER(InitializeCommandExecCommandHandler)(InitializerContext* const) {
    Command::registerExecCommand(execCommandHandler);
    return Status::OK();
}

void registerErrorImpl(OperationContext* opCtx, const DBException& exception) {}

MONGO_INITIALIZER(InitializeRegisterErrorHandler)(InitializerContext* const) {
    Command::registerRegisterError(registerErrorImpl);
    return Status::OK();
}

}  // namespace

void Strategy::queryOp(OperationContext* opCtx, const NamespaceString& nss, DbMessage* dbm) {
    globalOpCounters.gotQuery();

    const QueryMessage q(*dbm);

    Client* const client = opCtx->getClient();
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
        uassertStatusOK(CanonicalQuery::canonicalize(opCtx, q, ExtensionsCallbackNoop()));

    // If the $explain flag was set, we must run the operation on the shards as an explain command
    // rather than a find command.
    const QueryRequest& queryRequest = canonicalQuery->getQueryRequest();
    if (queryRequest.isExplain()) {
        const BSONObj findCommand = queryRequest.asFindCommand();

        // We default to allPlansExecution verbosity.
        const auto verbosity = ExplainOptions::Verbosity::kExecAllPlans;

        const bool secondaryOk = (readPreference.pref != ReadPreference::PrimaryOnly);
        const rpc::ServerSelectionMetadata metadata(secondaryOk, readPreference);

        BSONObjBuilder explainBuilder;
        uassertStatusOK(Strategy::explainFind(
            opCtx, findCommand, queryRequest, verbosity, metadata, &explainBuilder));

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
        ClusterFind::runQuery(opCtx,
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

void Strategy::clientCommandOp(OperationContext* opCtx,
                               const NamespaceString& nss,
                               DbMessage* dbm) {
    const QueryMessage q(*dbm);

    Client* const client = opCtx->getClient();

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
                opCtx, interposedNss.ns().c_str(), interposedCmd, reply, q.queryOptions);
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
        opCtx->setDeadlineAfterNowBy(Milliseconds{maxTimeMS});
    }

    int loops = 5;

    while (true) {
        try {
            OpQueryReplyBuilder reply;
            {
                BSONObjBuilder builder(reply.bufBuilderForResults());
                runAgainstRegistered(opCtx, q.ns, cmdObj, builder, q.queryOptions);
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

            ShardConnection::checkMyConnectionVersions(opCtx, staleNS);
            if (loops < 4) {
                const NamespaceString nss(staleNS);
                if (nss.isValid()) {
                    Grid::get(opCtx)->catalogCache()->invalidateShardedCollection(nss);
                }
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

void Strategy::commandOp(OperationContext* opCtx,
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
    cursor.init(opCtx);

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

void Strategy::getMore(OperationContext* opCtx, const NamespaceString& nss, DbMessage* dbm) {
    const int ntoreturn = dbm->pullInt();
    uassert(
        34424, str::stream() << "Invalid ntoreturn for OP_GET_MORE: " << ntoreturn, ntoreturn >= 0);
    const long long cursorId = dbm->pullInt64();

    globalOpCounters.gotGetMore();

    Client* const client = opCtx->getClient();

    // TODO: Handle stale config exceptions here from coll being dropped or sharded during op for
    // now has same semantics as legacy request.

    auto statusGetDb = Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, nss.db());
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

    auto cursorResponse = ClusterFind::runGetMore(opCtx, getMoreRequest);
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

void Strategy::killCursors(OperationContext* opCtx, DbMessage* dbm) {
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

    Client* const client = opCtx->getClient();
    AuthorizationSession* const authSession = AuthorizationSession::get(client);
    ClusterCursorManager* const manager = Grid::get(opCtx)->getCursorManager();

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

void Strategy::writeOp(OperationContext* opCtx, DbMessage* dbm) {
    std::vector<std::unique_ptr<BatchedCommandRequest>> commandRequests;

    msgToBatchRequests(dbm->msg(), &commandRequests);

    auto& clientLastError = LastError::get(opCtx->getClient());

    for (auto it = commandRequests.begin(); it != commandRequests.end(); ++it) {
        // Multiple commands registered to last error as multiple requests
        if (it != commandRequests.begin()) {
            clientLastError.startRequest();
        }

        BatchedCommandRequest* const commandRequest = it->get();

        BatchedCommandResponse commandResponse;

        {
            // Disable the last error object for the duration of the write cmd
            LastError::Disabled disableLastError(&clientLastError);

            // Adjust namespace for command
            const NamespaceString& fullNS(commandRequest->getNS());
            const std::string cmdNS = fullNS.getCommandNS();

            BSONObj commandBSON = commandRequest->toBSON();

            BSONObjBuilder builder;
            runAgainstRegistered(opCtx, cmdNS.c_str(), commandBSON, builder, 0);

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

Status Strategy::explainFind(OperationContext* opCtx,
                             const BSONObj& findCommand,
                             const QueryRequest& qr,
                             ExplainOptions::Verbosity verbosity,
                             const rpc::ServerSelectionMetadata& serverSelectionMetadata,
                             BSONObjBuilder* out) {
    BSONObjBuilder explainCmdBob;
    int options = 0;
    ClusterExplain::wrapAsExplain(
        findCommand, verbosity, serverSelectionMetadata, &explainCmdBob, &options);

    // We will time how long it takes to run the commands on the shards.
    Timer timer;

    std::vector<Strategy::CommandResult> shardResults;
    Strategy::commandOp(opCtx,
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
        opCtx, shardResults, mongosStageName, millisElapsed, out);
}

/**
 * Called into by the commands infrastructure.
 */
void execCommandClient(OperationContext* opCtx,
                       Command* c,
                       int queryOptions,
                       const char* ns,
                       BSONObj& cmdObj,
                       BSONObjBuilder& result) {
    const std::string dbname = nsToDatabase(ns);

    ON_BLOCK_EXIT([opCtx, &result] { appendRequiredFieldsToResponse(opCtx, &result); });

    StringMap<int> topLevelFields;
    for (auto&& element : cmdObj) {
        StringData fieldName = element.fieldNameStringData();
        if (fieldName == "help" && element.type() == Bool && element.Bool()) {
            std::stringstream help;
            help << "help for: " << c->getName() << " ";
            c->help(help);
            result.append("help", help.str());
            Command::appendCommandStatus(result, true, "");
            return;
        }

        uassert(ErrorCodes::FailedToParse,
                str::stream() << "Parsed command object contains duplicate top level key: "
                              << fieldName,
                topLevelFields[fieldName]++ == 0);
    }

    Status status = Command::checkAuthorization(c, opCtx, dbname, cmdObj);
    if (!status.isOK()) {
        Command::appendCommandStatus(result, status);
        return;
    }

    c->_commandsExecuted.increment();

    if (c->shouldAffectCommandCounter()) {
        globalOpCounters.gotCommand();
    }

    StatusWith<WriteConcernOptions> wcResult =
        WriteConcernOptions::extractWCFromCommand(cmdObj, dbname);
    if (!wcResult.isOK()) {
        Command::appendCommandStatus(result, wcResult.getStatus());
        return;
    }

    bool supportsWriteConcern = c->supportsWriteConcern(cmdObj);
    if (!supportsWriteConcern && !wcResult.getValue().usedDefault) {
        // This command doesn't do writes so it should not be passed a writeConcern.
        // If we did not use the default writeConcern, one was provided when it shouldn't have
        // been by the user.
        Command::appendCommandStatus(
            result, Status(ErrorCodes::InvalidOptions, "Command does not support writeConcern"));
        return;
    }

    // attach tracking
    rpc::TrackingMetadata trackingMetadata;
    trackingMetadata.initWithOperName(c->getName());
    rpc::TrackingMetadata::get(opCtx) = trackingMetadata;

    auto metadataStatus = processCommandMetadata(opCtx, cmdObj);
    if (!metadataStatus.isOK()) {
        Command::appendCommandStatus(result, metadataStatus);
        return;
    }

    std::string errmsg;
    bool ok = false;
    try {
        if (!supportsWriteConcern) {
            ok = c->run(opCtx, dbname, cmdObj, queryOptions, errmsg, result);
        } else {
            // Change the write concern while running the command.
            const auto oldWC = opCtx->getWriteConcern();
            ON_BLOCK_EXIT([&] { opCtx->setWriteConcern(oldWC); });
            opCtx->setWriteConcern(wcResult.getValue());

            ok = c->run(opCtx, dbname, cmdObj, queryOptions, errmsg, result);
        }
    } catch (const DBException& e) {
        result.resetToEmpty();
        const int code = e.getCode();

        // Codes for StaleConfigException
        if (code == ErrorCodes::RecvStaleConfig || code == ErrorCodes::SendStaleConfig) {
            throw;
        }

        errmsg = e.what();
        result.append("code", code);
    }

    if (!ok) {
        c->_commandsFailed.increment();
    }

    Command::appendCommandStatus(result, ok, errmsg);
}

}  // namespace mongo
