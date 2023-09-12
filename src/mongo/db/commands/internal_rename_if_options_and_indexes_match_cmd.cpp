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

#include <list>
#include <memory>
#include <string>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/internal_rename_if_options_and_indexes_match_gen.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/ddl_lock_manager.h"
#include "mongo/db/s/scoped_collection_metadata.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(blockBeforeInternalRenameAndBeforeTakingDDLLocks);
MONGO_FAIL_POINT_DEFINE(blockBeforeInternalRenameAndAfterTakingDDLLocks);

bool isCollectionSharded(OperationContext* opCtx, const NamespaceString& nss) {
    AutoGetCollectionForRead lock(opCtx, nss);
    return opCtx->writesAreReplicated() &&
        CollectionShardingState::assertCollectionLockedAndAcquire(opCtx, nss)
            ->getCollectionDescription(opCtx)
            .isSharded();
}

/**
 * Rename a collection while checking collection option and indexes.
 */
class InternalRenameIfOptionsAndIndexesMatchCmd final
    : public TypedCommand<InternalRenameIfOptionsAndIndexesMatchCmd> {
public:
    using Request = InternalRenameIfOptionsAndIndexesMatch;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            auto thisRequest = request();
            const auto& fromNss = thisRequest.getFrom();
            const auto& toNss = thisRequest.getTo();
            const auto& originalIndexes = thisRequest.getIndexes();
            const auto indexList =
                std::list<BSONObj>(originalIndexes.begin(), originalIndexes.end());
            const auto& collectionOptions = thisRequest.getCollectionOptions();

            if (MONGO_unlikely(blockBeforeInternalRenameAndBeforeTakingDDLLocks.shouldFail())) {
                blockBeforeInternalRenameAndBeforeTakingDDLLocks.pauseWhileSet();
            }

            if (serverGlobalParams.clusterRole.has(ClusterRole::None)) {
                // No need to acquire additional locks in a non-sharded environment
                _internalRun(opCtx, fromNss, toNss, indexList, collectionOptions);
                return;
            }

            /**
             * Acquiring the DDL lock for involved namespaces allows to:
             * - Serialize with sharded DDLs, ensuring no concurrent modifications of the
             * collections.
             * - Check safely if the target collection is sharded or not.
             * - Check if the current shard is still the primary for the database.
             */
            static constexpr StringData lockReason{"internalRenameCollection"_sd};
            const DDLLockManager::ScopedCollectionDDLLock fromCollDDLLock{
                opCtx, fromNss, lockReason, MODE_X};

            // If we are renaming a buckets collection in the $out stage, we must acquire a lock on
            // the view namespace, instead of the buckets namespace. This lock avoids concurrent
            // modifications, since users run operations on the view and not the buckets namespace
            // and all time-series DDL operations take a lock on the view namespace.
            const DDLLockManager::ScopedCollectionDDLLock toCollDDLLock{
                opCtx,
                fromNss.isOutTmpBucketsCollection() ? toNss.getTimeseriesViewNamespace() : toNss,
                lockReason,
                MODE_X};

            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << "cannot rename to sharded collection '"
                                  << toNss.toStringForErrorMsg() << "'",
                    !isCollectionSharded(opCtx, toNss));

            _internalRun(opCtx, fromNss, toNss, indexList, collectionOptions);
        }

    private:
        static void _internalRun(OperationContext* opCtx,
                                 const NamespaceString& fromNss,
                                 const NamespaceString& toNss,
                                 const std::list<BSONObj>& indexList,
                                 const BSONObj& collectionOptions) {
            if (MONGO_unlikely(blockBeforeInternalRenameAndAfterTakingDDLLocks.shouldFail())) {
                blockBeforeInternalRenameAndAfterTakingDDLLocks.pauseWhileSet();
            }
            RenameCollectionOptions options;
            options.dropTarget = true;
            options.stayTemp = false;
            doLocalRenameIfOptionsAndIndexesHaveNotChanged(
                opCtx, fromNss, toNss, options, indexList, collectionOptions);
        }

        NamespaceString ns() const override {
            return request().getFrom();
        }
        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            auto thisRequest = request();
            auto from = thisRequest.getFrom();
            auto to = thisRequest.getTo();
            uassert(ErrorCodes::Unauthorized,
                    str::stream() << "Unauthorized to rename " << from.toStringForErrorMsg(),
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(from),
                                                           ActionType::renameCollection));
            uassert(ErrorCodes::Unauthorized,
                    str::stream() << "Unauthorized to drop " << to.toStringForErrorMsg(),
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(to),
                                                           ActionType::dropCollection));
            uassert(ErrorCodes::Unauthorized,
                    str::stream() << "Unauthorized to insert into " << to.toStringForErrorMsg(),
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(to),
                                                           ActionType::insert));
        }
    };

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command to rename and check collection options";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
};
MONGO_REGISTER_COMMAND(InternalRenameIfOptionsAndIndexesMatchCmd).forShard();

}  // namespace
}  // namespace mongo
