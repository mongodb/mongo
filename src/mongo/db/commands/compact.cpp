/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <cstdint>
#include <iosfwd>
#include <string>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection_compact.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/compact_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"

namespace mongo {

using std::string;
using std::stringstream;

class CompactCmd : public BasicCommand {
public:
    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool adminOnly() const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const override {
        auto* as = AuthorizationSession::get(opCtx->getClient());
        if (!as->isAuthorizedForActionsOnResource(parseResourcePattern(dbName, cmdObj),
                                                  ActionType::compact)) {
            return {ErrorCodes::Unauthorized, "unauthorized"};
        }

        return Status::OK();
    }

    std::string help() const override {
        return "compact collection\n"
               "warning: this operation has blocking behaviour and is slow. You can cancel with "
               "killOp()\n"
               "{ compact : <collection_name>, [force:<bool>], [freeSpaceTargetMB:<int64_t>] }\n"
               "  force - allows to run on a replica set primary\n"
               "  freeSpaceTargetMB - minimum amount of space recoverable for compaction to "
               "proceed\n";
    }

    CompactCmd() : BasicCommand("compact") {}

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        NamespaceString nss = CommandHelpers::parseNsCollectionRequired(dbName, cmdObj);

        const auto vts = auth::ValidatedTenancyScope::get(opCtx);
        const auto sc = vts != boost::none
            ? SerializationContext::stateCommandRequest(vts->hasTenantId(), vts->isFromAtlasProxy())
            : SerializationContext::stateCommandRequest();

        repl::ReplicationCoordinator* replCoord = repl::ReplicationCoordinator::get(opCtx);
        auto params = CompactCommand::parse(
            IDLParserContext("compact", false /*apiStrict*/, vts, dbName.tenantId(), sc), cmdObj);
        bool force = params.getForce() && *params.getForce();

        uassert(ErrorCodes::IllegalOperation,
                "will not run compact on an active replica set primary as this will slow down "
                "other running operations. use force:true to force",
                !replCoord->getMemberState().primary() || force);

        StatusWith<int64_t> status = compactCollection(opCtx, params.getFreeSpaceTargetMB(), nss);
        uassertStatusOK(status.getStatus());

        int64_t bytesFreed = status.getValue();
        if (bytesFreed < 0) {
            // When compacting a collection that is actively being written to, it is possible that
            // the collection is larger at the completion of compaction than when it started.
            bytesFreed = 0;
        }

        result.appendNumber("bytesFreed", static_cast<long long>(bytesFreed));

        return true;
    }
};

MONGO_REGISTER_COMMAND(CompactCmd).forShard();

}  // namespace mongo
