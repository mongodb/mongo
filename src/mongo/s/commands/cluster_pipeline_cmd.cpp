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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include <string>

#include "mongo/base/status.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/s/commands/cluster_aggregate.h"

namespace mongo {
namespace {

/**
 * Implements the aggregation (pipeline command for sharding).
 */
class PipelineCommand : public Command {
public:
    PipelineCommand() : Command(AggregationRequest::kCommandName, false) {}

    virtual bool slaveOk() const {
        return true;
    }

    virtual bool adminOnly() const {
        return false;
    }


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return Pipeline::aggSupportsWriteConcern(cmd);
    }

    virtual void help(std::stringstream& help) const {
        help << "Runs the sharded aggregation command";
    }

    // virtuals from Command
    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) final {
        const NamespaceString nss(parseNsCollectionRequired(dbname, cmdObj));
        return AuthorizationSession::get(client)->checkAuthForAggregate(nss, cmdObj);
    }

    virtual bool run(OperationContext* opCtx,
                     const std::string& dbname,
                     BSONObj& cmdObj,
                     std::string& errmsg,
                     BSONObjBuilder& result) {
        const NamespaceString nss(parseNsCollectionRequired(dbname, cmdObj));

        auto request = AggregationRequest::parseFromBSON(nss, cmdObj);
        if (!request.isOK()) {
            return appendCommandStatus(result, request.getStatus());
        }

        ClusterAggregate::Namespaces nsStruct;
        nsStruct.requestedNss = nss;
        nsStruct.executionNss = std::move(nss);
        auto status =
            ClusterAggregate::runAggregate(opCtx, nsStruct, request.getValue(), cmdObj, &result);
        appendCommandStatus(result, status);
        return status.isOK();
    }

    Status explain(OperationContext* opCtx,
                   const std::string& dbName,
                   const BSONObj& cmdObj,
                   ExplainOptions::Verbosity verbosity,
                   const rpc::ServerSelectionMetadata& serverSelectionMetadata,
                   BSONObjBuilder* out) const override {
        const NamespaceString nss(parseNsCollectionRequired(dbName, cmdObj));

        auto request = AggregationRequest::parseFromBSON(nss, cmdObj, verbosity);
        if (!request.isOK()) {
            return request.getStatus();
        }

        // Add the server selection metadata to the aggregate command in the "unwrapped" format that
        // runAggregate() expects: {aggregate: ..., $queryOptions: {$readPreference: ...}}.
        BSONObjBuilder aggCmdBuilder;
        aggCmdBuilder.appendElements(cmdObj);
        if (auto readPref = serverSelectionMetadata.getReadPreference()) {
            auto readPrefObj = readPref->toBSON();
            aggCmdBuilder.append(QueryRequest::kUnwrappedReadPrefField,
                                 BSON("$readPreference" << readPrefObj));
        }

        ClusterAggregate::Namespaces nsStruct;
        nsStruct.requestedNss = nss;
        nsStruct.executionNss = std::move(nss);

        return ClusterAggregate::runAggregate(opCtx, nsStruct, request.getValue(), cmdObj, out);
    }
} clusterPipelineCmd;

}  // namespace
}  // namespace mongo
