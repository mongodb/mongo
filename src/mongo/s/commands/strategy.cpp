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
#include "mongo/base/status.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/initialize_operation_session_info.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_session_id_helpers.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_time_tracker.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/db/query/query_request.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/logical_time_metadata.h"
#include "mongo/rpc/metadata/tracking_metadata.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/parallel.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/commands/cluster_commands_helpers.h"
#include "mongo/s/commands/cluster_explain.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/query/cluster_find.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/op_msg.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace {

const auto kOperationTime = "operationTime"_sd;

/**
 * Extract and process metadata from the command request body.
 */
Status processCommandMetadata(OperationContext* opCtx, const BSONObj& cmdObj) {
    ReadPreferenceSetting::get(opCtx) =
        uassertStatusOK(ReadPreferenceSetting::fromContainingBSON(cmdObj));

    auto logicalClock = LogicalClock::get(opCtx);
    invariant(logicalClock);

    auto logicalTimeMetadata = rpc::LogicalTimeMetadata::readFromMetadata(cmdObj);
    if (!logicalTimeMetadata.isOK()) {
        return logicalTimeMetadata.getStatus();
    }

    auto logicalTimeValidator = LogicalTimeValidator::get(opCtx);
    const auto& signedTime = logicalTimeMetadata.getValue().getSignedTime();

    // No need to check proof is no time is given.
    if (signedTime.getTime() == LogicalTime::kUninitialized) {
        return Status::OK();
    }

    if (!LogicalTimeValidator::isAuthorizedToAdvanceClock(opCtx)) {
        auto advanceClockStatus = logicalTimeValidator->validate(opCtx, signedTime);

        if (!advanceClockStatus.isOK()) {
            return advanceClockStatus;
        }
    }

    return logicalClock->advanceClusterTime(signedTime.getTime());
}

/**
 * Append required fields to command response.
 */
void appendRequiredFieldsToResponse(OperationContext* opCtx, BSONObjBuilder* responseBuilder) {
    auto validator = LogicalTimeValidator::get(opCtx);
    if (validator->shouldGossipLogicalTime()) {
        auto now = LogicalClock::get(opCtx)->getClusterTime();

        // Add operationTime.
        auto operationTime = OperationTimeTracker::get(opCtx)->getMaxOperationTime();
        if (operationTime != LogicalTime::kUninitialized) {
            responseBuilder->append(kOperationTime, operationTime.asTimestamp());
        } else if (now != LogicalTime::kUninitialized) {
            // If we don't know the actual operation time, use the cluster time instead. This is
            // safe but not optimal because we can always return a later operation time than actual.
            responseBuilder->append(kOperationTime, now.asTimestamp());
        }

        // Add $clusterTime.
        if (LogicalTimeValidator::isAuthorizedToAdvanceClock(opCtx)) {
            SignedLogicalTime dummySignedTime(now, TimeProofService::TimeProof(), 0);
            rpc::LogicalTimeMetadata(dummySignedTime).writeToMetadata(responseBuilder);
        } else {
            auto currentTime = validator->signLogicalTime(opCtx, now);
            rpc::LogicalTimeMetadata(currentTime).writeToMetadata(responseBuilder);
        }
    }
}

void execCommandClient(OperationContext* opCtx,
                       Command* c,
                       const OpMsgRequest& request,
                       BSONObjBuilder& result) {
    ON_BLOCK_EXIT([opCtx, &result] { appendRequiredFieldsToResponse(opCtx, &result); });

    const auto dbname = request.getDatabase().toString();
    uassert(ErrorCodes::IllegalOperation,
            "Can't use 'local' database through mongos",
            dbname != NamespaceString::kLocalDb);
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "Invalid database name: '" << dbname << "'",
            NamespaceString::validDBName(dbname, NamespaceString::DollarInDbNameBehavior::Allow));

    StringMap<int> topLevelFields;
    for (auto&& element : request.body) {
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

    Status status = Command::checkAuthorization(c, opCtx, request);
    if (!status.isOK()) {
        Command::appendCommandStatus(result, status);
        return;
    }

    c->incrementCommandsExecuted();

    if (c->shouldAffectCommandCounter()) {
        globalOpCounters.gotCommand();
    }

    StatusWith<WriteConcernOptions> wcResult =
        WriteConcernOptions::extractWCFromCommand(request.body, dbname);
    if (!wcResult.isOK()) {
        Command::appendCommandStatus(result, wcResult.getStatus());
        return;
    }

    bool supportsWriteConcern = c->supportsWriteConcern(request.body);
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

    auto metadataStatus = processCommandMetadata(opCtx, request.body);
    if (!metadataStatus.isOK()) {
        Command::appendCommandStatus(result, metadataStatus);
        return;
    }

    bool ok = false;
    if (!supportsWriteConcern) {
        ok = c->publicRun(opCtx, request, result);
    } else {
        // Change the write concern while running the command.
        const auto oldWC = opCtx->getWriteConcern();
        ON_BLOCK_EXIT([&] { opCtx->setWriteConcern(oldWC); });
        opCtx->setWriteConcern(wcResult.getValue());

        ok = c->publicRun(opCtx, request, result);
    }
    if (!ok) {
        c->incrementCommandsFailed();
    }
    Command::appendCommandStatus(result, ok);
}

