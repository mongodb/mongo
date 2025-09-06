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


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/crypto/fle_options_gen.h"
#include "mongo/crypto/fle_stats.h"
#include "mongo/crypto/fle_stats_gen.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/commands/fle2_compact.h"
#include "mongo/db/commands/fle2_compact_gen.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/clustered_collection_options_gen.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/create_collection.h"
#include "mongo/db/local_catalog/ddl/create_gen.h"
#include "mongo/db/local_catalog/ddl/drop_gen.h"
#include "mongo/db/local_catalog/ddl/replica_set_ddl_tracker.h"
#include "mongo/db/local_catalog/drop_collection.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/rename_collection.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/server_parameter_with_storage.h"
#include "mongo/db/service_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#include <memory>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

MONGO_FAIL_POINT_DEFINE(fleCompactHangBeforeECOCCreateUnsharded);
MONGO_FAIL_POINT_DEFINE(fleCompactSkipECOCDropUnsharded);

namespace mongo {
namespace {

CompactStats compactEncryptedCompactionCollection(OperationContext* opCtx,
                                                  const CompactStructuredEncryptionData& request) {
    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setShouldOmitDiagnosticInformation(lk, true);
    }

    uassert(6583201,
            str::stream() << CompactStructuredEncryptionData::kCommandName
                          << " must be run through mongos in a sharded cluster",
            !ShardingState::get(opCtx)->enabled());

    // Since this command holds an IX lock on the DB and the global lock throughout
    // the lifetime of this operation, setFCV should not be allowed to abort the transaction
    // performing the compaction. Otherwise, on retry, the transaction may attempt to
    // acquire the global lock in IX mode, while setFCV is already waiting to acquire it
    // in S mode, causing a deadlock.
    FixedFCVRegion fixedFcv(opCtx);

    const auto& edcNss = request.getNamespace();

    LOGV2(6319900, "Compacting the encrypted compaction collection", logAttrs(edcNss));

