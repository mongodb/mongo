// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands/rwc_defaults_commands_gen.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/topology/cluster_role.h"

namespace mongo {
namespace {

class ReadWriteConcernDefaultsServerStatus final : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return !serverGlobalParams.clusterRole.isShardOnly();
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        if (serverGlobalParams.clusterRole.isShardOnly() ||
            !repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet()) {
            return {};
        }

        auto rwcDefault = ReadWriteConcernDefaults::get(opCtx).getDefault(opCtx);
        GetDefaultRWConcernResponse response;
        response.setRWConcernDefault(rwcDefault);
        response.setLocalUpdateWallClockTime(rwcDefault.localUpdateWallClockTime());
        return response.toBSON();
    }
};
auto defaultRWConcernServerStatus =
    *ServerStatusSectionBuilder<ReadWriteConcernDefaultsServerStatus>("defaultRWConcern")
         .forShard();
}  // namespace
}  // namespace mongo
