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

#include "mongo/crypto/fle_options_gen.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/create_gen.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/commands/fle2_compact.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

namespace {

void createQEClusteredStateCollection(OperationContext* opCtx, const NamespaceString& nss) {
    CreateCommand createCmd(nss);
    mongo::ClusteredIndexSpec clusterIdxSpec(BSON("_id" << 1), true);
    createCmd.setClusteredIndex(
        stdx::variant<bool, mongo::ClusteredIndexSpec>(std::move(clusterIdxSpec)));
    auto status = createCollection(opCtx, createCmd);
    if (!status.isOK()) {
        if (status != ErrorCodes::NamespaceExists) {
            uassertStatusOK(status);
        }
        LOGV2_DEBUG(
            7618801, 1, "Create collection failed because namespace already exists", logAttrs(nss));
    }
}

void dropQEStateCollection(OperationContext* opCtx, const NamespaceString& nss) {
    DropReply dropReply;
    uassertStatusOK(
        dropCollection(opCtx,
                       nss,
                       &dropReply,
                       DropCollectionSystemCollectionMode::kDisallowSystemCollectionDrops));
    LOGV2_DEBUG(7618802, 1, "QE state collection drop finished", "reply"_attr = dropReply);
}

/**
 * QE cleanup is similar to QE compact in that it also performs "compaction" of the
 * ESC collection by removing stale ESC non-anchors. Unlike compact, cleanup also removes
 * stale ESC anchors. It also differs from compact in that instead of inserting "anchors"
 * to the ESC, cleanup only inserts or updates "null" anchors.
 *
 * At a high level, the cleanup algorithm works as follows:
 * 1. The _ids of random ESC non-anchors are first read into an in-memory set 'P'.
 * 2. (*) a temporary 'esc.deletes' collection is created. This will collection will contain
       the _ids of anchor documents that cleanup will remove towards the end of the operation.
 * 3. The ECOC is renamed to a temporary namespace (hereby referred to as 'ecoc.compact').
 * 4. Unique entries from 'ecoc.compact' are decoded into an in-memory set of tokens: 'C'.
 * 5. For each token in 'C', the following is performed:
 *    a. Start a transaction
 *    b. Run EmuBinary to collect the latest anchor and non-anchor positions for the current token.
 *    c. (*) Insert (or update an existing) null anchor which encodes the latest positions.
 *    d. (*) If there are anchors corresponding to the current token, insert their _ids
 *       into 'esc.deletes'. These anchors are now stale and are marked for deletion.
 *    e. Commit transaction
 * 6. Delete every document in the ESC whose _id can be found in 'P'
 * 7. (*) Delete every document in the ESC whose _id can be found in 'esc.deletes'
 * 8. (*) Drop 'esc.deletes'
 * 9. Drop 'ecoc.compact'
 *
 * Steps marked with (*) are unique to the cleanup operation.
 */
CleanupStats cleanupEncryptedCollection(OperationContext* opCtx,
                                        const CleanupStructuredEncryptionData& request) {
    CurOp::get(opCtx)->debug().shouldOmitDiagnosticInformation = true;

    uassert(7618803,
            str::stream() << "Feature flag `FLE2CleanupCommand` must be enabled to run "
                          << CleanupStructuredEncryptionData::kCommandName,
            gFeatureFlagFLE2CleanupCommand.isEnabled(serverGlobalParams.featureCompatibility));

    uassert(7618804,
            str::stream() << CleanupStructuredEncryptionData::kCommandName
                          << " must be run through mongos in a sharded cluster",
            !ShardingState::get(opCtx)->enabled());

    // Since this command holds an IX lock on the DB and the global lock throughout
    // the lifetime of this operation, setFCV should not be allowed to abort the transaction
    // performing the cleanup. Otherwise, on retry, the transaction may attempt to
    // acquire the global lock in IX mode, while setFCV is already waiting to acquire it
    // in S mode, causing a deadlock.
    FixedFCVRegion fixedFcv(opCtx);

    const auto& edcNss = request.getNamespace();

    AutoGetDb autoDb(opCtx, edcNss.dbName(), MODE_IX);
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Database '" << edcNss.dbName().toStringForErrorMsg()
                          << "' does not exist",
            autoDb.getDb());
    Lock::CollectionLock edcLock(opCtx, edcNss, MODE_IS);