void runCommand(OperationContext* opCtx, const OpMsgRequest& request, BSONObjBuilder&& builder) {
    // Handle command option maxTimeMS first thing while processing the command so that the
    // subsequent code has the deadline available
    uassert(ErrorCodes::InvalidOptions,
            "no such command option $maxTimeMs; use maxTimeMS instead",
            request.body[QueryRequest::queryOptionMaxTimeMS].eoo());

    const int maxTimeMS = uassertStatusOK(
        QueryRequest::parseMaxTimeMS(request.body[QueryRequest::cmdOptionMaxTimeMS]));
    if (maxTimeMS > 0) {
        opCtx->setDeadlineAfterNowBy(Milliseconds{maxTimeMS});
    }

    auto const commandName = request.getCommandName();
    auto const command = Command::findCommand(commandName);
    if (!command) {
        ON_BLOCK_EXIT([opCtx, &builder] { appendRequiredFieldsToResponse(opCtx, &builder); });
        Command::appendCommandStatus(
            builder,
            {ErrorCodes::CommandNotFound, str::stream() << "no such cmd: " << commandName});
        Command::unknownCommands.increment();
        return;
    }

    uassertStatusOK(
        initializeOperationSessionInfo(opCtx, request.body, command->requiresAuth(), true, true));

    int loops = 5;

    while (true) {
        builder.resetToEmpty();
        try {
            execCommandClient(opCtx, command, request, builder);
            return;
        } catch (const StaleConfigException& e) {
            if (e.getns().empty()) {
                // This should be impossible but older versions tried incorrectly to handle it here.
                log() << "Received a stale config error with an empty namespace while executing "
                      << redact(request.body) << " : " << redact(e);
                throw;
            }

            if (loops <= 0)
                throw e;

            loops--;
            log() << "Retrying command " << redact(request.body) << causedBy(e);

            ShardConnection::checkMyConnectionVersions(opCtx, e.getns());
            if (loops < 4) {
                const NamespaceString staleNSS(e.getns());
                if (staleNSS.isValid()) {
                    Grid::get(opCtx)->catalogCache()->invalidateShardedCollection(staleNSS);
                }
            }

            continue;
        } catch (const DBException& e) {
            ON_BLOCK_EXIT([opCtx, &builder] { appendRequiredFieldsToResponse(opCtx, &builder); });
            builder.resetToEmpty();
            command->incrementCommandsFailed();
            Command::appendCommandStatus(builder, e.toStatus());
            LastError::get(opCtx->getClient()).setLastError(e.code(), e.reason());
            return;
        }

        MONGO_UNREACHABLE;
    }
}

}  // namespace

