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

#include "mongo/s/strategy.h"

#include "mongo/base/data_cursor.h"
#include "mongo/base/owned_pointer_vector.h"
#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connpool.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/max_time.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/lite_parsed_query.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/db/stats/counters.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/s/bson_serializable.h"
#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/config.h"
#include "mongo/s/cursors.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_find.h"
#include "mongo/s/request.h"
#include "mongo/s/stale_exception.h"
#include "mongo/s/version_manager.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batch_upconvert.h"
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

MONGO_EXPORT_SERVER_PARAMETER(useClusterClientCursor, bool, false);

static bool _isSystemIndexes(const char* ns) {
    return nsToCollectionSubstring(ns) == "system.indexes";
}

/**
 * Returns true if request is a query for sharded indexes.
 */
static bool doShardedIndexQuery(OperationContext* txn, Request& request, const QuerySpec& qSpec) {
    // Extract the ns field from the query, which may be embedded within the "query" or
    // "$query" field.
    auto nsField = qSpec.filter()["ns"];
    if (nsField.eoo()) {
        return false;
    }
    const NamespaceString indexNSSQuery(nsField.str());

    auto status = grid.catalogCache()->getDatabase(txn, indexNSSQuery.db().toString());
    if (!status.isOK()) {
        return false;
    }

    shared_ptr<DBConfig> config = status.getValue();
    if (!config->isSharded(indexNSSQuery.ns())) {
        return false;
    }

    // if you are querying on system.indexes, we need to make sure we go to a shard
    // that actually has chunks. This is not a perfect solution (what if you just
    // look at all indexes), but better than doing nothing.

    ShardPtr shard;
    ChunkManagerPtr cm;
    config->getChunkManagerOrPrimary(txn, indexNSSQuery.ns(), cm, shard);
    if (cm) {
        set<ShardId> shardIds;
        cm->getAllShardIds(&shardIds);
        verify(shardIds.size() > 0);
        shard = grid.shardRegistry()->getShard(txn, *shardIds.begin());
    }

    ShardConnection dbcon(shard->getConnString(), request.getns());
    DBClientBase& c = dbcon.conn();

    string actualServer;

    Message response;
    bool ok = c.call(request.m(), response, true, &actualServer);
    uassert(10200, "mongos: error calling db", ok);

    {
        QueryResult::View qr = response.singleData().view2ptr();
        if (qr.getResultFlags() & ResultFlag_ShardConfigStale) {
            dbcon.done();
            // Version is zero b/c this is deprecated codepath
            throw RecvStaleConfigException(request.getns(),
                                           "Strategy::doQuery",
                                           ChunkVersion(0, 0, OID()),
                                           ChunkVersion(0, 0, OID()));
        }
    }

    request.reply(response, actualServer.size() ? actualServer : c.getServerAddress());
    dbcon.done();

    return true;
}

