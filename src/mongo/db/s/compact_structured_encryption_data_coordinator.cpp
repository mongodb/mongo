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


#include <boost/none.hpp>
#include <boost/smart_ptr.hpp>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/crypto/fle_options_gen.h"
#include "mongo/crypto/fle_stats.h"
#include "mongo/db/catalog/clustered_collection_options_gen.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/create_gen.h"
#include "mongo/db/commands/rename_collection_gen.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/drop_gen.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/s/compact_structured_encryption_data_coordinator.h"
#include "mongo/db/s/sharding_ddl_coordinator.h"
#include "mongo/db/s/sharding_ddl_coordinator_gen.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/server_parameter_with_storage.h"
#include "mongo/db/tenant_id.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/namespace_string_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(fleCompactHangAfterDropTempCollection);
MONGO_FAIL_POINT_DEFINE(fleCompactHangBeforeECOCCreate);
MONGO_FAIL_POINT_DEFINE(fleCompactHangAfterECOCCreate);
MONGO_FAIL_POINT_DEFINE(fleCompactHangBeforeESCCleanup);
MONGO_FAIL_POINT_DEFINE(fleCompactSkipECOCDrop);

const auto kMajorityWriteConcern = BSON("writeConcern" << BSON("w"
                                                               << "majority"));
/**
 * Issue a simple success/fail command such as renameCollection or drop
 * using majority write concern.
 */
template <typename Request>
Status doRunCommand(OperationContext* opCtx, const DatabaseName& dbname, const Request& request) {
    DBDirectClient client(opCtx);
    BSONObj cmd = request.toBSON(kMajorityWriteConcern);
    auto reply = client
                     .runCommand(OpMsgRequestBuilder::create(
                         auth::ValidatedTenancyScope::get(opCtx), dbname, cmd))
                     ->getCommandReply();
    return getStatusFromCommandResult(reply);
}

template <class CompactionState>
void checkRequiredOperation(const CompactionState& state,
                            OperationContext* opCtx,
                            bool* needRename,
                            bool* needCreate,
                            bool* needCompact) {
    const auto& ecocNss = state.getEcocNss();
    const auto& ecocRenameNss = state.getEcocRenameNss();

    auto catalog = CollectionCatalog::get(opCtx);
    auto ecocUuid = catalog->lookupUUIDByNSS(opCtx, ecocNss);
    auto ecocRenameUuid = catalog->lookupUUIDByNSS(opCtx, ecocRenameNss);

    auto hasEcocBefore = state.getEcocUuid().has_value();
    auto hasEcocNow = !!ecocUuid;
    auto hasEcocRenameBefore = state.getEcocRenameUuid().has_value();
    auto hasEcocRenameNow = !!ecocRenameUuid;

    *needRename = false;
    *needCreate = false;
    *needCompact = true;

    if (hasEcocRenameBefore != hasEcocRenameNow) {
        LOGV2_DEBUG(6517001,
                    1,
                    "Skipping compaction due to change in collection state",
                    "ecocRenameNss"_attr = ecocRenameNss,
                    "hasEcocRenameBefore"_attr = hasEcocRenameBefore,
                    "hasEcocRenameNow"_attr = hasEcocRenameNow);
        // skip compact; no need to rename or create ECOC
        *needCompact = false;
        return;
    }

    if (hasEcocRenameNow) {
        if (ecocRenameUuid.value() != state.getEcocRenameUuid().value()) {
            LOGV2_DEBUG(6517002,
                        1,
                        "Skipping compaction due to mismatched collection uuid",
                        "ecocRenameNss"_attr = ecocRenameNss,
                        "uuid"_attr = ecocRenameUuid.value(),
                        "expectedUUID"_attr = state.getEcocRenameUuid().value());
            *needCompact = false;
        }
        // If the ECOC does not exist, create it
        *needCreate = !hasEcocNow;
        // The temp ECOC from a previous compact still exists, so no need to rename.
        // Compaction will resume using the existing temp ECOC.
        return;
    }

    if (!hasEcocNow) {
        // Nothing to rename & there's no existing temp ECOC, so skip compact.
        LOGV2_DEBUG(6350492,
                    1,
                    "Skipping rename stage as there is no source collection",
                    "ecocNss"_attr = ecocNss);
        *needCompact = false;
    } else if (!hasEcocBefore) {
        // Mismatch of before/after state, so skip rename & compact.
        LOGV2_DEBUG(6517003,
                    1,
                    "Skipping compaction due to change in collection state",
                    "ecocNss"_attr = ecocNss);
        *needCompact = false;
    } else if (ecocUuid.value() != state.getEcocUuid().value()) {
        // The generation of the collection to be compacted is different than the one which was
        // requested. Skip rename & compact.
        LOGV2_DEBUG(6350491,
                    1,
                    "Skipping rename of mismatched collection uuid",
                    "ecocNss"_attr = ecocNss,
                    "uuid"_attr = ecocUuid.value(),
                    "expectedUUID"_attr = state.getEcocUuid().value());
        *needCompact = false;
    } else {
        // ECOC is safe to rename & create; compact can be performed
        *needRename = true;
        *needCreate = true;
    }
}

