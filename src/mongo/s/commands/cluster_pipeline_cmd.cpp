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

#include "mongo/base/status.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/s/commands/cluster_aggregate.h"

namespace mongo {
namespace {

class ClusterPipelineCommand : public BasicCommand {
public:
    ClusterPipelineCommand() : BasicCommand("aggregate") {}

    void help(std::stringstream& help) const {
        help << "Runs the sharded aggregation command. See "
                "http://dochub.mongodb.org/core/aggregation for more details.";
    }

    bool slaveOk() const override {
        return true;
    }

    bool adminOnly() const override {
        return false;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return Pipeline::aggSupportsWriteConcern(cmd);
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) override {
        const NamespaceString nss(AggregationRequest::parseNs(dbname, cmdObj));
        return AuthorizationSession::get(client)->checkAuthForAggregate(nss, cmdObj, true);
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        return appendCommandStatus(result,
                                   _runAggCommand(opCtx, dbname, cmdObj, boost::none, &result));
    }

    Status explain(OperationContext* opCtx,
                   const std::string& dbname,
                   const BSONObj& cmdObj,
                   ExplainOptions::Verbosity verbosity,
                   BSONObjBuilder* out) const override {
        return _runAggCommand(opCtx, dbname, cmdObj, verbosity, out);
    }

private:
    static Status _runAggCommand(OperationContext* opCtx,
                                 const std::string& dbname,
                                 const BSONObj& cmdObj,
                                 boost::optional<ExplainOptions::Verbosity> verbosity,
                                 BSONObjBuilder* result) {
        const auto aggregationRequest =
            uassertStatusOK(AggregationRequest::parseFromBSON(dbname, cmdObj, verbosity));

        const auto& nss = aggregationRequest.getNamespaceString();

        return ClusterAggregate::runAggregate(
            opCtx, ClusterAggregate::Namespaces{nss, nss}, aggregationRequest, cmdObj, result);
    }

} clusterPipelineCmd;

}  // namespace
}  // namespace mongo