    // Validate the request and acquire the relevant namespaces
    EncryptedStateCollectionsNamespaces namespaces;
    {
        auto catalog = CollectionCatalog::get(opCtx);

        // Check the data collection exists and is not a view
        auto edc = catalog->lookupCollectionByNamespace(opCtx, edcNss);
        if (!edc) {
            uassert(ErrorCodes::CommandNotSupportedOnView,
                    "Cannot cleanup structured encryption data on a view",
                    !catalog->lookupView(opCtx, edcNss));
            uasserted(ErrorCodes::NamespaceNotFound,
                      str::stream()
                          << "Collection '" << edcNss.toStringForErrorMsg() << "' does not exist");
        }

        validateCleanupRequest(request, *edc);

        namespaces =
            uassertStatusOK(EncryptedStateCollectionsNamespaces::createFromDataCollection(*edc));
    }

    // Acquire exclusive lock on the associated 'ecoc.lock' namespace to serialize calls
    // to cleanup and compact on the same EDC namespace.
    Lock::CollectionLock compactionLock(opCtx, namespaces.ecocLockNss, MODE_X);

    LOGV2(7618805, "Cleaning up the encrypted compaction collection", logAttrs(edcNss));

    CleanupStats stats({}, {});
    FLECompactESCDeleteSet escDeleteSet;
    auto tagsPerDelete =
        ServerParameterSet::getClusterParameterSet()
            ->get<ClusterParameterWithStorage<FLECompactionOptions>>("fleCompactionOptions")
            ->getValue(boost::none)
            .getMaxESCEntriesPerCompactionDelete();

    // If 'esc.deletes' exists, clean up the matching anchors in ESC and drop 'esc.deletes'
    {
        AutoGetCollection escDeletes(opCtx, namespaces.escDeletesNss, MODE_IS);
        if (escDeletes) {
            LOGV2(7618806,
                  "Cleaning up ESC deletes collection from a prior cleanup operation",
                  logAttrs(namespaces.escDeletesNss));
            cleanupESCAnchors(
                opCtx, namespaces.escNss, namespaces.escDeletesNss, tagsPerDelete, &stats.getEsc());
        }
    }
    dropQEStateCollection(opCtx, namespaces.escDeletesNss);

    bool createEcoc = false;
    bool renameEcoc = false;
    {
        AutoGetCollection ecoc(opCtx, namespaces.ecocNss, MODE_IS);
        AutoGetCollection ecocCompact(opCtx, namespaces.ecocRenameNss, MODE_IS);

        // Early exit if there's no ECOC
        if (!ecoc && !ecocCompact) {
            LOGV2(7618807,
                  "Skipping cleanup as there is no ECOC collection to compact",
                  "ecocNss"_attr = namespaces.ecocNss,
                  "ecocCompactNss"_attr = namespaces.ecocRenameNss);
            return stats;
        }

        createEcoc = !ecoc;

        // Set up the temporary 'ecoc.compact' collection
        if (ecoc && !ecocCompact) {
            // Load the random set of ESC non-anchor entries to be deleted post-cleanup
            auto memoryLimit =
                ServerParameterSet::getClusterParameterSet()
                    ->get<ClusterParameterWithStorage<FLECompactionOptions>>("fleCompactionOptions")
                    ->getValue(boost::none)
                    .getMaxCompactionSize();
            escDeleteSet =
                readRandomESCNonAnchorIds(opCtx, namespaces.escNss, memoryLimit, &stats.getEsc());
            renameEcoc = createEcoc = true;
        } else /* ecocCompact exists */ {
            LOGV2(7618808,
                  "Resuming compaction from a stale ECOC collection",
                  logAttrs(namespaces.ecocRenameNss));
        }
    }