template <class CompactionState>
bool doRenameOperation(const CompactionState& state,
                       boost::optional<UUID>* newEcocRenameUuid,
                       FLECompactESCDeleteSet* escDeleteSet,
                       ECStats* escStats) {
    const auto& ecocNss = state.getEcocNss();
    const auto& ecocRenameNss = state.getEcocRenameNss();
    auto opCtx = cc().makeOperationContext();

    bool needRename, needCreate, needCompact;
    checkRequiredOperation(state, opCtx.get(), &needRename, &needCreate, &needCompact);

    *newEcocRenameUuid = state.getEcocRenameUuid();

    if (needRename) {
        invariant(needCreate);

        if (escDeleteSet) {
            // (v2 only) load the random set of ESC non-anchor entries to be deleted post-compact
            auto memoryLimit =
                ServerParameterSet::getClusterParameterSet()
                    ->get<ClusterParameterWithStorage<FLECompactionOptions>>("fleCompactionOptions")
                    ->getValue(boost::none)
                    .getMaxCompactionSize();

            *escDeleteSet =
                readRandomESCNonAnchorIds(opCtx.get(), state.getEscNss(), memoryLimit, escStats);
        }

        // Perform the rename so long as the target namespace does not exist.
        RenameCollectionCommand cmd(ecocNss, ecocRenameNss);
        cmd.setDropTarget(false);
        cmd.setCollectionUUID(state.getEcocUuid().value());

        uassertStatusOK(doRunCommand(opCtx.get(), ecocNss.dbName(), cmd));
        *newEcocRenameUuid = state.getEcocUuid();
    }

    if (needCreate) {
        if (MONGO_unlikely(fleCompactHangBeforeECOCCreate.shouldFail())) {
            LOGV2(7140500, "Hanging due to fleCompactHangBeforeECOCCreate fail point");
            fleCompactHangBeforeECOCCreate.pauseWhileSet();
        }

        // Create the new ECOC collection
        CreateCommand createCmd(ecocNss);
        mongo::ClusteredIndexSpec clusterIdxSpec(BSON("_id" << 1), true);
        CreateCollectionRequest request;
        request.setClusteredIndex(
            std::variant<bool, mongo::ClusteredIndexSpec>(std::move(clusterIdxSpec)));
        createCmd.setCreateCollectionRequest(std::move(request));
        auto status = doRunCommand(opCtx.get(), ecocNss.dbName(), createCmd);
        if (!status.isOK()) {
            if (status != ErrorCodes::NamespaceExists) {
                uassertStatusOK(status);
            }
            LOGV2_DEBUG(7299603,
                        1,
                        "Create collection failed because namespace already exists",
                        "namespace"_attr = ecocNss);
        }

        if (MONGO_unlikely(fleCompactHangAfterECOCCreate.shouldFail())) {
            LOGV2(7140501, "Hanging due to fleCompactHangAfterECOCCreate fail point");
            fleCompactHangAfterECOCCreate.pauseWhileSet();
        }
    }

    // returns whether we can skip compaction
    return !needCompact;
}

void doCompactOperation(const CompactStructuredEncryptionDataState& state,
                        const FLECompactESCDeleteSet& escDeleteSet,
                        ECStats* escStats,
                        ECOCStats* ecocStats) {
    if (state.getSkipCompact()) {
        LOGV2_DEBUG(7472703, 1, "Skipping compaction");
        return;
    }

    EncryptedStateCollectionsNamespaces namespaces;
    namespaces.edcNss = state.getId().getNss();
    namespaces.escNss = state.getEscNss();
    namespaces.ecocNss = state.getEcocNss();
    namespaces.ecocRenameNss = state.getEcocRenameNss();
    auto opCtx = cc().makeOperationContext();
    CompactStructuredEncryptionData request(namespaces.edcNss, state.getCompactionTokens());
    request.setEncryptionInformation(state.getEncryptionInformation());
    request.setAnchorPaddingFactor(state.getAnchorPaddingFactor());

    processFLECompactV2(
        opCtx.get(), request, &getTransactionWithRetriesForMongoS, namespaces, escStats, ecocStats);

    if (MONGO_unlikely(fleCompactHangBeforeESCCleanup.shouldFail())) {
        LOGV2(7472702, "Hanging due to fleCompactHangBeforeESCCleanup fail point");
        fleCompactHangBeforeESCCleanup.pauseWhileSet();
    }

    auto tagsPerDelete =
        ServerParameterSet::getClusterParameterSet()
            ->get<ClusterParameterWithStorage<FLECompactionOptions>>("fleCompactionOptions")
            ->getValue(boost::none)
            .getMaxESCEntriesPerCompactionDelete();

    cleanupESCNonAnchors(opCtx.get(), namespaces.escNss, escDeleteSet, tagsPerDelete, escStats);
}