void Strategy::queryOp(OperationContext* txn, Request& request) {
    verify(!NamespaceString(request.getns()).isCommand());

    Timer queryTimer;

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

    // Spigot which controls whether OP_QUERY style find on mongos uses the new ClusterClientCursor
    // code path.
    // TODO: Delete the spigot and always use the new code.
    if (useClusterClientCursor) {
        ReadPreferenceSetting readPreference(ReadPreference::PrimaryOnly, TagSet::primaryOnly());

        BSONElement rpElem;
        auto readPrefExtractStatus = bsonExtractTypedField(
            q.query, LiteParsedQuery::kWrappedReadPrefField, mongo::Object, &rpElem);

        if (readPrefExtractStatus.isOK()) {
            auto parsedRps = ReadPreferenceSetting::fromBSON(rpElem.Obj());
            uassertStatusOK(parsedRps.getStatus());
            readPreference = parsedRps.getValue();
        } else if (readPrefExtractStatus != ErrorCodes::NoSuchKey) {
            uassertStatusOK(readPrefExtractStatus);
        }

        auto canonicalQuery = CanonicalQuery::canonicalize(q, WhereCallbackNoop());
        uassertStatusOK(canonicalQuery.getStatus());

        // If the $explain flag was set, we must run the operation on the shards as an explain
        // command rather than a find command.
        if (canonicalQuery.getValue()->getParsed().isExplain()) {
            const LiteParsedQuery& lpq = canonicalQuery.getValue()->getParsed();
            BSONObj findCommand = lpq.asFindCommand();

            // We default to allPlansExecution verbosity.
            auto verbosity = ExplainCommon::EXEC_ALL_PLANS;

            const bool secondaryOk = (readPreference.pref != ReadPreference::PrimaryOnly);
            rpc::ServerSelectionMetadata metadata(secondaryOk, readPreference);

            BSONObjBuilder explainBuilder;
            uassertStatusOK(ClusterFind::runExplain(
                txn, findCommand, lpq, verbosity, metadata, &explainBuilder));

            BSONObj explainObj = explainBuilder.done();
            replyToQuery(0,  // query result flags
                         request.p(),
                         request.m(),
                         static_cast<const void*>(explainObj.objdata()),
                         explainObj.objsize(),
                         1,  // numResults
                         0,  // startingFrom
                         CursorId(0));
            return;
        }

        // Do the work to generate the first batch of results. This blocks waiting to get responses
        // from the shard(s).
        std::vector<BSONObj> batch;

        // 0 means the cursor is exhausted and
        // otherwise we assume that a cursor with the returned id can be retrieved via the
        // ClusterCursorManager
        auto cursorId =
            ClusterFind::runQuery(txn, *canonicalQuery.getValue(), readPreference, &batch);
        uassertStatusOK(cursorId.getStatus());

        // TODO: this constant should be shared between mongos and mongod, and should
        // not be inside ShardedClientCursor.
        BufBuilder buffer(ShardedClientCursor::INIT_REPLY_BUFFER_SIZE);

        // Fill out the response buffer.
        int numResults = 0;
        for (const auto& obj : batch) {
            buffer.appendBuf((void*)obj.objdata(), obj.objsize());
            numResults++;
        }

        replyToQuery(0,  // query result flags
                     request.p(),
                     request.m(),
                     buffer.buf(),
                     buffer.len(),
                     numResults,
                     0,  // startingFrom
                     cursorId.getValue());
        return;
    }

    QuerySpec qSpec((string)q.ns, q.query, q.fields, q.ntoskip, q.ntoreturn, q.queryOptions);

    // Parse "$maxTimeMS".
    StatusWith<int> maxTimeMS = LiteParsedQuery::parseMaxTimeMSQuery(q.query);
    uassert(17233, maxTimeMS.getStatus().reason(), maxTimeMS.isOK());

    if (_isSystemIndexes(q.ns) && doShardedIndexQuery(txn, request, qSpec)) {
        return;
    }

    ParallelSortClusteredCursor* cursor = new ParallelSortClusteredCursor(qSpec, CommandInfo());
    verify(cursor);

    // TODO:  Move out to Request itself, not strategy based
    try {
        cursor->init(txn);

        if (qSpec.isExplain()) {
            BSONObjBuilder explain_builder;
            cursor->explain(explain_builder);
            explain_builder.appendNumber("executionTimeMillis",
                                         static_cast<long long>(queryTimer.millis()));
            BSONObj b = explain_builder.obj();

            replyToQuery(0, request.p(), request.m(), b);
            delete (cursor);
            return;
        }
    } catch (...) {
        delete cursor;
        throw;
    }

    // TODO: Revisit all of this when we revisit the sharded cursor cache

    if (cursor->getNumQueryShards() != 1) {
        // More than one shard (or zero), manage with a ShardedClientCursor
        // NOTE: We may also have *zero* shards here when the returnPartial flag is set.
        // Currently the code in ShardedClientCursor handles this.

        ShardedClientCursorPtr cc(new ShardedClientCursor(q, cursor));

        BufBuilder buffer(ShardedClientCursor::INIT_REPLY_BUFFER_SIZE);
        int docCount = 0;
        const int startFrom = cc->getTotalSent();
        bool hasMore = cc->sendNextBatch(q.ntoreturn, buffer, docCount);

        if (hasMore) {
            LOG(5) << "storing cursor : " << cc->getId();

            int cursorLeftoverMillis = maxTimeMS.getValue() - queryTimer.millis();
            if (maxTimeMS.getValue() == 0) {  // 0 represents "no limit".
                cursorLeftoverMillis = kMaxTimeCursorNoTimeLimit;
            } else if (cursorLeftoverMillis <= 0) {
                cursorLeftoverMillis = kMaxTimeCursorTimeLimitExpired;
            }

            cursorCache.store(cc, cursorLeftoverMillis);
        }

        replyToQuery(0,
                     request.p(),
                     request.m(),
                     buffer.buf(),
                     buffer.len(),
                     docCount,
                     startFrom,
                     hasMore ? cc->getId() : 0);
    } else {
        // Only one shard is used

        // Remote cursors are stored remotely, we shouldn't need this around.
        unique_ptr<ParallelSortClusteredCursor> cursorDeleter(cursor);

        ShardPtr shard = grid.shardRegistry()->getShard(txn, cursor->getQueryShardId());
        verify(shard.get());
        DBClientCursorPtr shardCursor = cursor->getShardCursor(shard->getId());

        // Implicitly stores the cursor in the cache
        request.reply(*(shardCursor->getMessage()), shardCursor->originalHost());

        // We don't want to kill the cursor remotely if there's still data left
        shardCursor->decouple();
    }
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
    bool cmChangeAttempted = false;

    while (true) {
        BSONObjBuilder builder;
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

            Command::runAgainstRegistered(txn, q.ns, cmdObj, builder, q.queryOptions);
            BSONObj x = builder.done();
            replyToQuery(0, request.p(), request.m(), x);
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
            if (e.getCode() == ErrorCodes::IncompatibleCatalogManager) {
                fassert(28791, !cmChangeAttempted);
                cmChangeAttempted = true;

                grid.forwardingCatalogManager()->waitForCatalogManagerChange(txn);
            } else {
                Command::appendCommandStatus(builder, e.toStatus());
                BSONObj x = builder.done();
                replyToQuery(0, request.p(), request.m(), x);
                return;
            }
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
        Command::runAgainstRegistered(
            txn, interposedNss.ns().c_str(), interposedCmd, reply, q.queryOptions);
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
    replyToQuery(0, request.p(), request.m(), x);
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
            28739, ConnectionString::parse(cursor.getShardCursor(shardId)->originalHost()));
        result.result = cursor.getShardCursor(shardId)->peekFirst().getOwned();
        results->push_back(result);
    }
}

