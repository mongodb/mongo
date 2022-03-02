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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/commands/fle2_compact.h"

#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace {

CompactStats compactEncryptedCompactionCollection(OperationContext* opCtx,
                                                  const CompactStructuredEncryptionData& request) {
    mongo::ECOCStats ecocStats(0, 0);
    mongo::ECStats eccStats(0, 0, 0, 0);
    mongo::ECStats escStats(0, 0, 0, 0);
    const auto& edcNss = request.getNamespace();

    LOGV2(6319900, "Compacting the encrypted compaction collection", "namespace"_attr = edcNss);

    AutoGetDb autoDb(opCtx, edcNss.db(), MODE_IX);
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Database '" << edcNss.db() << "' does not exist",
            autoDb.getDb());

    auto catalog = CollectionCatalog::get(opCtx);
    Lock::CollectionLock edcLock(opCtx, edcNss, MODE_IS);

    // Check the data collection exists and is not a view
    auto edc = catalog->lookupCollectionByNamespace(opCtx, edcNss);
    if (!edc) {
        uassert(ErrorCodes::CommandNotSupportedOnView,
                "Cannot compact structured encryption data on a view",
                !catalog->lookupView(opCtx, edcNss));
        uasserted(ErrorCodes::NamespaceNotFound,
                  str::stream() << "Collection '" << edcNss << "' does not exist");
    }

    uassert(6319903, "Feature flag FLE2 is not enabled", gFeatureFlagFLE2.isEnabledAndIgnoreFCV());

    auto namespaces =
        uassertStatusOK(EncryptedStateCollectionsNamespaces::createFromDataCollection(*edc.get()));


    auto ecoc = catalog->lookupCollectionByNamespace(opCtx, namespaces.ecocNss);
    auto ecocRename = catalog->lookupCollectionByNamespace(opCtx, namespaces.ecocRenameNss);

    if (ecoc && !ecocRename) {
        LOGV2(6319901,
              "Renaming the encrypted compaction collection",
              "ecocNss"_attr = namespaces.ecocNss,
              "ecocRenameNss"_attr = namespaces.ecocRenameNss);
        RenameCollectionOptions renameOpts;
        validateAndRunRenameCollection(
            opCtx, namespaces.ecocNss, namespaces.ecocRenameNss, renameOpts);
        ecoc.reset();
    }

    LOGV2(6319902,
          "Done compacting the encrypted compaction collection",
          "namespace"_attr = request.getNamespace());
    return CompactStats(ecocStats, eccStats, escStats);
}

class CompactStructuredEncryptionDataCmd final
    : public TypedCommand<CompactStructuredEncryptionDataCmd> {
public:
    using Request = CompactStructuredEncryptionData;
    using Reply = CompactStructuredEncryptionData::Reply;
    using TC = TypedCommand<CompactStructuredEncryptionDataCmd>;

    class Invocation final : public TC::InvocationBase {
    public:
        using TC::InvocationBase::InvocationBase;
        using TC::InvocationBase::request;

        Reply typedRun(OperationContext* opCtx) {
            return Reply(compactEncryptedCompactionCollection(opCtx, request()));
        }

    private:
        bool supportsWriteConcern() const final {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            auto* as = AuthorizationSession::get(opCtx->getClient());
            uassert(ErrorCodes::Unauthorized,
                    "Not authorized to compact structured encryption data",
                    as->isAuthorizedForActionsOnResource(
                        ResourcePattern::forExactNamespace(request().getNamespace()),
                        ActionType::compactStructuredEncryptionData));
        }

        NamespaceString ns() const final {
            return request().getNamespace();
        }
    };

    typename TC::AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return BasicCommand::AllowedOnSecondary::kNever;
    }

    bool adminOnly() const final {
        return false;
    }
} compactStructuredEncryptionDataCmd;

}  // namespace
}  // namespace mongo
