/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include <set>
#include <string>

#include <boost/move/utility_core.hpp>

#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/commands/fle2_cleanup_gen.h"
#include "mongo/db/commands/fle2_compact.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/cleanup_structured_encryption_data_coordinator.h"
#include "mongo/db/s/cleanup_structured_encryption_data_coordinator_gen.h"
#include "mongo/db/s/sharding_ddl_coordinator_gen.h"
#include "mongo/db/s/sharding_ddl_coordinator_service.h"
#include "mongo/db/service_context.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

class _shardsvrCleanupStructuredEncryptionDataCommand final
    : public TypedCommand<_shardsvrCleanupStructuredEncryptionDataCommand> {
public:
    using Request = CleanupStructuredEncryptionData;
    using Reply = typename Request::Reply;

    _shardsvrCleanupStructuredEncryptionDataCommand()
        : TypedCommand("_shardsvrCleanupStructuredEncryptionData"_sd) {}

    bool skipApiVersionCheck() const final {
        // Internal command (server to server).
        return true;
    }

    std::string help() const final {
        return "Internal command. Do not call directly. Cleans up an ECOC collection.";
    }

    bool adminOnly() const final {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kNever;
    }

    std::set<StringData> sensitiveFieldNames() const final {
        return {CleanupStructuredEncryptionData::kCleanupTokensFieldName};
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Reply typedRun(OperationContext* opCtx) {
            {
                stdx::lock_guard<Client> lk(*opCtx->getClient());
                CurOp::get(opCtx)->setShouldOmitDiagnosticInformation(lk, true);
            }

            auto cleanupCoordinator =
                [&]() -> std::shared_ptr<ShardingDDLCoordinatorService::Instance> {
                FixedFCVRegion fixedFcvRegion(opCtx);

                auto cleanup = makeRequest(opCtx);
                return ShardingDDLCoordinatorService::getService(opCtx)->getOrCreateInstance(
                    opCtx, cleanup.toBSON());
            }();

            return checked_pointer_cast<CleanupStructuredEncryptionDataCoordinator>(
                       cleanupCoordinator)
                ->getResponse(opCtx);
        }

    private:
        CleanupStructuredEncryptionDataState makeRequest(OperationContext* opCtx) {
            const auto& req = request();
            const auto& nss = req.getNamespace();

            AutoGetCollection baseColl(opCtx, nss, MODE_IX);
            uassert(ErrorCodes::NamespaceNotFound,
                    str::stream() << "Unknown collection: " << nss.toStringForErrorMsg(),
                    baseColl.getCollection());

            validateCleanupRequest(req, *(baseColl.getCollection().get()));

            auto namespaces =
                uassertStatusOK(EncryptedStateCollectionsNamespaces::createFromDataCollection(
                    *(baseColl.getCollection().get())));

            CleanupStructuredEncryptionDataState cleanup;

            // To avoid deadlock, IX locks for ecocRenameNss and ecocNss must be acquired in the
            // same order they'd be acquired during renameCollection (ascending ResourceId order).
            // Providing ecocRenameNss as a secondary to ecocNss in AutoGetCollection ensures the
            // locks for both namespaces are acquired in correct order.
            {
                std::vector<NamespaceStringOrUUID> secondaryNss = {namespaces.ecocRenameNss};
                AutoGetCollection ecocColl(opCtx,
                                           namespaces.ecocNss,
                                           MODE_IX,
                                           AutoGetCollection::Options{}.secondaryNssOrUUIDs(
                                               secondaryNss.cbegin(), secondaryNss.cend()));
                if (ecocColl.getCollection()) {
                    cleanup.setEcocUuid(ecocColl->uuid());
                }
                auto catalog = CollectionCatalog::get(opCtx);
                auto ecocTempColl = CollectionPtr(
                    catalog->lookupCollectionByNamespace(opCtx, namespaces.ecocRenameNss));
                if (ecocTempColl) {
                    cleanup.setEcocRenameUuid(ecocTempColl->uuid());
                }
            }

            cleanup.setShardingDDLCoordinatorMetadata(
                {{nss, DDLCoordinatorTypeEnum::kCleanupStructuredEncryptionData}});
            cleanup.setEscNss(namespaces.escNss);
            cleanup.setEcocNss(namespaces.ecocNss);
            cleanup.setEcocRenameNss(namespaces.ecocRenameNss);
            cleanup.setCleanupTokens(req.getCleanupTokens().getOwned());

            return cleanup;
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
MONGO_REGISTER_COMMAND(_shardsvrCleanupStructuredEncryptionDataCommand).forShard();

}  // namespace
}  // namespace mongo