template <class State>
void doDropOperation(const State& state) {
    if (state.getSkipCompact()) {
        LOGV2_DEBUG(6517006, 1, "Skipping drop of temporary encrypted compaction collection");
        return;
    }

    uassert(6517007,
            "Cannot drop temporary encrypted compaction collection due to missing collection UUID",
            state.getEcocRenameUuid().has_value());

    auto opCtx = cc().makeOperationContext();
    auto catalog = CollectionCatalog::get(opCtx.get());
    auto ecocNss = state.getEcocRenameNss();
    auto ecocUuid = catalog->lookupUUIDByNSS(opCtx.get(), ecocNss);

    if (!ecocUuid) {
        LOGV2_DEBUG(
            6790901,
            1,
            "Skipping drop operation as temporary encrypted compaction collection does not exist");
        return;
    }

    if (MONGO_unlikely(fleCompactSkipECOCDrop.shouldFail())) {
        LOGV2(7472704, "Skipping drop of ECOC temp due to fleCompactSkipECOCDrop fail point");
        return;
    }

    Drop cmd(ecocNss);
    cmd.setCollectionUUID(state.getEcocRenameUuid().value());
    uassertStatusOK(doRunCommand(opCtx.get(), ecocNss.dbName(), cmd));
}

}  // namespace

boost::optional<BSONObj> CompactStructuredEncryptionDataCoordinator::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept {
    auto bob = basicReportBuilder();

    stdx::lock_guard lg{_docMutex};
    bob.append(
        "escNss",
        NamespaceStringUtil::serialize(_doc.getEscNss(), SerializationContext::stateDefault()));
    bob.append(
        "ecocNss",
        NamespaceStringUtil::serialize(_doc.getEcocNss(), SerializationContext::stateDefault()));
    bob.append("ecocUuid", _doc.getEcocUuid() ? _doc.getEcocUuid().value().toString() : "none");
    bob.append("ecocRenameNss",
               NamespaceStringUtil::serialize(_doc.getEcocRenameNss(),
                                              SerializationContext::stateDefault()));
    bob.append("ecocRenameUuid",
               _doc.getEcocRenameUuid() ? _doc.getEcocRenameUuid().value().toString() : "none");
    return bob.obj();
}

std::set<NamespaceString> CompactStructuredEncryptionDataCoordinator::_getAdditionalLocksToAcquire(
    OperationContext* opCtx) {
    return {_doc.getEcocNss(), _doc.getEscNss(), _doc.getEcocRenameNss()};
}

ExecutorFuture<void> CompactStructuredEncryptionDataCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then(_buildPhaseHandler(Phase::kRenameEcocForCompact,
                                 [this, anchor = shared_from_this()]() {
                                     // if this was resumed from an interrupt, the _escDeleteSet
                                     // might not be empty, so clear it.
                                     _escDeleteSet.clear();
                                     _escStats = ECStats();

                                     _skipCompact = doRenameOperation(
                                         _doc, &_ecocRenameUuid, &_escDeleteSet, &_escStats);

                                     stdx::lock_guard lg{_docMutex};
                                     _doc.setSkipCompact(_skipCompact);
                                     _doc.setEcocRenameUuid(_ecocRenameUuid);
                                     _doc.setEscStats(_escStats);
                                 }))
        .then(_buildPhaseHandler(
            Phase::kCompactStructuredEncryptionData,
            [this, anchor = shared_from_this()]() {
                _escStats = _doc.getEscStats().value_or(ECStats{});
                _ecocStats = _doc.getEcocStats().value_or(ECOCStats{});

                doCompactOperation(_doc, _escDeleteSet, &_escStats, &_ecocStats);

                FLEStatusSection::get().updateCompactionStats(CompactStats(_ecocStats, _escStats));

                stdx::lock_guard lg(_docMutex);
                _doc.setEscStats(_escStats);
                _doc.setEcocStats(_ecocStats);
            }))
        .then(_buildPhaseHandler(Phase::kDropTempCollection, [this, anchor = shared_from_this()] {
            _escStats = _doc.getEscStats().value_or(ECStats{});
            _ecocStats = _doc.getEcocStats().value_or(ECOCStats{});
            _response =
                CompactStructuredEncryptionDataCommandReply(CompactStats(_ecocStats, _escStats));
            doDropOperation(_doc);
            if (MONGO_unlikely(fleCompactHangAfterDropTempCollection.shouldFail())) {
                LOGV2(7472705, "Hanging due to fleCompactHangAfterDropTempCollection fail point");
                fleCompactHangAfterDropTempCollection.pauseWhileSet();
            }
        }));
}

}  // namespace mongo
