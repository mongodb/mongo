/**
 *    Copyright (C) 2019-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include <cstddef>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/map_reduce_agg.h"
#include "mongo/db/commands/map_reduce_command_base.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"


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
