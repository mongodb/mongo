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


#include "mongo/platform/basic.h"

#include "mongo/db/commands/fle2_compact.h"

#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/create_gen.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {
namespace {

/**
 * Ensures that only one compactStructuredEncryptionData can run at a given time.
 */
Lock::ResourceMutex commandMutex("compactStructuredEncryptionDataCommandMutex");

CompactStats compactEncryptedCompactionCollection(OperationContext* opCtx,
                                                  const CompactStructuredEncryptionData& request) {

    uassert(6583201,
            str::stream() << CompactStructuredEncryptionData::kCommandName
                          << " must be run through mongos in a sharded cluster",
            !ShardingState::get(opCtx)->enabled());

    // Only allow one instance of compactStructuredEncryptionData to run at a time.
    Lock::ExclusiveLock fleCompactCommandLock(opCtx->lockState(), commandMutex);

    const auto& edcNss = request.getNamespace();

    LOGV2(6319900, "Compacting the encrypted compaction collection", "namespace"_attr = edcNss);

    AutoGetDb autoDb(opCtx, edcNss.dbName(), MODE_IX);
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

    validateCompactRequest(request, *edc.get());

    auto namespaces =
        uassertStatusOK(EncryptedStateCollectionsNamespaces::createFromDataCollection(*edc.get()));

    // Step 1: rename the ECOC collection if it exists
    auto ecoc = catalog->lookupCollectionByNamespace(opCtx, namespaces.ecocNss);
    auto ecocRename = catalog->lookupCollectionByNamespace(opCtx, namespaces.ecocRenameNss);
    bool renamed = false;

    if (ecoc && !ecocRename) {
        LOGV2(6319901,
              "Renaming the encrypted compaction collection",
              "ecocNss"_attr = namespaces.ecocNss,
              "ecocRenameNss"_attr = namespaces.ecocRenameNss);
        RenameCollectionOptions renameOpts;
        validateAndRunRenameCollection(
            opCtx, namespaces.ecocNss, namespaces.ecocRenameNss, renameOpts);
        ecoc.reset();
        renamed = true;
    }

    if (!ecocRename && !renamed) {
        // no pre-existing renamed ECOC collection and the rename did not occur,
        // so there is nothing to compact
        LOGV2(6548306, "Skipping compaction as there is no ECOC collection to compact");
        return CompactStats(ECOCStats(), ECStats(), ECStats());
    }

    // Step 2: for each encrypted field in compactionTokens, get distinct set of entries 'C'
    // from ECOC, and for each entry in 'C', compact ESC and ECC.
    CompactStats stats;
    {
        // acquire IS lock on the ecocRenameNss to prevent it from being dropped during compact
        AutoGetCollection tempEcocColl(opCtx, namespaces.ecocRenameNss, MODE_IS);

        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "Renamed encrypted compaction collection "
                              << namespaces.ecocRenameNss
                              << " no longer exists prior to compaction",
                tempEcocColl.getCollection());

        stats = processFLECompact(opCtx, request, &getTransactionWithRetriesForMongoD, namespaces);
    }

    // Step 3: drop the renamed ECOC collection
    DropReply dropReply;
    uassertStatusOK(
        dropCollection(opCtx,
                       namespaces.ecocRenameNss,
                       &dropReply,
                       DropCollectionSystemCollectionMode::kDisallowSystemCollectionDrops));

    LOGV2(6319902,
          "Done compacting the encrypted compaction collection",
          "namespace"_attr = request.getNamespace());
    return stats;
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
