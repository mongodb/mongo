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
#include "mongo/db/query/cursor_response.h"
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

namespace mongo {

namespace {

const char kTermField[] = "term";

}  // namespace

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

    Operation getLogicalOp() const override {
        return dbQuery;
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
        NamespaceString nss(parseNs(dbname, cmdObj));
        auto hasTerm = cmdObj.hasField(kTermField);
        return AuthorizationSession::get(client)->checkAuthForFind(nss, hasTerm);
    }

    Status explain(OperationContext* txn,
                   const std::string& dbname,
                   const BSONObj& cmdObj,
                   ExplainCommon::Verbosity verbosity,
                   const rpc::ServerSelectionMetadata&,
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

        ExtensionsCallbackReal extensionsCallback(txn, &nss);
        auto statusWithCQ =
            CanonicalQuery::canonicalize(lpqStatus.getValue().release(), extensionsCallback);
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
     *   --Parsing.
     *   --Acquire locks.
     *   --Plan query, obtaining an executor that can run it.
     *   --Generate the first batch.
     *   --Save state for getMore, transferring ownership of the executor to a ClientCursor.
     *   --Generate response to send to the client.
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

        // Parse the command BSON to a LiteParsedQuery.
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
        //
        // We pass negative values for 'ntoreturn' and 'ntoskip' to indicate that these values
        // should be omitted from the log line. Limit and skip information is already present in the
        // find command parameters, so these fields are redundant.
        const int ntoreturn = -1;
        const int ntoskip = -1;
        beginQueryOp(txn, nss, cmdObj, ntoreturn, ntoskip);

        // Finish the parsing step by using the LiteParsedQuery to create a CanonicalQuery.
        ExtensionsCallbackReal extensionsCallback(txn, &nss);
        auto statusWithCQ = CanonicalQuery::canonicalize(lpq.release(), extensionsCallback);
        if (!statusWithCQ.isOK()) {
            return appendCommandStatus(result, statusWithCQ.getStatus());
        }
        std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

        ShardingState* const shardingState = ShardingState::get(txn);

        if (OperationShardVersion::get(txn).hasShardVersion() && shardingState->enabled()) {
            ChunkVersion receivedVersion = OperationShardVersion::get(txn).getShardVersion(nss);
            ChunkVersion latestVersion;
            // Wait for migration completion to get the correct chunk version.
            const int maxTimeoutSec = 30;
            int timeoutSec = cq->getParsed().getMaxTimeMS() / 1000;
            if (!timeoutSec || timeoutSec > maxTimeoutSec) {
                timeoutSec = maxTimeoutSec;
            }

            if (!shardingState->waitTillNotInCriticalSection(timeoutSec)) {
                uasserted(ErrorCodes::LockTimeout, "Timeout while waiting for migration commit");
            }

            // If the received version is newer than the version cached in 'shardingState', then we
            // have to refresh 'shardingState' from the config servers. We do this before acquiring
            // locks so that we don't hold locks while waiting on the network.
            uassertStatusOK(shardingState->refreshMetadataIfNeeded(
                txn, nss.ns(), receivedVersion, &latestVersion));
        }

        // Acquire locks.
        AutoGetCollectionForRead ctx(txn, nss);
        Collection* collection = ctx.getCollection();

        const int dbProfilingLevel =
            ctx.getDb() ? ctx.getDb()->getProfilingLevel() : serverGlobalParams.defaultProfile;

        // It is possible that the sharding version will change during yield while we are
        // retrieving a plan executor. If this happens we will throw an error and mongos will
        // retry.
        const ChunkVersion shardingVersionAtStart = shardingState->getVersion(nss.ns());

        // Get the execution plan for the query.
        auto statusWithPlanExecutor =
            getExecutorFind(txn, collection, nss, std::move(cq), PlanExecutor::YIELD_AUTO);
        if (!statusWithPlanExecutor.isOK()) {
            return appendCommandStatus(result, statusWithPlanExecutor.getStatus());
        }

        std::unique_ptr<PlanExecutor> exec = std::move(statusWithPlanExecutor.getValue());

        if (!collection) {
            // No collection. Just fill out curop indicating that there were zero results and
            // there is no ClientCursor id, and then return.
            const long long numResults = 0;
            const CursorId cursorId = 0;
            endQueryOp(txn, collection, *exec, dbProfilingLevel, numResults, cursorId);
            appendCursorResponseObject(cursorId, nss.ns(), BSONArray(), &result);
            return true;
        }

        const LiteParsedQuery& pq = exec->getCanonicalQuery()->getParsed();

        // Stream query results, adding them to a BSONArray as we go.
        BSONArrayBuilder firstBatch;
        BSONObj obj;
        PlanExecutor::ExecState state = PlanExecutor::ADVANCED;
        long long numResults = 0;
        while (!FindCommon::enoughForFirstBatch(pq, numResults, firstBatch.len()) &&
               PlanExecutor::ADVANCED == (state = exec->getNext(&obj, NULL))) {
            // If adding this object will cause us to exceed the BSON size limit, then we stash
            // it for later.
            if (firstBatch.len() + obj.objsize() > BSONObjMaxUserSize && numResults > 0) {
                exec->enqueue(obj);
                break;
            }

            // Add result to output buffer.
            firstBatch.append(obj);
            numResults++;
        }

        // Throw an assertion if query execution fails for any reason.
        if (PlanExecutor::FAILURE == state || PlanExecutor::DEAD == state) {
            const std::unique_ptr<PlanStageStats> stats(exec->getStats());
            error() << "Plan executor error during find command: " << PlanExecutor::statestr(state)
                    << ", stats: " << Explain::statsToBSON(*stats);

            return appendCommandStatus(result,
                                       Status(ErrorCodes::OperationFailed,
                                              str::stream()
                                                  << "Executor error during find command: "
                                                  << WorkingSetCommon::toStatusString(obj)));
        }

        // TODO: Currently, chunk ranges are kept around until all ClientCursors created while the
        // chunk belonged on this node are gone. Separating chunk lifetime management from
        // ClientCursor should allow this check to go away.
        if (!shardingState->getVersion(nss.ns()).isWriteCompatibleWith(shardingVersionAtStart)) {
            // Version changed while retrieving a PlanExecutor. Terminate the operation,
            // signaling that mongos should retry.
            throw SendStaleConfigException(nss.ns(),
                                           "version changed during find command",
                                           shardingVersionAtStart,
                                           shardingState->getVersion(nss.ns()));
        }

        // Set up the cursor for getMore.
        CursorId cursorId = 0;
        if (shouldSaveCursor(txn, collection, state, exec.get())) {
            // Register the execution plan inside a ClientCursor. Ownership of the PlanExecutor is
            // transferred to the ClientCursor.
            //
            // First unregister the PlanExecutor so it can be re-registered with ClientCursor.
            exec->deregisterExec();

            // Create a ClientCursor containing this plan executor. We don't have to worry about
            // leaking it as it's inserted into a global map by its ctor.
            ClientCursor* cursor =
                new ClientCursor(collection->getCursorManager(),
                                 exec.release(),
                                 nss.ns(),
                                 txn->recoveryUnit()->isReadingFromMajorityCommittedSnapshot(),
                                 pq.getOptions(),
                                 pq.getFilter());
            cursorId = cursor->cursorid();

            invariant(!exec);
            PlanExecutor* cursorExec = cursor->getExecutor();

            // State will be restored on getMore.
            cursorExec->saveState();
            cursorExec->detachFromOperationContext();

            cursor->setLeftoverMaxTimeMicros(CurOp::get(txn)->getRemainingMaxTimeMicros());
            cursor->setPos(numResults);

            // Fill out curop based on the results.
            endQueryOp(txn, collection, *cursorExec, dbProfilingLevel, numResults, cursorId);
        } else {
            endQueryOp(txn, collection, *exec, dbProfilingLevel, numResults, cursorId);
        }

        // Generate the response object to send to the client.
        appendCursorResponseObject(cursorId, nss.ns(), firstBatch.arr(), &result);
        return true;
    }

} findCmd;

}  // namespace mongo
