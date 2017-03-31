/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/s/commands/scatter_gather_from_shards.h"

#include <list>
#include <set>

#include "mongo/db/jsobj.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/commands/sharded_command_processing.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"

namespace mongo {

using ShardAndReply = std::tuple<ShardId, BSONObj>;

StatusWith<std::vector<ShardAndReply>> gatherResults(
    OperationContext* opCtx,
    const std::string& dbName,
    const BSONObj& cmdObj,
    int options,
    const std::vector<AsyncRequestsSender::Request>& requests,
    BSONObjBuilder* output) {
    // Extract the readPreference from the command.

    rpc::ServerSelectionMetadata ssm;
    BSONObjBuilder unusedCmdBob;
    BSONObjBuilder upconvertedMetadataBob;
    uassertStatusOK(rpc::ServerSelectionMetadata::upconvert(
        cmdObj, options, &unusedCmdBob, &upconvertedMetadataBob));
    auto upconvertedMetadata = upconvertedMetadataBob.obj();
    auto ssmElem = upconvertedMetadata.getField(rpc::ServerSelectionMetadata::fieldName());
    if (!ssmElem.eoo()) {
        ssm = uassertStatusOK(rpc::ServerSelectionMetadata::readFromMetadata(ssmElem));
    }
    auto readPref = ssm.getReadPreference();

    // Send the requests.

    AsyncRequestsSender ars(
        opCtx,
        Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
        dbName,
        requests,
        readPref ? *readPref : ReadPreferenceSetting(ReadPreference::PrimaryOnly, TagSet()));

    // Get the responses.

    std::vector<ShardAndReply> results;                 // Stores results by ShardId
    BSONObjBuilder subobj(output->subobjStart("raw"));  // Stores results by ConnectionString
    BSONObjBuilder errors;                              // Stores errors by ConnectionString
    int commonErrCode = -1;                             // Stores the overall error code

    BSONElement wcErrorElem;
    ShardId wcErrorShardId;
    bool hasWCError = false;

    while (!ars.done()) {
        auto response = ars.next();
        const auto swShard = Grid::get(opCtx)->shardRegistry()->getShard(opCtx, response.shardId);
        if (!swShard.isOK()) {
            output->resetToEmpty();
            return swShard.getStatus();
        }
        const auto shard = std::move(swShard.getValue());

        auto status = response.swResponse.getStatus();
        if (status.isOK()) {
            // We successfully received a response.

            status = getStatusFromCommandResult(response.swResponse.getValue().data);
            if (ErrorCodes::isStaleShardingError(status.code())) {
                // Do not report any raw results if we fail to establish a shardVersion.
                output->resetToEmpty();
                return status;
            }

            auto result = std::move(response.swResponse.getValue().data);
            if (!hasWCError) {
                if ((wcErrorElem = result["writeConcernError"])) {
                    wcErrorShardId = response.shardId;
                    hasWCError = true;
                }
            }

            if (status.isOK()) {
                // The command status was OK.
                subobj.append(shard->getConnString().toString(), result);
                results.emplace_back(std::move(response.shardId), std::move(result));
                continue;
            }
        }

        // Either we failed to get a response, or the command had a non-OK status.

        // Convert the error status back into the format of a command result.
        BSONObjBuilder resultBob;
        Command::appendCommandStatus(resultBob, status);
        auto result = resultBob.obj();

        // Update the data structures that store the results.
        errors.append(shard->getConnString().toString(), status.reason());
        if (commonErrCode == -1) {
            commonErrCode = status.code();
        } else if (commonErrCode != status.code()) {
            commonErrCode = 0;
        }
        subobj.append(shard->getConnString().toString(), result);
        results.emplace_back(std::move(response.shardId), std::move(result));
    }

    subobj.done();

    if (hasWCError) {
        appendWriteConcernErrorToCmdResponse(wcErrorShardId, wcErrorElem, *output);
    }

    BSONObj errobj = errors.done();
    if (!errobj.isEmpty()) {
        // If code for all errors is the same, then report the common error code.
        if (commonErrCode > 0) {
            return {ErrorCodes::fromInt(commonErrCode), errobj.toString()};
        }
        return {ErrorCodes::OperationFailed, errobj.toString()};
    }

    return results;
}

}  // namespace mongo
