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
#include "mongo/db/client.h"
#include "mongo/db/client_basic.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/write_commands/write_commands_common.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/counters.h"
#include "mongo/s/chunk_manager_targeter.h"
#include "mongo/s/client/dbclient_multi_command.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_last_error_info.h"
#include "mongo/s/cluster_write.h"
#include "mongo/s/commands/cluster_explain.h"
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

    virtual Status checkAuthForCommand(ClientBasic* client,
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

    virtual Status explain(OperationContext* txn,
                           const std::string& dbname,
                           const BSONObj& cmdObj,
                           ExplainCommon::Verbosity verbosity,
                           const rpc::ServerSelectionMetadata& serverSelectionMetadata,
                           BSONObjBuilder* out) const {
        BatchedCommandRequest request(_writeType);

        string errMsg;
        if (!request.parseBSON(dbname, cmdObj, &errMsg) || !request.isValid(&errMsg)) {
            return Status(ErrorCodes::FailedToParse, errMsg);
        }

        // We can only explain write batches of size 1.
        if (request.sizeWriteOps() != 1U) {
            return Status(ErrorCodes::InvalidLength, "explained write batches must be of size 1");
        }

        BSONObjBuilder explainCmdBob;
        int options = 0;
        ClusterExplain::wrapAsExplain(
            cmdObj, verbosity, serverSelectionMetadata, &explainCmdBob, &options);

        // We will time how long it takes to run the commands on the shards.
        Timer timer;

        // Target the command to the shards based on the singleton batch item.
        BatchItemRef targetingBatchItem(&request, 0);
        vector<Strategy::CommandResult> shardResults;
        Status status =
            _commandOpWrite(txn, dbname, explainCmdBob.obj(), targetingBatchItem, &shardResults);
        if (!status.isOK()) {
            return status;
        }

        return ClusterExplain::buildExplainResult(
            txn, shardResults, ClusterExplain::kWriteOnShards, timer.millis(), out);
    }

    virtual bool run(OperationContext* txn,
                     const string& dbname,
                     BSONObj& cmdObj,
                     int options,
                     string& errmsg,
                     BSONObjBuilder& result) {
        BatchedCommandRequest request(_writeType);
        BatchedCommandResponse response;

        ClusterWriter writer(true, 0);

        LastError* cmdLastError = &LastError::get(cc());

        {
            // Disable the last error object for the duration of the write
            LastError::Disabled disableLastError(cmdLastError);

            // TODO: if we do namespace parsing, push this to the type
            if (!request.parseBSON(dbname, cmdObj, &errmsg) || !request.isValid(&errmsg)) {
                // Batch parse failure
                response.setOk(false);
                response.setErrCode(ErrorCodes::FailedToParse);
                response.setErrMessage(errmsg);
            } else {
                writer.write(txn, request, &response);
            }

            dassert(response.isValid(NULL));
        }

        // Populate the lastError object based on the write response
        cmdLastError->reset();
        batchErrorToLastError(request, response, cmdLastError);

        size_t numAttempts;

        if (!response.getOk()) {
            numAttempts = 0;
        } else if (request.getOrdered() && response.isErrDetailsSet()) {
            // Add one failed attempt
            numAttempts = response.getErrDetailsAt(0)->getIndex() + 1;
        } else {
            numAttempts = request.sizeWriteOps();
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
            ClusterLastErrorInfo::get(cc()).addHostOpTimes(writer.getStats().getWriteOpTimes());
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
    static Status _commandOpWrite(OperationContext* txn,
                                  const std::string& dbName,
                                  const BSONObj& command,
                                  BatchItemRef targetingBatchItem,
                                  std::vector<Strategy::CommandResult>* results) {
        // Note that this implementation will not handle targeting retries and does not completely
        // emulate write behavior
        TargeterStats stats;
        ChunkManagerTargeter targeter(
            NamespaceString(targetingBatchItem.getRequest()->getTargetingNS()), &stats);
        Status status = targeter.init(txn);
        if (!status.isOK())
            return status;

        OwnedPointerVector<ShardEndpoint> endpointsOwned;
        vector<ShardEndpoint*>& endpoints = endpointsOwned.mutableVector();

        if (targetingBatchItem.getOpType() == BatchedCommandRequest::BatchType_Insert) {
            ShardEndpoint* endpoint;
            Status status = targeter.targetInsert(txn, targetingBatchItem.getDocument(), &endpoint);
            if (!status.isOK())
                return status;
            endpoints.push_back(endpoint);
        } else if (targetingBatchItem.getOpType() == BatchedCommandRequest::BatchType_Update) {
            Status status = targeter.targetUpdate(txn, *targetingBatchItem.getUpdate(), &endpoints);
            if (!status.isOK())
                return status;
        } else {
            invariant(targetingBatchItem.getOpType() == BatchedCommandRequest::BatchType_Delete);
            Status status = targeter.targetDelete(txn, *targetingBatchItem.getDelete(), &endpoints);
            if (!status.isOK())
                return status;
        }

        DBClientMultiCommand dispatcher;

        // Assemble requests
        for (vector<ShardEndpoint*>::const_iterator it = endpoints.begin(); it != endpoints.end();
             ++it) {
            const ShardEndpoint* endpoint = *it;

            const ReadPreferenceSetting readPref(ReadPreference::PrimaryOnly, TagSet());
            auto shard = grid.shardRegistry()->getShard(txn, endpoint->shardName);
            if (!shard) {
                return Status(ErrorCodes::ShardNotFound,
                              "Could not find shard with id " + endpoint->shardName.toString());
            }
            auto swHostAndPort = shard->getTargeter()->findHost(readPref);
            if (!swHostAndPort.isOK()) {
                return swHostAndPort.getStatus();
            }

            ConnectionString host(swHostAndPort.getValue());
            dispatcher.addCommand(host, dbName, command);
        }

        // Errors reported when recv'ing responses
        dispatcher.sendAll();
        Status dispatchStatus = Status::OK();

        // Recv responses
        while (dispatcher.numPending() > 0) {
            ConnectionString host;
            RawBSONSerializable response;

            Status status = dispatcher.recvAny(&host, &response);
            if (!status.isOK()) {
                // We always need to recv() all the sent operations
                dispatchStatus = status;
                continue;
            }

            Strategy::CommandResult result;
            result.target = host;
            {
                const auto shard = grid.shardRegistry()->getShard(txn, host.toString());
                result.shardTargetId = shard->getId();
            }
            result.result = response.toBSON();

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
