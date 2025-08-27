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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/compact_gen.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/collection_compact.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"

#include <cstdint>
#include <iosfwd>
#include <string>

#include <absl/container/btree_set.h>

namespace mongo {

namespace {
static stdx::mutex mutex;
static absl::btree_set<UUID> compactsRunning;
}  // namespace

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
               "{ compact : <collection_name>, [dryRun:<bool>], [force:<bool>], "
               "[freeSpaceTargetMB:<int64_t>] }\n"
               "  dryRun - runs only the estimation phase of the compact operation\n"
               "  force - allows to run on a replica set primary\n"
               "  freeSpaceTargetMB - minimum amount of space recoverable for compaction to "
               "proceed\n";
    }

    CompactCmd() : BasicCommand("compact") {}

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        NamespaceString collectionNss = CommandHelpers::parseNsCollectionRequired(dbName, cmdObj);

        const auto vts = auth::ValidatedTenancyScope::get(opCtx);
        const auto sc = vts != boost::none
            ? SerializationContext::stateCommandRequest(vts->hasTenantId(), vts->isFromAtlasProxy())
            : SerializationContext::stateCommandRequest();

        auto params =
            CompactCommand::parse(cmdObj, IDLParserContext("compact", vts, dbName.tenantId(), sc));

        _assertCanRunCompact(opCtx, params);

        Lock::GlobalLock lk(opCtx,
                            MODE_IX,
                            Date_t::max(),
                            Lock::InterruptBehavior::kThrow,
                            {.skipFlowControlTicket = true, .skipRSTLLock = true});

        // Hold reference to the catalog for collection lookup without locks to be safe.
        auto collectionCatalog = CollectionCatalog::get(opCtx);

        CollectionPtr collection = [&]() {
            // Here and below, using the UNSAFE API is not a problem:
            // - The catalog snapshot is held open.
            // - Failures to locate the on-disk collection are handled at the storage-engine layer.
            // - We are unable to use the safe APIs (establishConsistentCollection or
            //   AcquireCollection) since the storage snapshot is abandoned at the storage-engine
            //   layer as part of running compact.
            if (CollectionPtr collection = CollectionPtr::CollectionPtr_UNSAFE(
                    collectionCatalog->lookupCollectionByNamespace(opCtx, collectionNss))) {
                return collection;
            }

            // Check if this is a time-series collection.
            auto bucketsNs = collectionNss.makeTimeseriesBucketsNamespace();
            if (CollectionPtr collection = CollectionPtr::CollectionPtr_UNSAFE(
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

        // Check against collection UUID as UUID's are immutable and stay consistent through
        // renames.
        UUID uuid = collection->uuid();

        {
            // Do not allow concurrent compact operations on the same namespace as this
            // concurrency will impact statistic gathering and can result in incorrect reporting.
            stdx::lock_guard<stdx::mutex> lk(mutex);
            if (compactsRunning.contains(uuid)) {
                uasserted(ErrorCodes::OperationFailed,
                          str::stream() << "Compaction is already in progress for "
                                        << collectionNss.toStringForErrorMsg());
            }
            compactsRunning.emplace(uuid);
        }

        ON_BLOCK_EXIT([&] {
            stdx::lock_guard<stdx::mutex> lk(mutex);
            compactsRunning.erase(uuid);
        });

        AutoStatsTracker statsTracker(opCtx,
                                      collectionNss,
                                      Top::LockType::NotLocked,
                                      AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                                      DatabaseProfileSettings::get(opCtx->getServiceContext())
                                          .getDatabaseProfileLevel(collectionNss.dbName()));

        CompactOptions options{.dryRun = params.getDryRun(),
                               .freeSpaceTargetMB = params.getFreeSpaceTargetMB()};
        StatusWith<int64_t> status = compactCollection(opCtx, options, collection);

        uassertStatusOK(status.getStatus());

        int64_t bytesForCompact = status.getValue();
        if (bytesForCompact < 0) {
            // When compacting a collection that is actively being written to, it is possible that
            // the collection is larger at the completion of compaction than when it started.
            bytesForCompact = 0;
        }

        result.appendNumber(options.dryRun ? "estimatedBytesFreed" : "bytesFreed",
                            static_cast<long long>(bytesForCompact));

        return true;
    }

private:
    void _assertCanRunCompact(OperationContext* opCtx, const CompactCommand& params) {
        repl::ReplicationCoordinator* replCoord = repl::ReplicationCoordinator::get(opCtx);
        bool force = params.getForce() && *params.getForce();
        uassert(ErrorCodes::IllegalOperation,
                "will not run compact on an active replica set primary as this will slow down "
                "other running operations. use force:true to force",
                !replCoord->getMemberState().primary() || force);

        uassert(ErrorCodes::IllegalOperation,
                "Compact command with extra options requires its feature flag to be enabled",
                gFeatureFlagCompactOptions.isEnabled() ||
                    (!params.getFreeSpaceTargetMB() && !params.getDryRun()));
    }
};

MONGO_REGISTER_COMMAND(CompactCmd).forShard();

}  // namespace mongo