    if (renameEcoc) {
        LOGV2(7618809,
              "Renaming the encrypted compaction collection",
              "ecocNss"_attr = namespaces.ecocNss,
              "ecocRenameNss"_attr = namespaces.ecocRenameNss);
        RenameCollectionOptions renameOpts;
        validateAndRunRenameCollection(
            opCtx, namespaces.ecocNss, namespaces.ecocRenameNss, renameOpts);
    }

    if (createEcoc) {
        createQEClusteredStateCollection(opCtx, namespaces.ecocNss);
    }

    // Create the temporary 'esc.deletes' clustered collection
    createQEClusteredStateCollection(opCtx, namespaces.escDeletesNss);

    {
        AutoGetCollection ecocCompact(opCtx, namespaces.ecocRenameNss, MODE_IS);
        AutoGetCollection escDeletes(opCtx, namespaces.escDeletesNss, MODE_IS);

        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "Renamed encrypted compaction collection "
                              << namespaces.ecocRenameNss.toStringForErrorMsg()
                              << " no longer exists prior to cleanup",
                ecocCompact.getCollection());
        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "ESC deletes collection "
                              << namespaces.escDeletesNss.toStringForErrorMsg()
                              << " no longer exists prior to cleanup",
                escDeletes.getCollection());

        // Clean up entries for each encrypted field in compactionTokens
        processFLECleanup(opCtx,
                          request,
                          &getTransactionWithRetriesForMongoD,
                          namespaces,
                          &stats.getEsc(),
                          &stats.getEcoc());

        // Delete the entries in 'C' from the ESC
        cleanupESCNonAnchors(
            opCtx, namespaces.escNss, escDeleteSet, tagsPerDelete, &stats.getEsc());

        // Delete the entries in esc.deletes collection from the ESC
        cleanupESCAnchors(
            opCtx, namespaces.escNss, namespaces.escDeletesNss, tagsPerDelete, &stats.getEsc());
    }

    // Drop the 'esc.deletes' collection
    dropQEStateCollection(opCtx, namespaces.escDeletesNss);

    // Drop the 'ecoc.compact' collection
    dropQEStateCollection(opCtx, namespaces.ecocRenameNss);

    LOGV2(7618810,
          "Done cleaning up the encrypted compaction collection",
          logAttrs(request.getNamespace()));

    FLEStatusSection::get().updateCleanupStats(stats);
    return stats;
}

class CleanupStructuredEncryptionDataCmd final
    : public TypedCommand<CleanupStructuredEncryptionDataCmd> {
public:
    using Request = CleanupStructuredEncryptionData;
    using Reply = CleanupStructuredEncryptionData::Reply;
    using TC = TypedCommand<CleanupStructuredEncryptionDataCmd>;

    class Invocation final : public TC::InvocationBase {
    public:
        using TC::InvocationBase::InvocationBase;
        using TC::InvocationBase::request;

        Reply typedRun(OperationContext* opCtx) {
            return Reply(cleanupEncryptedCollection(opCtx, request()));
        }

    private:
        bool supportsWriteConcern() const final {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            auto* as = AuthorizationSession::get(opCtx->getClient());
            uassert(ErrorCodes::Unauthorized,
                    "Not authorized to cleanup structured encryption data",
                    as->isAuthorizedForActionsOnResource(
                        ResourcePattern::forExactNamespace(request().getNamespace()),
                        ActionType::cleanupStructuredEncryptionData));
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

    std::set<StringData> sensitiveFieldNames() const final {
        return {CleanupStructuredEncryptionData::kCleanupTokensFieldName};
    }
} cleanupStructuredEncryptionDataCmd;


}  // namespace

}  // namespace mongo
