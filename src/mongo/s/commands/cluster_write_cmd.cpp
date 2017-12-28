/**
 *    Copyright (C) 2013 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/owned_pointer_vector.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/write_commands/write_commands_common.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/stats/counters.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_last_error_info.h"
#include "mongo/s/commands/chunk_manager_targeter.h"
#include "mongo/s/commands/cluster_explain.h"
#include "mongo/s/commands/cluster_write.h"
#include "mongo/s/grid.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace {

void batchErrorToLastError(const BatchedCommandRequest& request,
                           const BatchedCommandResponse& response,
                           LastError* error) {
    error->reset();

    std::unique_ptr<WriteErrorDetail> commandError;
    WriteErrorDetail* lastBatchError = NULL;

    if (!response.getOk()) {
        // Command-level error, all writes failed
        commandError.reset(new WriteErrorDetail);
        commandError->setErrCode(response.getErrCode());
        commandError->setErrMessage(response.getErrMessage());

        lastBatchError = commandError.get();
    } else if (response.isErrDetailsSet()) {
        // The last error in the batch is always reported - this matches expected COE semantics for
        // insert batches. For updates and deletes, error is only reported if the error was on the
        // last item.
        const bool lastOpErrored = response.getErrDetails().back()->getIndex() ==
            static_cast<int>(request.sizeWriteOps() - 1);
        if (request.getBatchType() == BatchedCommandRequest::BatchType_Insert || lastOpErrored) {
            lastBatchError = response.getErrDetails().back();
        }
    } else {
        // We don't care about write concern errors, these happen in legacy mode in GLE.
    }

    // Record an error if one exists
    if (lastBatchError) {
        const auto& errMsg = lastBatchError->getErrMessage();
        error->setLastError(lastBatchError->getErrCode(),
                            errMsg.empty() ? "see code for details" : errMsg);
        return;
    }

    // Record write stats otherwise
    //
    // NOTE: For multi-write batches, our semantics change a little because we don't have
    // un-aggregated "n" stats
    if (request.getBatchType() == BatchedCommandRequest::BatchType_Update) {
        BSONObj upsertedId;
        if (response.isUpsertDetailsSet()) {
            // Only report the very last item's upserted id if applicable
            if (response.getUpsertDetails().back()->getIndex() + 1 ==
                static_cast<int>(request.sizeWriteOps())) {
                upsertedId = response.getUpsertDetails().back()->getUpsertedID();
            }
        }

        const int numUpserted = response.isUpsertDetailsSet() ? response.sizeUpsertDetails() : 0;
        const int numMatched = response.getN() - numUpserted;
        invariant(numMatched >= 0);

        // Wrap upserted id in "upserted" field
        BSONObj leUpsertedId;
        if (!upsertedId.isEmpty()) {
            leUpsertedId = upsertedId.firstElement().wrap(kUpsertedFieldName);
        }

        error->recordUpdate(numMatched > 0, response.getN(), leUpsertedId);
    } else if (request.getBatchType() == BatchedCommandRequest::BatchType_Delete) {
        error->recordDelete(response.getN());
    }
}

BatchedCommandRequest parseRequest(BatchedCommandRequest::BatchType type,
                                   const OpMsgRequest& request) {
    switch (type) {
        case BatchedCommandRequest::BatchType_Insert:
            return BatchedCommandRequest::cloneInsertWithIds(
                BatchedCommandRequest::parseInsert(request));
        case BatchedCommandRequest::BatchType_Update:
            return BatchedCommandRequest::parseUpdate(request);
        case BatchedCommandRequest::BatchType_Delete:
            return BatchedCommandRequest::parseDelete(request);
    }
    MONGO_UNREACHABLE;
}

/**
 * Base class for mongos write commands.
 */
class ClusterWriteCmd : public Command {
public:
    virtual ~ClusterWriteCmd() {}

