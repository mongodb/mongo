// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/map_reduce_command_base.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/service_context.h"
#include "mongo/s/commands/query_cmd/cluster_map_reduce_agg.h"

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

class ClusterMapReduceCommand : public MapReduceCommandBase {
public:
    ClusterMapReduceCommand() = default;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    void _explainImpl(OperationContext* opCtx,
                      const DatabaseName& dbName,
                      const BSONObj& cmd,
                      BSONObjBuilder& result,
                      boost::optional<ExplainOptions::Verbosity> verbosity) const override {
        runAggregationMapReduce(opCtx, dbName, cmd, result, verbosity);
    }

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        return runAggregationMapReduce(opCtx, dbName, cmdObj, result, boost::none);
    }
};
MONGO_REGISTER_COMMAND(ClusterMapReduceCommand).forRouter();

}  // namespace
}  // namespace mongo
