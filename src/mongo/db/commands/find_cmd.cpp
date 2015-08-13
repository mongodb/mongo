/**
 *    Copyright (C) 2014 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/service_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/query/cursor_responses.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/find.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/s/operation_shard_version.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/stats/counters.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

/**
 * A command for running .find() queries.
 */
class FindCmd : public Command {
    MONGO_DISALLOW_COPYING(FindCmd);

public:
    FindCmd() : Command("find") {}

    bool isWriteCommandForConfigServer() const override {
        return false;
    }

    bool slaveOk() const override {
        return false;
    }

    bool slaveOverrideOk() const override {
        return true;
    }

    bool maintenanceOk() const override {
        return false;
    }

    bool adminOnly() const override {
        return false;
    }

    bool supportsReadConcern() const final {
        return true;
    }

    void help(std::stringstream& help) const override {
        help << "query for documents";
    }

    /**
     * A find command does not increment the command counter, but rather increments the
     * query counter.
     */
    bool shouldAffectCommandCounter() const override {
        return false;
    }

    Status checkAuthForCommand(ClientBasic* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) override {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);
        ResourcePattern pattern = parseResourcePattern(dbname, cmdObj);

        if (authzSession->isAuthorizedForActionsOnResource(pattern, ActionType::find)) {
            return Status::OK();
        }

