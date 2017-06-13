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
#include "mongo/s/write_ops/batch_upconvert.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/timer.h"

namespace mongo {

using std::string;
using std::stringstream;
using std::vector;

namespace {

/**
 * Base class for mongos write commands.  Cluster write commands support batch writes and write
 * concern, and return per-item error information.  All cluster write commands use the entry
 * point ClusterWriteCmd::run().
 *
 * Batch execution (targeting and dispatching) is performed by the BatchWriteExec class.
 */
class ClusterWriteCmd : public Command {
public:
    virtual ~ClusterWriteCmd() {}

    virtual bool slaveOk() const {
        return false;
    }


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        Status status = auth::checkAuthForWriteCommand(AuthorizationSession::get(client),
                                                       _writeType,
                                                       NamespaceString(parseNs(dbname, cmdObj)),
                                                       cmdObj);

        // TODO: Remove this when we standardize GLE reporting from commands
        if (!status.isOK()) {
            LastError::get(client).setLastError(status.code(), status.reason());
        }

        return status;
    }

    virtual Status explain(OperationContext* opCtx,
                           const std::string& dbname,
                           const BSONObj& cmdObj,
                           ExplainOptions::Verbosity verbosity,
                           BSONObjBuilder* out) const {
        BatchedCommandRequest batchedRequest(_writeType);
        OpMsgRequest request;
        request.body = cmdObj;
        invariant(request.getDatabase() == dbname);  // Ensured by explain command's run() method.
        batchedRequest.parseRequest(request);

        std::string errMsg;
        if (!batchedRequest.isValid(&errMsg)) {
            return Status(ErrorCodes::FailedToParse, errMsg);
        }

        // We can only explain write batches of size 1.
        if (batchedRequest.sizeWriteOps() != 1U) {
            return Status(ErrorCodes::InvalidLength, "explained write batches must be of size 1");
        }

        const auto explainCmd = ClusterExplain::wrapAsExplain(cmdObj, verbosity);

        // We will time how long it takes to run the commands on the shards.
        Timer timer;

        // Target the command to the shards based on the singleton batch item.
        BatchItemRef targetingBatchItem(&batchedRequest, 0);
        vector<Strategy::CommandResult> shardResults;
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
                     string& errmsg,
                     BSONObjBuilder& result) final {
        BatchedCommandRequest batchedRequest(_writeType);
        BatchedCommandResponse response;

        ClusterWriter writer(true, 0);

        LastError* cmdLastError = &LastError::get(cc());

        {
            // Disable the last error object for the duration of the write
            LastError::Disabled disableLastError(cmdLastError);
            batchedRequest.parseRequest(request);
            if (!batchedRequest.isValid(&errmsg)) {
                // Batch parse failure
                response.setOk(false);
                response.setErrCode(ErrorCodes::FailedToParse);
                response.setErrMessage(errmsg);
            } else {
                writer.write(opCtx, batchedRequest, &response);
            }

            dassert(response.isValid(NULL));
        }

        // Populate the lastError object based on the write response
        cmdLastError->reset();
        batchErrorToLastError(batchedRequest, response, cmdLastError);

        size_t numAttempts;

        if (!response.getOk()) {
            numAttempts = 0;
        } else if (batchedRequest.getOrdered() && response.isErrDetailsSet()) {
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
        if (haveClient()) {
            ClusterLastErrorInfo::get(opCtx->getClient())
                ->addHostOpTimes(writer.getStats().getWriteOpTimes());
        }

        // TODO
        // There's a pending issue about how to report response here. If we use
        // the command infra-structure, we should reuse the 'errmsg' field. But
        // we have already filed that message inside the BatchCommandResponse.
        // return response.getOk();
        result.appendElements(response.toBSON());
        return true;
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
        ChunkManagerTargeter targeter(targetingBatchItem.getRequest()->getTargetingNSS(), &stats);
        Status status = targeter.init(opCtx);
        if (!status.isOK())
            return status;

        vector<std::unique_ptr<ShardEndpoint>> endpoints;

        if (targetingBatchItem.getOpType() == BatchedCommandRequest::BatchType_Insert) {
            ShardEndpoint* endpoint;
            Status status =
                targeter.targetInsert(opCtx, targetingBatchItem.getDocument(), &endpoint);
            if (!status.isOK())
                return status;
            endpoints.push_back(std::unique_ptr<ShardEndpoint>{endpoint});
        } else if (targetingBatchItem.getOpType() == BatchedCommandRequest::BatchType_Update) {
            Status status =
                targeter.targetUpdate(opCtx, *targetingBatchItem.getUpdate(), &endpoints);
            if (!status.isOK())
                return status;
        } else {
            invariant(targetingBatchItem.getOpType() == BatchedCommandRequest::BatchType_Delete);
            Status status =
                targeter.targetDelete(opCtx, *targetingBatchItem.getDelete(), &endpoints);
            if (!status.isOK())
                return status;
        }

        auto shardRegistry = Grid::get(opCtx)->shardRegistry();

        // Assemble requests
        std::vector<AsyncRequestsSender::Request> requests;
        for (auto it = endpoints.begin(); it != endpoints.end(); ++it) {
            const ShardEndpoint* endpoint = it->get();

            auto shardStatus = shardRegistry->getShard(opCtx, endpoint->shardName);
            if (!shardStatus.isOK()) {
                return shardStatus.getStatus();
            }
            requests.emplace_back(shardStatus.getValue()->getId(), command);
        }

        // Send the requests.

        const ReadPreferenceSetting readPref(ReadPreference::PrimaryOnly, TagSet());
        AsyncRequestsSender ars(opCtx,
                                Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                                dbName,
                                requests,
                                readPref);

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

    void help(stringstream& help) const {
        help << "insert documents";
    }

} clusterInsertCmd;

class ClusterCmdUpdate : public ClusterWriteCmd {
public:
    ClusterCmdUpdate() : ClusterWriteCmd("update", BatchedCommandRequest::BatchType_Update) {}

    void help(stringstream& help) const {
        help << "update documents";
    }

} clusterUpdateCmd;

class ClusterCmdDelete : public ClusterWriteCmd {
public:
    ClusterCmdDelete() : ClusterWriteCmd("delete", BatchedCommandRequest::BatchType_Delete) {}

    void help(stringstream& help) const {
        help << "delete documents";
    }

} clusterDeleteCmd;

}  // namespace
}  // namespace mongo