DbResponse Strategy::queryOp(OperationContext* opCtx, const NamespaceString& nss, DbMessage* dbm) {
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
    const auto defaultReadPref = q.queryOptions & QueryOption_SlaveOk
        ? ReadPreference::SecondaryPreferred
        : ReadPreference::PrimaryOnly;
    ReadPreferenceSetting::get(opCtx) =
        uassertStatusOK(ReadPreferenceSetting::fromContainingBSON(q.query, defaultReadPref));

    const boost::intrusive_ptr<ExpressionContext> expCtx;
    auto canonicalQuery = uassertStatusOK(
        CanonicalQuery::canonicalize(opCtx,
                                     q,
                                     expCtx,
                                     ExtensionsCallbackNoop(),
                                     MatchExpressionParser::kAllowAllSpecialFeatures));

    // If the $explain flag was set, we must run the operation on the shards as an explain command
    // rather than a find command.
    const QueryRequest& queryRequest = canonicalQuery->getQueryRequest();
    if (queryRequest.isExplain()) {
        const BSONObj findCommand = queryRequest.asFindCommand();

        // We default to allPlansExecution verbosity.
        const auto verbosity = ExplainOptions::Verbosity::kExecAllPlans;

        BSONObjBuilder explainBuilder;
        uassertStatusOK(Strategy::explainFind(opCtx,
                                              findCommand,
                                              queryRequest,
                                              verbosity,
                                              ReadPreferenceSetting::get(opCtx),
                                              &explainBuilder));

        BSONObj explainObj = explainBuilder.done();
        return replyToQuery(explainObj);
    }

    // Do the work to generate the first batch of results. This blocks waiting to get responses from
    // the shard(s).
    std::vector<BSONObj> batch;

    // 0 means the cursor is exhausted. Otherwise we assume that a cursor with the returned id can
    // be retrieved via the ClusterCursorManager.
    auto cursorId =
        ClusterFind::runQuery(opCtx,
                              *canonicalQuery,
                              ReadPreferenceSetting::get(opCtx),
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

    return DbResponse{reply.toQueryReply(0,  // query result flags
                                         numResults,
                                         0,  // startingFrom
                                         cursorId.getValue())};
}

DbResponse Strategy::clientCommand(OperationContext* opCtx, const Message& m) {
    auto reply = rpc::makeReplyBuilder(rpc::protocolForMessage(m));

    [&] {
        OpMsgRequest request;
        std::string db;
        try {  // Parse.
            request = rpc::opMsgRequestFromAnyProtocol(m);
            db = request.getDatabase().toString();
        } catch (const DBException& ex) {
            // If this error needs to fail the connection, propagate it out.
            if (ErrorCodes::isConnectionFatalMessageParseError(ex.code()))
                throw;

            LOG(1) << "Exception thrown while parsing command " << causedBy(redact(ex));
            reply->reset();
            auto bob = reply->getInPlaceReplyBuilder(0);
            Command::appendCommandStatus(bob, ex.toStatus());
            appendRequiredFieldsToResponse(opCtx, &bob);

            return;  // From lambda. Don't try executing if parsing failed.
        }

        try {  // Execute.
            LOG(3) << "Command begin db: " << db << " msg id: " << m.header().getId();
            runCommand(opCtx, request, reply->getInPlaceReplyBuilder(0));
            LOG(3) << "Command end db: " << db << " msg id: " << m.header().getId();
        } catch (const DBException& ex) {
            LOG(1) << "Exception thrown while processing command on " << db
                   << " msg id: " << m.header().getId() << causedBy(redact(ex));

            reply->reset();
            auto bob = reply->getInPlaceReplyBuilder(0);
            Command::appendCommandStatus(bob, ex.toStatus());
            appendRequiredFieldsToResponse(opCtx, &bob);
        }
    }();

    if (OpMsg::isFlagSet(m, OpMsg::kMoreToCome)) {
        return {};  // Don't reply.
    }

    reply->setMetadata(BSONObj());  // mongos doesn't use metadata but the API requires this call.
    return DbResponse{reply->done()};
}

void Strategy::commandOp(OperationContext* opCtx,
                         const std::string& db,
                         const BSONObj& command,
                         const std::string& versionedNS,
                         const BSONObj& targetingQuery,
                         const BSONObj& targetingCollation,
                         std::vector<CommandResult>* results) {
    QuerySpec qSpec(db + ".$cmd", command, BSONObj(), 0, 1, 0);

    ParallelSortClusteredCursor cursor(
        qSpec, CommandInfo(versionedNS, targetingQuery, targetingCollation));

    // Initialize the cursor
    cursor.init(opCtx);

    std::set<ShardId> shardIds;
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

DbResponse Strategy::getMore(OperationContext* opCtx, const NamespaceString& nss, DbMessage* dbm) {
    const int ntoreturn = dbm->pullInt();
    uassert(
        34424, str::stream() << "Invalid ntoreturn for OP_GET_MORE: " << ntoreturn, ntoreturn >= 0);
    const long long cursorId = dbm->pullInt64();

    globalOpCounters.gotGetMore();

    // TODO: Handle stale config exceptions here from coll being dropped or sharded during op for
    // now has same semantics as legacy request.

    auto statusGetDb = Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, nss.db());
    if (statusGetDb == ErrorCodes::NamespaceNotFound) {
        return replyToQuery(ResultFlag_CursorNotFound, nullptr, 0, 0);
    }
    uassertStatusOK(statusGetDb);

    boost::optional<long long> batchSize;
    if (ntoreturn) {
        batchSize = ntoreturn;
    }

    GetMoreRequest getMoreRequest(nss, cursorId, batchSize, boost::none, boost::none, boost::none);

    auto cursorResponse = ClusterFind::runGetMore(opCtx, getMoreRequest);
    if (cursorResponse == ErrorCodes::CursorNotFound) {
        return replyToQuery(ResultFlag_CursorNotFound, nullptr, 0, 0);
    }
    uassertStatusOK(cursorResponse.getStatus());

    // Build the response document.
    BufBuilder buffer(FindCommon::kInitReplyBufferSize);

    int numResults = 0;
    for (const auto& obj : cursorResponse.getValue().getBatch()) {
        buffer.appendBuf((void*)obj.objdata(), obj.objsize());
        ++numResults;
    }

    return replyToQuery(0,
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

        {
            // Block scope ccPin so that it releases our checked out cursor
            // prior to the killCursor invocation below.
            auto ccPin = manager->checkOutCursor(
                *nss, cursorId, opCtx, ClusterCursorManager::kNoCheckSession);
            if (!ccPin.isOK()) {
                LOG(3) << "Unable to check out cursor for killCursor.  Namespace: '" << *nss
                       << "', cursor id: " << cursorId << ".";
                continue;
            }
            auto cursorOwners = ccPin.getValue().getAuthenticatedUsers();
            auto authorizationStatus = authSession->checkAuthForKillCursors(*nss, cursorOwners);

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
    runCommand(opCtx,
               [&]() {
                   const auto& msg = dbm->msg();

                   switch (msg.operation()) {
                       case dbInsert: {
                           return InsertOp::parseLegacy(msg).serialize({});
                       }
                       case dbUpdate: {
                           return UpdateOp::parseLegacy(msg).serialize({});
                       }
                       case dbDelete: {
                           return DeleteOp::parseLegacy(msg).serialize({});
                       }
                       default:
                           MONGO_UNREACHABLE;
                   }
               }(),
               BSONObjBuilder());
}

Status Strategy::explainFind(OperationContext* opCtx,
                             const BSONObj& findCommand,
                             const QueryRequest& qr,
                             ExplainOptions::Verbosity verbosity,
                             const ReadPreferenceSetting& readPref,
                             BSONObjBuilder* out) {
    const auto explainCmd = ClusterExplain::wrapAsExplain(findCommand, verbosity);

    // We will time how long it takes to run the commands on the shards.
    Timer timer;

    BSONObj viewDefinition;
    auto swShardResponses =
        scatterGatherVersionedTargetByRoutingTable(opCtx,
                                                   qr.nss().db().toString(),
                                                   qr.nss(),
                                                   explainCmd,
                                                   readPref,
                                                   Shard::RetryPolicy::kIdempotent,
                                                   qr.getFilter(),
                                                   qr.getCollation(),
                                                   &viewDefinition);

    long long millisElapsed = timer.millis();

    if (ErrorCodes::CommandOnShardedViewNotSupportedOnMongod == swShardResponses.getStatus()) {
        uassert(ErrorCodes::InternalError,
                str::stream() << "Missing resolved view definition, but remote returned "
                              << ErrorCodes::errorString(swShardResponses.getStatus().code()),
                !viewDefinition.isEmpty());

        out->appendElements(viewDefinition);
        return swShardResponses.getStatus();
    }

    uassertStatusOK(swShardResponses.getStatus());
    auto shardResponses = std::move(swShardResponses.getValue());

    const char* mongosStageName =
        ClusterExplain::getStageNameForReadOp(shardResponses.size(), findCommand);

    return ClusterExplain::buildExplainResult(opCtx,
                                              ClusterExplain::downconvert(opCtx, shardResponses),
                                              mongosStageName,
                                              millisElapsed,
                                              out);
}
}  // namespace mongo
