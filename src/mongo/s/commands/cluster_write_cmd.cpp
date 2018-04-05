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
#include "mongo/db/curop.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/stats/counters.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_last_error_info.h"
#include "mongo/s/commands/cluster_explain.h"
#include "mongo/s/grid.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/s/write_ops/chunk_manager_targeter.h"
#include "mongo/s/write_ops/cluster_write.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace {

void batchErrorToLastError(const BatchedCommandRequest& request,
                           const BatchedCommandResponse& response,
                           LastError* error) {
    error->reset();

    std::unique_ptr<WriteErrorDetail> commandError;
    WriteErrorDetail* lastBatchError = nullptr;

    if (!response.getOk()) {
        // Command-level error, all writes failed
        commandError = stdx::make_unique<WriteErrorDetail>();
        commandError->setStatus(response.getTopLevelStatus());
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
        const auto& errMsg = lastBatchError->toStatus().reason();
        error->setLastError(lastBatchError->toStatus().code(),
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

    std::unique_ptr<CommandInvocation> parse(OperationContext* opCtx,
                                             const OpMsgRequest& request) final;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kNever;
    }

    LogicalOp getLogicalOp() const {
        switch (_writeType) {
            case BatchedCommandRequest::BatchType::BatchType_Insert:
                return LogicalOp::opInsert;
            case BatchedCommandRequest::BatchType::BatchType_Delete:
                return LogicalOp::opDelete;
            case BatchedCommandRequest::BatchType::BatchType_Update:
                return LogicalOp::opUpdate;
        }

        MONGO_UNREACHABLE;
    }

    Status explainImpl(OperationContext* opCtx,
                       const OpMsgRequest& request,
                       ExplainOptions::Verbosity verbosity,
                       BatchedCommandRequest& batchedRequest,
                       BSONObjBuilder* out) const {
        // We can only explain write batches of size 1.
        if (batchedRequest.sizeWriteOps() != 1U) {
            return Status(ErrorCodes::InvalidLength, "explained write batches must be of size 1");
        }

        const auto explainCmd = ClusterExplain::wrapAsExplain(request.body, verbosity);

        // We will time how long it takes to run the commands on the shards.
        Timer timer;

        // Target the command to the shards based on the singleton batch item.
        BatchItemRef targetingBatchItem(&batchedRequest, 0);
        std::vector<Strategy::CommandResult> shardResults;
        Status status = _commandOpWrite(
            opCtx, request.getDatabase().toString(), explainCmd, targetingBatchItem, &shardResults);
        if (!status.isOK()) {
            return status;
        }

        return ClusterExplain::buildExplainResult(
            opCtx, shardResults, ClusterExplain::kWriteOnShards, timer.millis(), out);
    }

    bool runImpl(OperationContext* opCtx,
                 const OpMsgRequest& request,
                 BatchedCommandRequest& batchedRequest,
                 BSONObjBuilder& result) const {
        auto db = batchedRequest.getNS().db();
        if (db != NamespaceString::kAdminDb && db != NamespaceString::kConfigDb) {
            batchedRequest.setAllowImplicitCreate(false);
        }

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
        auto& debug = CurOp::get(opCtx)->debug();
        if (_writeType == BatchedCommandRequest::BatchType_Insert) {
            for (size_t i = 0; i < numAttempts; ++i) {
                globalOpCounters.gotInsert();
            }
            debug.ninserted = response.getN();
        } else if (_writeType == BatchedCommandRequest::BatchType_Update) {
            for (size_t i = 0; i < numAttempts; ++i) {
                globalOpCounters.gotUpdate();
            }
            debug.upsert = response.isUpsertDetailsSet();
            debug.nMatched = response.getN() - (debug.upsert ? response.sizeUpsertDetails() : 0);
            debug.nModified = response.getNModified();
        } else if (_writeType == BatchedCommandRequest::BatchType_Delete) {
            for (size_t i = 0; i < numAttempts; ++i) {
                globalOpCounters.gotDelete();
            }
            debug.ndeleted = response.getN();
        }

        // Save the last opTimes written on each shard for this client, to allow GLE to work
        ClusterLastErrorInfo::get(opCtx->getClient())->addHostOpTimes(stats.getWriteOpTimes());

        // Record the number of shards targeted by this write.
        CurOp::get(opCtx)->debug().nShards = stats.getTargetedShards().size();

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
    class Invocation;

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

    // Type of batch: insert, update, or delete.
    const BatchedCommandRequest::BatchType _writeType;
};

class ClusterWriteCmd::Invocation final : public CommandInvocation {
public:
    Invocation(const ClusterWriteCmd* clusterWriteCmd,
               const OpMsgRequest& request,
               NamespaceString ns,
               BatchedCommandRequest batchedRequest)
        : CommandInvocation(clusterWriteCmd),
          _request{&request},
          _ns{std::move(ns)},
          _batchedRequest{std::move(batchedRequest)} {}

private:
    void run(OperationContext* opCtx, CommandReplyBuilder* result) override {
        try {
            BSONObjBuilder bob = result->getBodyBuilder();
            bool ok = command()->runImpl(opCtx, *_request, _batchedRequest, bob);
            CommandHelpers::appendCommandStatus(bob, ok);
        } catch (const ExceptionFor<ErrorCodes::Unauthorized>&) {
            CommandHelpers::logAuthViolation(opCtx, command(), *_request, ErrorCodes::Unauthorized);
            throw;
        }
    }

    void explain(OperationContext* opCtx,
                 ExplainOptions::Verbosity verbosity,
                 BSONObjBuilder* result) override {
        uassertStatusOK(
            command()->explainImpl(opCtx, *_request, verbosity, _batchedRequest, result));
    }

    NamespaceString ns() const override {
        return _ns;
    }

    bool supportsWriteConcern() const override {
        return true;
    }

    Command::AllowedOnSecondary secondaryAllowed(ServiceContext* context) const override {
        return command()->secondaryAllowed(context);
    }

    void doCheckAuthorization(OperationContext* opCtx) const override {
        try {
            auth::checkAuthForWriteCommand(
                AuthorizationSession::get(opCtx->getClient()), command()->_writeType, *_request);
        } catch (const DBException& e) {
            LastError::get(opCtx->getClient()).setLastError(e.code(), e.reason());
            throw;
        }
    }

    const ClusterWriteCmd* command() const {
        return static_cast<const ClusterWriteCmd*>(definition());
    }

    const OpMsgRequest* _request;
    NamespaceString _ns;
    BatchedCommandRequest _batchedRequest;
};

std::unique_ptr<CommandInvocation> ClusterWriteCmd::parse(OperationContext* opCtx,
                                                          const OpMsgRequest& request) {
    return stdx::make_unique<Invocation>(
        this,
        request,
        NamespaceString(parseNs(request.getDatabase().toString(), request.body)),
        parseRequest(_writeType, request));
}

class ClusterCmdInsert final : public ClusterWriteCmd {
public:
    ClusterCmdInsert() : ClusterWriteCmd("insert", BatchedCommandRequest::BatchType_Insert) {}

    std::string help() const override {
        return "insert documents";
    }

} clusterInsertCmd;

class ClusterCmdUpdate final : public ClusterWriteCmd {
public:
    ClusterCmdUpdate() : ClusterWriteCmd("update", BatchedCommandRequest::BatchType_Update) {}

    std::string help() const override {
        return "update documents";
    }

} clusterUpdateCmd;

class ClusterCmdDelete final : public ClusterWriteCmd {
public:
    ClusterCmdDelete() : ClusterWriteCmd("delete", BatchedCommandRequest::BatchType_Delete) {}

    std::string help() const override {
        return "delete documents";
    }

} clusterDeleteCmd;

}  // namespace
}  // namespace mongo
