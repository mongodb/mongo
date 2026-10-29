// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/map_reduce_agg.h"
#include "mongo/db/commands/query_cmd/map_reduce_command_base.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"

#include <cstddef>

#include <boost/optional/optional.hpp>


namespace mongo {
namespace {

/**
 * This class represents a map/reduce command executed on a single server.
 */
class MapReduceCommand : public MapReduceCommandBase {
public:
    MapReduceCommand() = default;

    AllowedOnSecondary secondaryAllowed(ServiceContext* serviceContext) const override {
        if (!repl::ReplicationCoordinator::get(serviceContext)->getSettings().isReplSet()) {
            return AllowedOnSecondary::kAlways;
        }
        return AllowedOnSecondary::kOptIn;
    }

    bool isSubjectToIngressAdmissionControl() const override {
        return true;
    }

    std::size_t reserveBytesForReply() const override {
        return FindCommon::kInitReplyBufferSize;
    }

    void _explainImpl(OperationContext* opCtx,
                      const DatabaseName& dbName,
                      const BSONObj& cmd,
                      BSONObjBuilder& result,
                      boost::optional<ExplainOptions::Verbosity> verbosity) const override {
        map_reduce_agg::runAggregationMapReduce(opCtx, dbName, cmd, result, verbosity);
    }

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        return map_reduce_agg::runAggregationMapReduce(opCtx, dbName, cmdObj, result, boost::none);
    }
};
MONGO_REGISTER_COMMAND(MapReduceCommand).forShard();

}  // namespace
}  // namespace mongo