Status Strategy::commandOpUnsharded(OperationContext* txn,
                                    const std::string& db,
                                    const BSONObj& command,
                                    int options,
                                    const std::string& versionedNS,
                                    CommandResult* cmdResult) {
    // Note that this implementation will not handle targeting retries and fails when the
    // sharding metadata is too stale
    auto status = grid.catalogCache()->getDatabase(txn, db);
    if (!status.isOK()) {
        mongoutils::str::stream ss;
        ss << "Passthrough command failed: " << command.toString() << " on ns " << versionedNS
           << ". Caused by " << causedBy(status.getStatus());
        return Status(ErrorCodes::IllegalOperation, ss);
    }

    shared_ptr<DBConfig> conf = status.getValue();
    if (conf->isSharded(versionedNS)) {
        mongoutils::str::stream ss;
        ss << "Passthrough command failed: " << command.toString() << " on ns " << versionedNS
           << ". Cannot run on sharded namespace.";
        return Status(ErrorCodes::IllegalOperation, ss);
    }

    const auto primaryShard = grid.shardRegistry()->getShard(txn, conf->getPrimaryId());

    BSONObj shardResult;
    try {
        ShardConnection conn(primaryShard->getConnString(), "");

        // TODO: this can throw a stale config when mongos is not up-to-date -- fix.
        if (!conn->runCommand(db, command, shardResult, options)) {
            conn.done();
            return Status(ErrorCodes::OperationFailed,
                          str::stream() << "Passthrough command failed: " << command << " on ns "
                                        << versionedNS << "; result: " << shardResult);
        }
        conn.done();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    // Fill out the command result.
    cmdResult->shardTargetId = conf->getPrimaryId();
    cmdResult->result = shardResult;
    cmdResult->target = primaryShard->getConnString();

    return Status::OK();
}

void Strategy::getMore(OperationContext* txn, Request& request) {
    Timer getMoreTimer;

    const char* ns = request.getns();
    const int ntoreturn = request.d().pullInt();
    const long long id = request.d().pullInt64();

    // TODO:  Handle stale config exceptions here from coll being dropped or sharded during op
    // for now has same semantics as legacy request
    const NamespaceString nss(ns);
    auto statusGetDb = grid.catalogCache()->getDatabase(txn, nss.db().toString());
    if (statusGetDb == ErrorCodes::DatabaseNotFound) {
        cursorCache.remove(id);
        replyToQuery(ResultFlag_CursorNotFound, request.p(), request.m(), 0, 0, 0);
        return;
    }

    uassertStatusOK(statusGetDb);

    // Spigot which controls whether OP_QUERY style find on mongos uses the new ClusterClientCursor
    // code path.
    //
    // TODO: Delete the spigot and always use the new code.
    if (useClusterClientCursor) {
        boost::optional<long long> batchSize;
        if (ntoreturn) {
            batchSize = ntoreturn;
        }
        GetMoreRequest getMoreRequest(NamespaceString(ns), id, batchSize, boost::none);

        auto cursorResponse = ClusterFind::runGetMore(txn, getMoreRequest);
        if (cursorResponse == ErrorCodes::CursorNotFound) {
            replyToQuery(ResultFlag_CursorNotFound, request.p(), request.m(), 0, 0, 0);
            return;
        }
        uassertStatusOK(cursorResponse.getStatus());

        // Build the response document.
        //
        // TODO: this constant should be shared between mongos and mongod, and should not be inside
        // ShardedClientCursor.
        BufBuilder buffer(ShardedClientCursor::INIT_REPLY_BUFFER_SIZE);

        int numResults = 0;
        for (const auto& obj : cursorResponse.getValue().batch) {
            buffer.appendBuf((void*)obj.objdata(), obj.objsize());
            ++numResults;
        }

        replyToQuery(0,
                     request.p(),
                     request.m(),
                     buffer.buf(),
                     buffer.len(),
                     numResults,
                     cursorResponse.getValue().numReturnedSoFar.value_or(0),
                     cursorResponse.getValue().cursorId);
        return;
    }

    shared_ptr<DBConfig> config = statusGetDb.getValue();

    ShardPtr primary;
    ChunkManagerPtr info;
    config->getChunkManagerOrPrimary(txn, ns, info, primary);

    //
    // TODO: Cleanup cursor cache, consolidate into single codepath
    //
    const string host = cursorCache.getRef(id);
    ShardedClientCursorPtr cursor = cursorCache.get(id);
    int cursorMaxTimeMS = cursorCache.getMaxTimeMS(id);

    // Cursor ids should not overlap between sharded and unsharded cursors
    massert(17012,
            str::stream() << "duplicate sharded and unsharded cursor id " << id << " detected for "
                          << ns << ", duplicated on host " << host,
            NULL == cursorCache.get(id).get() || host.empty());

    ClientBasic* client = ClientBasic::getCurrent();
    NamespaceString nsString(ns);
    AuthorizationSession* authSession = AuthorizationSession::get(client);
    Status status = authSession->checkAuthForGetMore(nsString, id, false);
    audit::logGetMoreAuthzCheck(client, nsString, id, status.code());
    uassertStatusOK(status);

    if (!host.empty()) {
        LOG(3) << "single getmore: " << ns;

        // we used ScopedDbConnection because we don't get about config versions
        // not deleting data is handled elsewhere
        // and we don't want to call setShardVersion
        ScopedDbConnection conn(host);

        Message response;
        bool ok = conn->callRead(request.m(), response);
        uassert(10204, "dbgrid: getmore: error calling db", ok);

        bool hasMore = (response.singleData().getCursor() != 0);

        if (!hasMore) {
            cursorCache.removeRef(id);
        }

        request.reply(response, "" /*conn->getServerAddress() */);
        conn.done();
        return;
    } else if (cursor) {
        if (cursorMaxTimeMS == kMaxTimeCursorTimeLimitExpired) {
            cursorCache.remove(id);
            uasserted(ErrorCodes::ExceededTimeLimit, "operation exceeded time limit");
        }

        // TODO: Try to match logic of mongod, where on subsequent getMore() we pull lots more data?
        BufBuilder buffer(ShardedClientCursor::INIT_REPLY_BUFFER_SIZE);
        int docCount = 0;
        const int startFrom = cursor->getTotalSent();
        bool hasMore = cursor->sendNextBatch(ntoreturn, buffer, docCount);

        if (hasMore) {
            // still more data
            cursor->accessed();

            if (cursorMaxTimeMS != kMaxTimeCursorNoTimeLimit) {
                // Update remaining amount of time in cursor cache.
                int cursorLeftoverMillis = cursorMaxTimeMS - getMoreTimer.millis();
                if (cursorLeftoverMillis <= 0) {
                    cursorLeftoverMillis = kMaxTimeCursorTimeLimitExpired;
                }
                cursorCache.updateMaxTimeMS(id, cursorLeftoverMillis);
            }
        } else {
            // we've exhausted the cursor
            cursorCache.remove(id);
        }

        replyToQuery(0,
                     request.p(),
                     request.m(),
                     buffer.buf(),
                     buffer.len(),
                     docCount,
                     startFrom,
                     hasMore ? cursor->getId() : 0);
        return;
    } else {
        LOG(3) << "could not find cursor " << id << " in cache for " << ns;

        replyToQuery(ResultFlag_CursorNotFound, request.p(), request.m(), 0, 0, 0);
        return;
    }
}

void Strategy::killCursors(OperationContext* txn, Request& request) {
    if (!useClusterClientCursor) {
        cursorCache.gotKillCursors(request.m());
        return;
    }

    DbMessage& dbMessage = request.d();
    const int numCursors = dbMessage.pullInt();
    massert(28793,
            str::stream() << "Invalid killCursors message. numCursors: " << numCursors
                          << ", message size: " << dbMessage.msg().dataSize() << ".",
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
            Command::runAgainstRegistered(txn, cmdNS.c_str(), requestBSON, builder, 0);
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
}
