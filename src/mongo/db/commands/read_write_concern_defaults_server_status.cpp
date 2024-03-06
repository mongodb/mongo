/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <memory>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/commands/rwc_defaults_commands_gen.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"

namespace mongo {
namespace {

class ReadWriteConcernDefaultsServerStatus final : public ServerStatusSection {
public:
    ReadWriteConcernDefaultsServerStatus() : ServerStatusSection("defaultRWConcern") {}

    bool includeByDefault() const override {
        return !serverGlobalParams.clusterRole.hasExclusively(ClusterRole::ShardServer);
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        if (serverGlobalParams.clusterRole.hasExclusively(ClusterRole::ShardServer) ||
            !repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet()) {
            return {};
        }

        auto rwcDefault = ReadWriteConcernDefaults::get(opCtx).getDefault(opCtx);
        GetDefaultRWConcernResponse response;
        response.setRWConcernDefault(rwcDefault);
        response.setLocalUpdateWallClockTime(rwcDefault.localUpdateWallClockTime());
        return response.toBSON();
    }

} defaultRWConcernServerStatus;

}  // namespace
}  // namespace mongo
