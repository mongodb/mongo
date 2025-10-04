/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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


#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/commands/fle2_compact.h"
#include "mongo/db/commands/fle2_compact_gen.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/compact_structured_encryption_data_coordinator.h"
#include "mongo/db/global_catalog/ddl/compact_structured_encryption_data_coordinator_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator_service.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <set>
#include <string>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

class _shardsvrCompactStructuredEncryptionDataCommand final
    : public TypedCommand<_shardsvrCompactStructuredEncryptionDataCommand> {
public:
    using Request = CompactStructuredEncryptionData;
    using Reply = typename Request::Reply;

    _shardsvrCompactStructuredEncryptionDataCommand()
        : TypedCommand("_shardsvrCompactStructuredEncryptionData"_sd) {}

    bool skipApiVersionCheck() const final {
        // Internal command (server to server).
        return true;
    }

    std::string help() const final {
        return "Internal command. Do not call directly. Compacts a ECOC collection.";
    }

    bool adminOnly() const final {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kNever;
    }

    std::set<StringData> sensitiveFieldNames() const final {
        return {CompactStructuredEncryptionData::kCompactionTokensFieldName};
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Reply typedRun(OperationContext* opCtx) {
            {
                stdx::lock_guard<Client> lk(*opCtx->getClient());
                CurOp::get(opCtx)->setShouldOmitDiagnosticInformation(lk, true);
            }

            auto compactCoordinator =
                [&]() -> std::shared_ptr<ShardingDDLCoordinatorService::Instance> {
                FixedFCVRegion fixedFcvRegion(opCtx);
                auto compact = writeConflictRetry(opCtx,
                                                  Request::kCommandName,
                                                  request().getNamespace(),
                                                  [&]() { return makeRequest(opCtx); });
                return ShardingDDLCoordinatorService::getService(opCtx)->getOrCreateInstance(
                    opCtx, compact.toBSON(), fixedFcvRegion);
            }();

            return checked_pointer_cast<CompactStructuredEncryptionDataCoordinator>(
                       compactCoordinator)
                ->getResponse(opCtx);
        }

    private:
        CompactStructuredEncryptionDataState makeRequest(OperationContext* opCtx) {
            const auto& req = request();
            const auto& nss = req.getNamespace();
            // Routers route DDLS to the db-primary shard, with a 'databaseVersion' attached to
            // the command but no 'shardVersion'. This is okay, because the db-primary shard
            // will coordinate the operation. However, we need to attach an IGNORED Shard Role
            // so that we are able to access the collection metadata. This is ok as long as we
            // don't access user data.
            boost::optional<ScopedSetShardRole> optShardRoleIgnore;
            if (!OperationShardingState::get(opCtx).getShardVersion(nss)) {
                ShardVersion shardVersionIgnored;
                shardVersionIgnored.setPlacementVersionIgnored();
                optShardRoleIgnore.emplace(opCtx, nss, shardVersionIgnored, boost::none);
            }
            auto baseColl = acquireCollection(opCtx,
                                              CollectionAcquisitionRequest::fromOpCtx(
                                                  opCtx, nss, AcquisitionPrerequisites::kWrite),
                                              MODE_IX);
            uassert(ErrorCodes::NamespaceNotFound,
                    str::stream() << "Unknown collection: " << nss.toStringForErrorMsg(),
                    baseColl.exists());

            validateCompactRequest(req, *(baseColl.getCollectionPtr().get()));

            auto namespaces =
                uassertStatusOK(EncryptedStateCollectionsNamespaces::createFromDataCollection(
                    *(baseColl.getCollectionPtr().get())));

            CompactStructuredEncryptionDataState compact;

            // To avoid deadlock, IX locks for ecocRenameNss and ecocNss must be acquired in the
            // same order they'd be acquired during renameCollection (ascending ResourceId order).
            // The 2 collections are unrouted so we need to specify a version. By design, these
            // collections are always unsharded (untracked) and therefore on the primary shard.
            {
                auto dbVersion = OperationShardingState::get(opCtx).getDbVersion(nss.dbName());
                auto pc = PlacementConcern(dbVersion, ShardVersion::UNSHARDED());
                CollectionAcquisitionRequests requests = {
                    CollectionAcquisitionRequest(namespaces.ecocNss,
                                                 pc,
                                                 repl::ReadConcernArgs::get(opCtx),
                                                 AcquisitionPrerequisites::kWrite),
                    CollectionAcquisitionRequest(namespaces.ecocRenameNss,
                                                 pc,
                                                 repl::ReadConcernArgs::get(opCtx),
                                                 AcquisitionPrerequisites::kWrite),
                };

                auto allAcquisitions =
                    makeAcquisitionMap(acquireCollections(opCtx, requests, MODE_IX));
                auto ecocColl = allAcquisitions.extract(namespaces.ecocNss).mapped();
                if (ecocColl.exists()) {
                    compact.setEcocUuid(ecocColl.uuid());
                }

                auto ecocTempColl = allAcquisitions.extract(namespaces.ecocRenameNss).mapped();
                if (ecocTempColl.exists()) {
                    compact.setEcocRenameUuid(ecocTempColl.uuid());
                }
            }

            compact.setShardingDDLCoordinatorMetadata(
                {{nss, DDLCoordinatorTypeEnum::kCompactStructuredEncryptionData}});
            compact.setEscNss(namespaces.escNss);
            compact.setEcocNss(namespaces.ecocNss);
            compact.setEcocRenameNss(namespaces.ecocRenameNss);
            compact.setCompactionTokens(req.getCompactionTokens().getOwned());
            compact.setEncryptionInformation(req.getEncryptionInformation());
            compact.setAnchorPaddingFactor(req.getAnchorPaddingFactor());


            return compact;
        }

        NamespaceString ns() const override {
            return request().getNamespace();
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::internal));
        }
    };
};
MONGO_REGISTER_COMMAND(_shardsvrCompactStructuredEncryptionDataCommand).forShard();

}  // namespace
}  // namespace mongo