    AutoGetDb autoDb(opCtx, edcNss.dbName(), MODE_IX);
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Database '" << edcNss.dbName().toStringForErrorMsg()
                          << "' does not exist",
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
                  str::stream() << "Collection '" << edcNss.toStringForErrorMsg()
                                << "' does not exist");
    }

    validateCompactRequest(request, *edc);

    auto namespaces =
        uassertStatusOK(EncryptedStateCollectionsNamespaces::createFromDataCollection(*edc));

    ReplicaSetDDLTracker::ScopedReplicaSetDDL scopedReplicaSetDDL(
        opCtx,
        std::vector<NamespaceString>{namespaces.edcNss,
                                     namespaces.escNss,
                                     namespaces.ecocNss,
                                     namespaces.ecocRenameNss,
                                     namespaces.ecocLockNss});

    // Acquire exclusive lock on the associated 'ecoc.lock' namespace to serialize calls
    // to cleanup and compact on the same EDC namespace
    Lock::CollectionLock compactionLock(opCtx, namespaces.ecocLockNss, MODE_X);

    // Step 1: rename the ECOC collection if it exists
    catalog = CollectionCatalog::get(opCtx);
    auto ecoc = catalog->lookupCollectionByNamespace(opCtx, namespaces.ecocNss);
    auto ecocRename = catalog->lookupCollectionByNamespace(opCtx, namespaces.ecocRenameNss);

    CompactStats stats({}, {});
    FLECompactESCDeleteSet escDeleteSet;

    if (!ecoc && !ecocRename) {
        // nothing to compact
        LOGV2(6548306, "Skipping compaction as there is no ECOC collection to compact");
        return stats;
    } else if (ecoc && !ecocRename) {
        // load the random set of ESC non-anchor entries to be deleted post-compact.
        // This must be done before renaming the ECOC because if not, we can end up with
        // ESC entries that have no corresponding ECOC entry in the renamed ECOC.
        auto memoryLimit =
            ServerParameterSet::getClusterParameterSet()
                ->get<ClusterParameterWithStorage<FLECompactionOptions>>("fleCompactionOptions")
                ->getValue(boost::none)
                .getMaxCompactionSize();

        escDeleteSet =
            readRandomESCNonAnchorIds(opCtx, namespaces.escNss, memoryLimit, &stats.getEsc());

        LOGV2(7293603,
              "Renaming the encrypted compaction collection",
              "ecocNss"_attr = namespaces.ecocNss,
              "ecocRenameNss"_attr = namespaces.ecocRenameNss);
        RenameCollectionOptions renameOpts;
        validateAndRunRenameCollection(
            opCtx, namespaces.ecocNss, namespaces.ecocRenameNss, renameOpts);
        ecoc = nullptr;
    } else {
        LOGV2(7293610,
              "Resuming compaction from a stale ECOC collection",
              logAttrs(namespaces.ecocRenameNss));
    }

    if (!ecoc) {
        if (MONGO_unlikely(fleCompactHangBeforeECOCCreateUnsharded.shouldFail())) {
            LOGV2(7299601, "Hanging due to fleCompactHangBeforeECOCCreateUnsharded fail point");
            fleCompactHangBeforeECOCCreateUnsharded.pauseWhileSet();
        }

        // create ECOC
        CreateCommand createCmd(namespaces.ecocNss);
        mongo::ClusteredIndexSpec clusterIdxSpec(BSON("_id" << 1), true);
        CreateCollectionRequest request;
        request.setClusteredIndex(
            std::variant<bool, mongo::ClusteredIndexSpec>(std::move(clusterIdxSpec)));
        createCmd.setCreateCollectionRequest(std::move(request));
        auto status = createCollection(opCtx, createCmd);
        if (!status.isOK()) {
            if (status != ErrorCodes::NamespaceExists) {
                uassertStatusOK(status);
            }
            LOGV2_DEBUG(7299602,
                        1,
                        "Create collection failed because namespace already exists",
                        logAttrs(namespaces.ecocNss));
        }
    }

    // Step 2: for each encrypted field in compactionTokens, get distinct set of entries 'C'
    // from ECOC, and for each entry in 'C', compact ESC.
    {
        // acquire IS lock on the ecocRenameNss to prevent it from being dropped during compact
        AutoGetCollection tempEcocColl(opCtx, namespaces.ecocRenameNss, MODE_IS);

        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "Renamed encrypted compaction collection "
                              << namespaces.ecocRenameNss.toStringForErrorMsg()
                              << " no longer exists prior to compaction",
                tempEcocColl.getCollection());

        processFLECompactV2(opCtx,
                            request,
                            &getTransactionWithRetriesForMongoD,
                            namespaces,
                            &stats.getEsc(),
                            &stats.getEcoc());
    }

    auto tagsPerDelete =
        ServerParameterSet::getClusterParameterSet()
            ->get<ClusterParameterWithStorage<FLECompactionOptions>>("fleCompactionOptions")
            ->getValue(boost::none)
            .getMaxESCEntriesPerCompactionDelete();
    cleanupESCNonAnchors(opCtx, namespaces.escNss, escDeleteSet, tagsPerDelete, &stats.getEsc());
    FLEStatusSection::get().updateCompactionStats(stats);

    if (MONGO_unlikely(fleCompactSkipECOCDropUnsharded.shouldFail())) {
        LOGV2(7299612,
              "Skipping drop of ECOC temp due to fleCompactSkipECOCDropUnsharded fail point");
        return stats;
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
          logAttrs(request.getNamespace()));
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

    std::set<StringData> sensitiveFieldNames() const final {
        return {CompactStructuredEncryptionData::kCompactionTokensFieldName};
    }
};
MONGO_REGISTER_COMMAND(CompactStructuredEncryptionDataCmd).forShard();

}  // namespace
}  // namespace mongo