        return Status(ErrorCodes::Unauthorized, "unauthorized");
    }

    Status explain(OperationContext* txn,
                   const std::string& dbname,
                   const BSONObj& cmdObj,
                   ExplainCommon::Verbosity verbosity,
                   BSONObjBuilder* out) const override {
        const std::string fullns = parseNs(dbname, cmdObj);
        const NamespaceString nss(fullns);
        if (!nss.isValid()) {
            return {ErrorCodes::InvalidNamespace,
                    str::stream() << "Invalid collection name: " << nss.ns()};
        }

        // Parse the command BSON to a LiteParsedQuery.
        const bool isExplain = true;
        auto lpqStatus = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
        if (!lpqStatus.isOK()) {
            return lpqStatus.getStatus();
        }

        // Finish the parsing step by using the LiteParsedQuery to create a CanonicalQuery.

        WhereCallbackReal whereCallback(txn, nss.db());
        auto statusWithCQ =
            CanonicalQuery::canonicalize(lpqStatus.getValue().release(), whereCallback);
        if (!statusWithCQ.isOK()) {
            return statusWithCQ.getStatus();
        }
        std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

        AutoGetCollectionForRead ctx(txn, nss);
        // The collection may be NULL. If so, getExecutor() should handle it by returning
        // an execution tree with an EOFStage.
        Collection* collection = ctx.getCollection();

        // We have a parsed query. Time to get the execution plan for it.
        auto statusWithPlanExecutor =
            getExecutorFind(txn, collection, nss, std::move(cq), PlanExecutor::YIELD_AUTO);
        if (!statusWithPlanExecutor.isOK()) {
            return statusWithPlanExecutor.getStatus();
        }
        std::unique_ptr<PlanExecutor> exec = std::move(statusWithPlanExecutor.getValue());

        // Got the execution tree. Explain it.
        Explain::explainStages(exec.get(), verbosity, out);
        return Status::OK();
    }

    /**
     * Runs a query using the following steps:
     *   1) Parsing.
     *   2) Acquire locks.
     *   3) Plan query, obtaining an executor that can run it.
     *   4) Setup a cursor for the query, which may be used on subsequent getMores.
     *   5) Generate the first batch.
     *   6) Save state for getMore.
     *   7) Generate response to send to the client.
     */
    bool run(OperationContext* txn,
             const std::string& dbname,
             BSONObj& cmdObj,
             int options,
             std::string& errmsg,
             BSONObjBuilder& result) override {
        const std::string fullns = parseNs(dbname, cmdObj);
        const NamespaceString nss(fullns);
        if (!nss.isValid() || nss.isCommand() || nss.isSpecialCommand()) {
            return appendCommandStatus(result,
                                       {ErrorCodes::InvalidNamespace,
                                        str::stream() << "Invalid collection name: " << nss.ns()});
        }

        // Although it is a command, a find command gets counted as a query.
        globalOpCounters.gotQuery();

        if (txn->getClient()->isInDirectClient()) {
            return appendCommandStatus(
                result,
                Status(ErrorCodes::IllegalOperation, "Cannot run find command from eval()"));
        }

        // 1a) Parse the command BSON to a LiteParsedQuery.
        const bool isExplain = false;
        auto lpqStatus = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
        if (!lpqStatus.isOK()) {
            return appendCommandStatus(result, lpqStatus.getStatus());
        }

        auto& lpq = lpqStatus.getValue();

        // Validate term, if provided.
        if (auto term = lpq->getReplicationTerm()) {
            auto replCoord = repl::ReplicationCoordinator::get(txn);
            Status status = replCoord->updateTerm(*term);
            // Note: updateTerm returns ok if term stayed the same.
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }
        }

        // Fill out curop information.
        long long ntoreturn = lpq->getNToReturn().value_or(0);
        beginQueryOp(txn, nss, cmdObj, ntoreturn, lpq->getSkip().value_or(0));

        // 1b) Finish the parsing step by using the LiteParsedQuery to create a CanonicalQuery.
        WhereCallbackReal whereCallback(txn, nss.db());
        auto statusWithCQ = CanonicalQuery::canonicalize(lpq.release(), whereCallback);
        if (!statusWithCQ.isOK()) {
            return appendCommandStatus(result, statusWithCQ.getStatus());
        }
        std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

        // If the received version is newer than the version cached in 'shardingState', then we have
        // to refresh 'shardingState' from the config servers. We do this before acquiring locks so
        // that we don't hold locks while waiting on the network.
        ShardingState* const shardingState = ShardingState::get(txn);
        if (OperationShardVersion::get(txn).hasShardVersion() && shardingState->enabled()) {
            ChunkVersion receivedVersion = OperationShardVersion::get(txn).getShardVersion();
            ChunkVersion latestVersion;
            uassertStatusOK(shardingState->refreshMetadataIfNeeded(
                txn, nss.ns(), receivedVersion, &latestVersion));
        }

        // 2) Acquire locks.
        AutoGetCollectionForRead ctx(txn, nss);
        Collection* collection = ctx.getCollection();

        const int dbProfilingLevel =
            ctx.getDb() ? ctx.getDb()->getProfilingLevel() : serverGlobalParams.defaultProfile;

        // It is possible that the sharding version will change during yield while we are
        // retrieving a plan executor. If this happens we will throw an error and mongos will
        // retry.
        const ChunkVersion shardingVersionAtStart = shardingState->getVersion(nss.ns());

        // 3) Get the execution plan for the query.
        auto statusWithPlanExecutor =
            getExecutorFind(txn, collection, nss, std::move(cq), PlanExecutor::YIELD_AUTO);
        if (!statusWithPlanExecutor.isOK()) {
            return appendCommandStatus(result, statusWithPlanExecutor.getStatus());
        }

        std::unique_ptr<PlanExecutor> exec = std::move(statusWithPlanExecutor.getValue());

        // TODO: Currently, chunk ranges are kept around until all ClientCursors created while
        // the chunk belonged on this node are gone. Separating chunk lifetime management from
        // ClientCursor should allow this check to go away.
        if (!shardingState->getVersion(nss.ns()).isWriteCompatibleWith(shardingVersionAtStart)) {
            // Version changed while retrieving a PlanExecutor. Terminate the operation,
            // signaling that mongos should retry.
            throw SendStaleConfigException(nss.ns(),
                                           "version changed during find command",
                                           shardingVersionAtStart,
                                           shardingState->getVersion(nss.ns()));
        }

        if (!collection) {
            // No collection. Just fill out curop indicating that there were zero results and
            // there is no ClientCursor id, and then return.
            const long long numResults = 0;
            const CursorId cursorId = 0;
            endQueryOp(txn, *exec, dbProfilingLevel, numResults, cursorId);
            appendCursorResponseObject(cursorId, nss.ns(), BSONArray(), &result);
            return true;
        }

        const LiteParsedQuery& pq = exec->getCanonicalQuery()->getParsed();

        // 4) If possible, register the execution plan inside a ClientCursor, and pin that
        // cursor. In this case, ownership of the PlanExecutor is transferred to the
        // ClientCursor, and 'exec' becomes null.
        //
        // First unregister the PlanExecutor so it can be re-registered with ClientCursor.
        exec->deregisterExec();

        // Create a ClientCursor containing this plan executor. We don't have to worry
        // about leaking it as it's inserted into a global map by its ctor.
        ClientCursor* cursor =
            new ClientCursor(collection->getCursorManager(),
                             exec.release(),
                             nss.ns(),
                             txn->recoveryUnit()->isReadingFromMajorityCommittedSnapshot(),
                             pq.getOptions(),
                             pq.getFilter());
        CursorId cursorId = cursor->cursorid();
        ClientCursorPin ccPin(collection->getCursorManager(), cursorId);

        // On early return, get rid of the the cursor.
        ScopeGuard cursorFreer = MakeGuard(&ClientCursorPin::deleteUnderlying, ccPin);

        invariant(!exec);
        PlanExecutor* cursorExec = cursor->getExecutor();

        // 5) Stream query results, adding them to a BSONArray as we go.
        BSONArrayBuilder firstBatch;
        BSONObj obj;
        PlanExecutor::ExecState state;
        long long numResults = 0;
        while (!FindCommon::enoughForFirstBatch(pq, numResults, firstBatch.len()) &&
               PlanExecutor::ADVANCED == (state = cursorExec->getNext(&obj, NULL))) {
            // If adding this object will cause us to exceed the BSON size limit, then we stash
            // it for later.
            if (firstBatch.len() + obj.objsize() > BSONObjMaxUserSize && numResults > 0) {
                cursorExec->enqueue(obj);
                break;
            }

            // Add result to output buffer.
            firstBatch.append(obj);
            numResults++;
        }

        // Throw an assertion if query execution fails for any reason.
        if (PlanExecutor::FAILURE == state || PlanExecutor::DEAD == state) {
            const std::unique_ptr<PlanStageStats> stats(cursorExec->getStats());
            error() << "Plan executor error during find command: " << PlanExecutor::statestr(state)
                    << ", stats: " << Explain::statsToBSON(*stats);

            return appendCommandStatus(result,
                                       Status(ErrorCodes::OperationFailed,
                                              str::stream()
                                                  << "Executor error during find command: "
                                                  << WorkingSetCommon::toStatusString(obj)));
        }

        // 6) Set up the cursor for getMore.
        if (shouldSaveCursor(txn, collection, state, cursorExec)) {
            // State will be restored on getMore.
            cursorExec->saveState();
            cursorExec->detachFromOperationContext();

            cursor->setLeftoverMaxTimeMicros(CurOp::get(txn)->getRemainingMaxTimeMicros());
            cursor->setPos(numResults);
        } else {
            cursorId = 0;
        }

        // Fill out curop based on the results.
        endQueryOp(txn, *cursorExec, dbProfilingLevel, numResults, cursorId);

        // 7) Generate the response object to send to the client.
        appendCursorResponseObject(cursorId, nss.ns(), firstBatch.arr(), &result);
        if (cursorId) {
            cursorFreer.Dismiss();
        }
        return true;
    }

} findCmd;

}  // namespace mongo
