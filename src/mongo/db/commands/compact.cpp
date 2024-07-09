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

#include <string>
#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_compact.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/replication_coordinator.h"

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
               "warning: this operation locks the database and is slow. you can cancel with "
               "killOp()\n"
               "{ compact : <collection_name>, [force:<bool>] }\n"
               "  force - allows to run on a replica set primary\n";
    }

    CompactCmd() : BasicCommand("compact") {}

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        NamespaceString collectionNss = CommandHelpers::parseNsCollectionRequired(dbName, cmdObj);

        repl::ReplicationCoordinator* replCoord = repl::ReplicationCoordinator::get(opCtx);
        uassert(ErrorCodes::IllegalOperation,
                "will not run compact on an active replica set primary as this will slow down "
                "other running operations. use force:true to force",
                !replCoord->getMemberState().primary() || cmdObj["force"].trueValue());

        // This command is internal to the storage engine and should not block oplog application.
        ShouldNotConflictWithSecondaryBatchApplicationBlock noPBWMBlock(opCtx->lockState());

        Lock::GlobalLock lk(opCtx,
                            MODE_IX,
                            Date_t::max(),
                            Lock::InterruptBehavior::kThrow,
                            {.skipFlowControlTicket = true, .skipRSTLLock = true});

        // Hold reference to the catalog for collection lookup without locks to be safe.
        auto collectionCatalog = CollectionCatalog::get(opCtx);

        CollectionPtr collection = [&]() {
            if (CollectionPtr collection = CollectionPtr(
                    collectionCatalog->lookupCollectionByNamespace(opCtx, collectionNss))) {
                return collection;
            }

            // Check if this is a time-series collection.
            auto bucketsNs = collectionNss.makeTimeseriesBucketsNamespace();
            if (CollectionPtr collection = CollectionPtr(
                    collectionCatalog->lookupCollectionByNamespace(opCtx, bucketsNs))) {
                return collection;
            }

            return CollectionPtr();
        }();

        if (!collection) {
            std::shared_ptr<const ViewDefinition> view =
                collectionCatalog->lookupView(opCtx, collectionNss);
            uassert(ErrorCodes::CommandNotSupportedOnView, "can't compact a view", !view);
            uasserted(ErrorCodes::NamespaceNotFound, "collection does not exist");
        }

        AutoStatsTracker statsTracker(
            opCtx,
            collectionNss,
            Top::LockType::NotLocked,
            AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
            collectionCatalog->getDatabaseProfileLevel(collectionNss.dbName()));

        StatusWith<int64_t> status = compactCollection(opCtx, collection);

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
static CompactCmd compactCmd;
}  // namespace mongo
