/*
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
#include "mongo/client/connpool.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/client/parallel.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/max_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/db/query/query_request.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/stats/counters.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/s/bson_serializable.h"
#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/client/version_manager.h"
#include "mongo/s/commands/cluster_explain.h"
#include "mongo/s/commands/request.h"
#include "mongo/s/config.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/query/cluster_find.h"
#include "mongo/s/stale_exception.h"
#include "mongo/s/write_ops/batch_upconvert.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/timer.h"

namespace mongo {

using std::unique_ptr;
using std::shared_ptr;
using std::set;
using std::string;
using std::stringstream;
using std::vector;

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

    Command::execCommandClientBasic(txn, c, cc(), queryOptions, ns, jsobj, anObjBuilder);
}

}  // namespace

void Strategy::queryOp(OperationContext* txn, Request& request) {
    verify(!NamespaceString(request.getns()).isCommand());

    globalOpCounters.gotQuery();

    QueryMessage q(request.d());

    NamespaceString ns(q.ns);
    ClientBasic* client = txn->getClient();
    AuthorizationSession* authSession = AuthorizationSession::get(client);
    Status status = authSession->checkAuthForFind(ns, false);
    audit::logQueryAuthzCheck(client, ns, q.query, status.code());
    uassertStatusOK(status);

    LOG(3) << "query: " << q.ns << " " << q.query << " ntoreturn: " << q.ntoreturn
           << " options: " << q.queryOptions;

    if (q.ntoreturn == 1 && strstr(q.ns, ".$cmd"))
        throw UserException(8010, "something is wrong, shouldn't see a command here");

    if (q.queryOptions & QueryOption_Exhaust) {
        uasserted(18526,
                  string("the 'exhaust' query option is invalid for mongos queries: ") + q.ns +
                      " " + q.query.toString());
    }

    // Determine the default read preference mode based on the value of the slaveOk flag.
    ReadPreference readPreferenceOption = (q.queryOptions & QueryOption_SlaveOk)
        ? ReadPreference::SecondaryPreferred
        : ReadPreference::PrimaryOnly;
    ReadPreferenceSetting readPreference(readPreferenceOption, TagSet());

    BSONElement rpElem;
    auto readPrefExtractStatus =
        bsonExtractTypedField(q.query, QueryRequest::kWrappedReadPrefField, mongo::Object, &rpElem);

    if (readPrefExtractStatus.isOK()) {
        auto parsedRps = ReadPreferenceSetting::fromBSON(rpElem.Obj());
        uassertStatusOK(parsedRps.getStatus());
        readPreference = parsedRps.getValue();
    } else if (readPrefExtractStatus != ErrorCodes::NoSuchKey) {
        uassertStatusOK(readPrefExtractStatus);
    }

    auto canonicalQuery = CanonicalQuery::canonicalize(txn, q, ExtensionsCallbackNoop());
    uassertStatusOK(canonicalQuery.getStatus());

    // If the $explain flag was set, we must run the operation on the shards as an explain command
    // rather than a find command.
    if (canonicalQuery.getValue()->getQueryRequest().isExplain()) {
        const QueryRequest& qr = canonicalQuery.getValue()->getQueryRequest();
        BSONObj findCommand = qr.asFindCommand();

        // We default to allPlansExecution verbosity.
        auto verbosity = ExplainCommon::EXEC_ALL_PLANS;

        const bool secondaryOk = (readPreference.pref != ReadPreference::PrimaryOnly);
        rpc::ServerSelectionMetadata metadata(secondaryOk, readPreference);

        BSONObjBuilder explainBuilder;
        uassertStatusOK(
            Strategy::explainFind(txn, findCommand, qr, verbosity, metadata, &explainBuilder));

        BSONObj explainObj = explainBuilder.done();
        replyToQuery(0,  // query result flags
                     request.session(),
                     request.m(),
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
    auto cursorId = ClusterFind::runQuery(txn, *canonicalQuery.getValue(), readPreference, &batch);
    uassertStatusOK(cursorId.getStatus());

    // Fill out the response buffer.
    int numResults = 0;
    OpQueryReplyBuilder reply;
    for (auto&& obj : batch) {
        obj.appendSelfToBufBuilder(reply.bufBuilderForResults());
        numResults++;
    }
    reply.send(request.session(),
               0,  // query result flags
               request.m(),
               numResults,
               0,  // startingFrom
               cursorId.getValue());
}

void Strategy::clientCommandOp(OperationContext* txn, Request& request) {
    QueryMessage q(request.d());

    LOG(3) << "command: " << q.ns << " " << q.query << " ntoreturn: " << q.ntoreturn
           << " options: " << q.queryOptions;

    if (q.queryOptions & QueryOption_Exhaust) {
        uasserted(18527,
                  string("the 'exhaust' query option is invalid for mongos commands: ") + q.ns +
                      " " + q.query.toString());
    }

    NamespaceString nss(request.getns());
    // Regular queries are handled in strategy_shard.cpp
    verify(nss.isCommand() || nss.isSpecialCommand());

    if (handleSpecialNamespaces(txn, request, q))
        return;

    int loops = 5;

    while (true) {
        try {
            BSONObj cmdObj = q.query;
            {
                BSONElement e = cmdObj.firstElement();
                if (e.type() == Object &&
                    (e.fieldName()[0] == '$' ? str::equals("query", e.fieldName() + 1)
                                             : str::equals("query", e.fieldName()))) {
                    // Extract the embedded query object.

                    if (cmdObj.hasField(Query::ReadPrefField.name())) {
                        // The command has a read preference setting. We don't want
                        // to lose this information so we copy this to a new field
                        // called $queryOptions.$readPreference
                        BSONObjBuilder finalCmdObjBuilder;
                        finalCmdObjBuilder.appendElements(e.embeddedObject());

                        BSONObjBuilder queryOptionsBuilder(
                            finalCmdObjBuilder.subobjStart("$queryOptions"));
                        queryOptionsBuilder.append(cmdObj[Query::ReadPrefField.name()]);
                        queryOptionsBuilder.done();

                        cmdObj = finalCmdObjBuilder.obj();
                    } else {
                        cmdObj = e.embeddedObject();
                    }
                }
            }

            OpQueryReplyBuilder reply;
            {
                BSONObjBuilder builder(reply.bufBuilderForResults());
                runAgainstRegistered(txn, q.ns, cmdObj, builder, q.queryOptions);
            }
            reply.sendCommandReply(request.session(), request.m());
            return;
        } catch (const StaleConfigException& e) {
            if (loops <= 0)
                throw e;

            loops--;
            log() << "retrying command: " << q.query;

            // For legacy reasons, ns may not actually be set in the exception :-(
            string staleNS = e.getns();
            if (staleNS.size() == 0)
                staleNS = q.ns;

            ShardConnection::checkMyConnectionVersions(txn, staleNS);
            if (loops < 4)
                versionManager.forceRemoteCheckShardVersionCB(txn, staleNS);
        } catch (const DBException& e) {
            OpQueryReplyBuilder reply;
            {
                BSONObjBuilder builder(reply.bufBuilderForResults());
                Command::appendCommandStatus(builder, e.toStatus());
            }
            reply.sendCommandReply(request.session(), request.m());
            return;
        }
    }
}

// TODO: remove after MongoDB 3.2
bool Strategy::handleSpecialNamespaces(OperationContext* txn, Request& request, QueryMessage& q) {
    const char* ns = strstr(request.getns(), ".$cmd.sys.");
    if (!ns)
        return false;
    ns += 10;

    BSONObjBuilder reply;

    const auto upgradeToRealCommand = [txn, &q, &reply](StringData commandName) {
        BSONObjBuilder cmdBob;
        cmdBob.append(commandName, 1);
        cmdBob.appendElements(q.query);  // fields are validated by Commands
        auto interposedCmd = cmdBob.done();
        // Rewrite upgraded pseudoCommands to run on the 'admin' database.
        NamespaceString interposedNss("admin", "$cmd");
        runAgainstRegistered(txn, interposedNss.ns().c_str(), interposedCmd, reply, q.queryOptions);
    };

    if (strcmp(ns, "inprog") == 0) {
        upgradeToRealCommand("currentOp");
    } else if (strcmp(ns, "killop") == 0) {
        upgradeToRealCommand("killOp");
    } else if (strcmp(ns, "unlock") == 0) {
        reply.append("err", "can't do unlock through mongos");
    } else {
        warning() << "unknown sys command [" << ns << "]";
        return false;
    }

    BSONObj x = reply.done();
    replyToQuery(0, request.session(), request.m(), x);
    return true;
}

void Strategy::commandOp(OperationContext* txn,
                         const string& db,
                         const BSONObj& command,
                         int options,
                         const string& versionedNS,
                         const BSONObj& targetingQuery,
                         vector<CommandResult>* results) {
    QuerySpec qSpec(db + ".$cmd", command, BSONObj(), 0, 1, options);

    ParallelSortClusteredCursor cursor(qSpec, CommandInfo(versionedNS, targetingQuery));

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

void Strategy::getMore(OperationContext* txn, Request& request) {
    const char* ns = request.getns();
    const int ntoreturn = request.d().pullInt();
    uassert(
        34424, str::stream() << "Invalid ntoreturn for OP_GET_MORE: " << ntoreturn, ntoreturn >= 0);
    const long long id = request.d().pullInt64();

    // TODO: Handle stale config exceptions here from coll being dropped or sharded during op for
    // now has same semantics as legacy request.
    const NamespaceString nss(ns);
    auto statusGetDb = grid.catalogCache()->getDatabase(txn, nss.db().toString());
    if (statusGetDb == ErrorCodes::NamespaceNotFound) {
        replyToQuery(ResultFlag_CursorNotFound, request.session(), request.m(), 0, 0, 0);
        return;
    }

    uassertStatusOK(statusGetDb);

    boost::optional<long long> batchSize;
    if (ntoreturn) {
        batchSize = ntoreturn;
    }
    GetMoreRequest getMoreRequest(
        NamespaceString(ns), id, batchSize, boost::none, boost::none, boost::none);

    auto cursorResponse = ClusterFind::runGetMore(txn, getMoreRequest);
    if (cursorResponse == ErrorCodes::CursorNotFound) {
        replyToQuery(ResultFlag_CursorNotFound, request.session(), request.m(), 0, 0, 0);
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
                 request.session(),
                 request.m(),
                 buffer.buf(),
                 buffer.len(),
                 numResults,
                 cursorResponse.getValue().getNumReturnedSoFar().value_or(0),
                 cursorResponse.getValue().getCursorId());
}

void Strategy::killCursors(OperationContext* txn, Request& request) {
    DbMessage& dbMessage = request.d();
    const int numCursors = dbMessage.pullInt();
    massert(34425,
            str::stream() << "Invalid killCursors message. numCursors: " << numCursors
                          << ", message size: "
                          << dbMessage.msg().dataSize()
                          << ".",
            dbMessage.msg().dataSize() == 8 + (8 * numCursors));
    uassert(28794,
            str::stream() << "numCursors must be between 1 and 29999.  numCursors: " << numCursors
                          << ".",
            numCursors >= 1 && numCursors < 30000);
    ConstDataCursor cursors(dbMessage.getArray(numCursors));
    Client* client = txn->getClient();
    AuthorizationSession* authSession = AuthorizationSession::get(client);
    ClusterCursorManager* manager = grid.getCursorManager();

    for (int i = 0; i < numCursors; ++i) {
        CursorId cursorId = cursors.readAndAdvance<LittleEndian<int64_t>>();
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

void Strategy::writeOp(OperationContext* txn, int op, Request& request) {
    // make sure we have a last error
    dassert(&LastError::get(cc()));

    OwnedPointerVector<BatchedCommandRequest> commandRequestsOwned;
    vector<BatchedCommandRequest*>& commandRequests = commandRequestsOwned.mutableVector();

    msgToBatchRequests(request.m(), &commandRequests);

    for (vector<BatchedCommandRequest*>::iterator it = commandRequests.begin();
         it != commandRequests.end();
         ++it) {
        // Multiple commands registered to last error as multiple requests
        if (it != commandRequests.begin())
            LastError::get(cc()).startRequest();

        BatchedCommandRequest* commandRequest = *it;

        // Adjust namespaces for command
        NamespaceString fullNS(commandRequest->getNS());
        string cmdNS = fullNS.getCommandNS();
        // We only pass in collection name to command
        commandRequest->setNS(fullNS);

        BSONObjBuilder builder;
        BSONObj requestBSON = commandRequest->toBSON();

        {
            // Disable the last error object for the duration of the write cmd
            LastError::Disabled disableLastError(&LastError::get(cc()));
            runAgainstRegistered(txn, cmdNS.c_str(), requestBSON, builder, 0);
        }

        BatchedCommandResponse commandResponse;
        bool parsed = commandResponse.parseBSON(builder.done(), NULL);
        (void)parsed;  // for compile
        dassert(parsed && commandResponse.isValid(NULL));

        // Populate the lastError object based on the write response
        LastError::get(cc()).reset();
        bool hadError =
            batchErrorToLastError(*commandRequest, commandResponse, &LastError::get(cc()));

        // Check if this is an ordered batch and we had an error which should stop processing
        if (commandRequest->getOrdered() && hadError)
            break;
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
                        &shardResults);

    long long millisElapsed = timer.millis();

    const char* mongosStageName = ClusterExplain::getStageNameForReadOp(shardResults, findCommand);

    return ClusterExplain::buildExplainResult(
        txn, shardResults, mongosStageName, millisElapsed, out);
}

}  // namespace mongo