    bool slaveOk() const final {
        return false;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const final {
        return true;
    }

    Status checkAuthForRequest(OperationContext* opCtx, const OpMsgRequest& request) final {
        Status status = auth::checkAuthForWriteCommand(
            AuthorizationSession::get(opCtx->getClient()), _writeType, request);

        // TODO: Remove this when we standardize GLE reporting from commands
        if (!status.isOK()) {
            LastError::get(opCtx->getClient()).setLastError(status.code(), status.reason());
        }

        return status;
    }

    Status explain(OperationContext* opCtx,
                   const std::string& dbname,
                   const BSONObj& cmdObj,
                   ExplainOptions::Verbosity verbosity,
                   BSONObjBuilder* out) const final {
        OpMsgRequest request;
        request.body = cmdObj;
        invariant(request.getDatabase() == dbname);  // Ensured by explain command's run() method.

        const auto batchedRequest(parseRequest(_writeType, request));

        // We can only explain write batches of size 1.
        if (batchedRequest.sizeWriteOps() != 1U) {
            return Status(ErrorCodes::InvalidLength, "explained write batches must be of size 1");
        }

        const auto explainCmd = ClusterExplain::wrapAsExplain(cmdObj, verbosity);

        // We will time how long it takes to run the commands on the shards.
        Timer timer;

        // Target the command to the shards based on the singleton batch item.
        BatchItemRef targetingBatchItem(&batchedRequest, 0);
        std::vector<Strategy::CommandResult> shardResults;
        Status status =
            _commandOpWrite(opCtx, dbname, explainCmd, targetingBatchItem, &shardResults);
        if (!status.isOK()) {
            return status;
        }

        return ClusterExplain::buildExplainResult(
            opCtx, shardResults, ClusterExplain::kWriteOnShards, timer.millis(), out);
    }

    bool enhancedRun(OperationContext* opCtx,
                     const OpMsgRequest& request,
                     BSONObjBuilder& result) final {
        const auto batchedRequest(parseRequest(_writeType, request));

        BatchWriteExecStats stats;
        BatchedCommandResponse response;
        ClusterWriter::write(opCtx, batchedRequest, &stats, &response);

        // Populate the lastError object based on the write response
        batchErrorToLastError(batchedRequest, response, &LastError::get(opCtx->getClient()));

        size_t numAttempts;

        if (!response.getOk()) {
            numAttempts = 0;
        } else if (batchedRequest.getWriteCommandBase().getOrdered() &&
                   response.isErrDetailsSet()) {
            // Add one failed attempt
            numAttempts = response.getErrDetailsAt(0)->getIndex() + 1;
        } else {
            numAttempts = batchedRequest.sizeWriteOps();
        }

        // TODO: increase opcounters by more than one
        if (_writeType == BatchedCommandRequest::BatchType_Insert) {
            for (size_t i = 0; i < numAttempts; ++i) {
                globalOpCounters.gotInsert();
            }
        } else if (_writeType == BatchedCommandRequest::BatchType_Update) {
            for (size_t i = 0; i < numAttempts; ++i) {
                globalOpCounters.gotUpdate();
            }
        } else if (_writeType == BatchedCommandRequest::BatchType_Delete) {
            for (size_t i = 0; i < numAttempts; ++i) {
                globalOpCounters.gotDelete();
            }
        }

        // Save the last opTimes written on each shard for this client, to allow GLE to work
        ClusterLastErrorInfo::get(opCtx->getClient())->addHostOpTimes(stats.getWriteOpTimes());

        result.appendElements(response.toBSON());
        return response.getOk();
    }

protected:
    /**
     * Instantiates a command that can be invoked by "name", which will be capable of issuing
     * write batches of type "writeType", and will require privilege "action" to run.
     */
    ClusterWriteCmd(StringData name, BatchedCommandRequest::BatchType writeType)
        : Command(name), _writeType(writeType) {}

private:
    // Type of batch (e.g. insert, update).
    const BatchedCommandRequest::BatchType _writeType;

    /**
     * Executes a write command against a particular database, and targets the command based on
     * a write operation.
     *
     * Does *not* retry or retarget if the metadata is stale.
     */
    static Status _commandOpWrite(OperationContext* opCtx,
                                  const std::string& dbName,
                                  const BSONObj& command,
                                  BatchItemRef targetingBatchItem,
                                  std::vector<Strategy::CommandResult>* results) {
        // Note that this implementation will not handle targeting retries and does not completely
        // emulate write behavior
        TargeterStats stats;
        ChunkManagerTargeter targeter(targetingBatchItem.getRequest()->getTargetingNS(), &stats);
        Status status = targeter.init(opCtx);
        if (!status.isOK())
            return status;

        auto swEndpoints = [&]() -> StatusWith<std::vector<ShardEndpoint>> {
            if (targetingBatchItem.getOpType() == BatchedCommandRequest::BatchType_Insert) {
                auto swEndpoint = targeter.targetInsert(opCtx, targetingBatchItem.getDocument());
                if (!swEndpoint.isOK())
                    return swEndpoint.getStatus();
                return std::vector<ShardEndpoint>{std::move(swEndpoint.getValue())};
            } else if (targetingBatchItem.getOpType() == BatchedCommandRequest::BatchType_Update) {
                return targeter.targetUpdate(opCtx, targetingBatchItem.getUpdate());
            } else if (targetingBatchItem.getOpType() == BatchedCommandRequest::BatchType_Delete) {
                return targeter.targetDelete(opCtx, targetingBatchItem.getDelete());
            } else {
                MONGO_UNREACHABLE;
            }
        }();

        if (!swEndpoints.isOK())
            return swEndpoints.getStatus();

        // Assemble requests
        std::vector<AsyncRequestsSender::Request> requests;
        for (const auto& endpoint : swEndpoints.getValue()) {
            requests.emplace_back(endpoint.shardName, command);
        }

        // Send the requests.

        const ReadPreferenceSetting readPref(ReadPreference::PrimaryOnly, TagSet());
        AsyncRequestsSender ars(opCtx,
                                Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                                dbName,
                                requests,
                                readPref,
                                Shard::RetryPolicy::kNoRetry);

        // Receive the responses.

        Status dispatchStatus = Status::OK();
        while (!ars.done()) {
            // Block until a response is available.
            auto response = ars.next();

            if (!response.swResponse.isOK()) {
                dispatchStatus = std::move(response.swResponse.getStatus());
                break;
            }

            Strategy::CommandResult result;

            // If the response status was OK, the response must contain which host was targeted.
            invariant(response.shardHostAndPort);
            result.target = ConnectionString(std::move(*response.shardHostAndPort));

            result.shardTargetId = std::move(response.shardId);
            result.result = std::move(response.swResponse.getValue().data);

            results->push_back(result);
        }

        return dispatchStatus;
    }
};

class ClusterCmdInsert : public ClusterWriteCmd {
public:
    ClusterCmdInsert() : ClusterWriteCmd("insert", BatchedCommandRequest::BatchType_Insert) {}

    void help(std::stringstream& help) const {
        help << "insert documents";
    }

} clusterInsertCmd;

class ClusterCmdUpdate : public ClusterWriteCmd {
public:
    ClusterCmdUpdate() : ClusterWriteCmd("update", BatchedCommandRequest::BatchType_Update) {}

    void help(std::stringstream& help) const {
        help << "update documents";
    }

} clusterUpdateCmd;

class ClusterCmdDelete : public ClusterWriteCmd {
public:
    ClusterCmdDelete() : ClusterWriteCmd("delete", BatchedCommandRequest::BatchType_Delete) {}

    void help(std::stringstream& help) const {
        help << "delete documents";
    }

} clusterDeleteCmd;

}  // namespace
}  // namespace mongo
