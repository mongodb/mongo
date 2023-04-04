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


#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/commands/fle2_compact.h"
#include "mongo/db/commands/fle2_compact_gen.h"
#include "mongo/db/s/compact_structured_encryption_data_coordinator.h"
#include "mongo/db/s/compact_structured_encryption_data_coordinator_gen.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/s/cluster_commands_helpers.h"

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

            CurOp::get(opCtx)->debug().shouldOmitDiagnosticInformation = true;

            auto compactCoordinator =
                [&]() -> std::shared_ptr<ShardingDDLCoordinatorService::Instance> {
                FixedFCVRegion fixedFcvRegion(opCtx);

                // TODO: SERVER-68373 Remove once 7.0 becomes last LTS
                uassert(7330300,
                        "The preview version of compactStructuredEncryptionData is no longer "
                        "supported in this binary version",
                        gFeatureFlagFLE2CompactForProtocolV2.isEnabled(
                            serverGlobalParams.featureCompatibility));


                auto compact = makeRequest(opCtx);
                return ShardingDDLCoordinatorService::getService(opCtx)->getOrCreateInstance(
                    opCtx, compact.toBSON());
            }();

            return checked_pointer_cast<CompactStructuredEncryptionDataCoordinator>(
                       compactCoordinator)
                ->getResponse(opCtx);
        }

    private:
        CompactStructuredEncryptionDataState makeRequest(OperationContext* opCtx) {
            const auto& req = request();
            const auto& nss = req.getNamespace();

            AutoGetCollection baseColl(opCtx, nss, MODE_IX);
            uassert(ErrorCodes::NamespaceNotFound,
                    str::stream() << "Unknown collection: " << nss,
                    baseColl.getCollection());

            validateCompactRequest(req, *(baseColl.getCollection().get()));

            auto namespaces =
                uassertStatusOK(EncryptedStateCollectionsNamespaces::createFromDataCollection(
                    *(baseColl.getCollection().get())));

            AutoGetCollection ecocColl(opCtx, namespaces.ecocNss, MODE_IX);
            AutoGetCollection ecocTempColl(opCtx, namespaces.ecocRenameNss, MODE_IX);

            CompactStructuredEncryptionDataState compact;

            if (ecocColl.getCollection()) {
                compact.setEcocUuid(ecocColl->uuid());
            }
            if (ecocTempColl.getCollection()) {
                compact.setEcocRenameUuid(ecocTempColl->uuid());
            }

            compact.setShardingDDLCoordinatorMetadata(
                {{nss, DDLCoordinatorTypeEnum::kCompactStructuredEncryptionData}});
            compact.setEscNss(namespaces.escNss);
            compact.setEcocNss(namespaces.ecocNss);
            compact.setEcocRenameNss(namespaces.ecocRenameNss);
            compact.setCompactionTokens(req.getCompactionTokens().getOwned());

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
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }
    };

} shardsvrCompactStructuredEncryptionDataCommand;

}  // namespace
}  // namespace mongo
