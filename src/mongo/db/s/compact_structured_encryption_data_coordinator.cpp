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

#include "mongo/db/s/compact_structured_encryption_data_coordinator.h"

#include "mongo/base/checked_cast.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/commands/create_gen.h"
#include "mongo/db/commands/fle2_compact.h"
#include "mongo/db/commands/rename_collection_gen.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/drop_gen.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(fleCompactHangAfterDropTempCollection);
MONGO_FAIL_POINT_DEFINE(fleCompactHangBeforeECOCCreate);
MONGO_FAIL_POINT_DEFINE(fleCompactHangAfterECOCCreate);

const auto kMajorityWriteConcern = BSON("writeConcern" << BSON("w"
                                                               << "majority"));
/**
 * Issue a simple success/fail command such as renameCollection or drop
 * using majority write concern.
 */
template <typename Request>
void doRunCommand(OperationContext* opCtx, StringData dbname, const Request& request) {
    DBDirectClient client(opCtx);
    BSONObj cmd = request.toBSON(kMajorityWriteConcern);
    auto reply = client.runCommand(OpMsgRequest::fromDBAndBody(dbname, cmd))->getCommandReply();
    uassertStatusOK(getStatusFromCommandResult(reply));
}

void doRenameOperation(const CompactStructuredEncryptionDataState& state,
                       bool* skipCompact,
                       boost::optional<UUID>* newEcocRenameUuid) {
    const auto& ecocNss = state.getEcocNss();
    const auto& ecocRenameNss = state.getEcocRenameNss();
    auto opCtx = cc().makeOperationContext();
    auto catalog = CollectionCatalog::get(opCtx.get());
    auto ecocUuid = catalog->lookupUUIDByNSS(opCtx.get(), ecocNss);
    auto ecocRenameUuid = catalog->lookupUUIDByNSS(opCtx.get(), ecocRenameNss);

    auto hasEcocBefore = state.getEcocUuid().has_value();
    auto hasEcocNow = !!ecocUuid;
    auto hasEcocRenameBefore = state.getEcocRenameUuid().has_value();
    auto hasEcocRenameNow = !!ecocRenameUuid;

    *skipCompact = false;
    *newEcocRenameUuid = state.getEcocRenameUuid();

    if (hasEcocRenameBefore != hasEcocRenameNow) {
        LOGV2_DEBUG(6517001,
                    1,
                    "Skipping compaction due to change in collection state",
                    "ecocRenameNss"_attr = ecocRenameNss,
                    "hasEcocRenameBefore"_attr = hasEcocRenameBefore,
                    "hasEcocRenameNow"_attr = hasEcocRenameNow);
        *skipCompact = true;
        return;
    } else if (hasEcocRenameNow) {
        if (ecocRenameUuid.value() != state.getEcocRenameUuid().value()) {
            LOGV2_DEBUG(6517002,
                        1,
                        "Skipping compaction due to mismatched collection uuid",
                        "ecocRenameNss"_attr = ecocRenameNss,
                        "uuid"_attr = ecocRenameUuid.value(),
                        "expectedUUID"_attr = state.getEcocRenameUuid().value());
            *skipCompact = true;
            return;
        }
        // The temp ECOC from a previous compact still exists, so no need to rename.
        if (hasEcocNow) {
            // The ECOC already exists, so skip explicitly creating it.
            return;
        }
    } else {
        if (!hasEcocNow) {
            // Nothing to rename.
            LOGV2_DEBUG(6350492,
                        1,
                        "Skipping rename stage as there is no source collection",
                        "ecocNss"_attr = ecocNss);
            *skipCompact = true;
            return;
        } else if (!hasEcocBefore) {
            // mismatch of before/after state
            LOGV2_DEBUG(6517003,
                        1,
                        "Skipping compaction due to change in collection state",
                        "ecocNss"_attr = ecocNss);
            *skipCompact = true;
            return;
        } else if (ecocUuid.value() != state.getEcocUuid().value()) {
            // The generation of the collection to be compacted is different than the one which was
            // requested.
            LOGV2_DEBUG(6350491,
                        1,
                        "Skipping rename of mismatched collection uuid",
                        "ecocNss"_attr = ecocNss,
                        "uuid"_attr = ecocUuid.value(),
                        "expectedUUID"_attr = state.getEcocUuid().value());
            *skipCompact = true;
            return;
        }

        LOGV2(6517004,
              "Renaming the encrypted compaction collection",
              "ecocNss"_attr = ecocNss,
              "ecocUuid"_attr = ecocUuid.value(),
              "ecocRenameNss"_attr = ecocRenameNss);

        // Perform the rename so long as the target namespace does not exist.
        RenameCollectionCommand cmd(ecocNss, ecocRenameNss);
        cmd.setDropTarget(false);
        cmd.setCollectionUUID(state.getEcocUuid().value());

        doRunCommand(opCtx.get(), ecocNss.db(), cmd);
        *newEcocRenameUuid = state.getEcocUuid();
    }

    if (MONGO_unlikely(fleCompactHangBeforeECOCCreate.shouldFail())) {
        LOGV2(7140500, "Hanging due to fleCompactHangBeforeECOCCreate fail point");
        fleCompactHangBeforeECOCCreate.pauseWhileSet();
    }

    // Create the new ECOC collection
    CreateCommand createCmd(ecocNss);
    mongo::ClusteredIndexSpec clusterIdxSpec(BSON("_id" << 1), true);
    createCmd.setClusteredIndex(
        stdx::variant<bool, mongo::ClusteredIndexSpec>(std::move(clusterIdxSpec)));
    doRunCommand(opCtx.get(), ecocNss.db(), createCmd);

    if (MONGO_unlikely(fleCompactHangAfterECOCCreate.shouldFail())) {
        LOGV2(7140501, "Hanging due to fleCompactHangAfterECOCCreate fail point");
        fleCompactHangAfterECOCCreate.pauseWhileSet();
    }
}

CompactStats doCompactOperation(const CompactStructuredEncryptionDataState& state) {
    if (state.getSkipCompact()) {
        LOGV2_DEBUG(6517005, 1, "Skipping compaction");
        return CompactStats(ECOCStats(), ECStats(), ECStats());
    }

    EncryptedStateCollectionsNamespaces namespaces;
    namespaces.edcNss = state.getId().getNss();
    namespaces.eccNss = state.getEccNss();
    namespaces.escNss = state.getEscNss();
    namespaces.ecocNss = state.getEcocNss();
    namespaces.ecocRenameNss = state.getEcocRenameNss();
    auto opCtx = cc().makeOperationContext();
    CompactStructuredEncryptionData request(namespaces.edcNss, state.getCompactionTokens());

    return processFLECompact(opCtx.get(), request, &getTransactionWithRetriesForMongoS, namespaces);
}

void doDropOperation(const CompactStructuredEncryptionDataState& state) {
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

    Drop cmd(ecocNss);
    cmd.setCollectionUUID(state.getEcocRenameUuid().value());
    doRunCommand(opCtx.get(), ecocNss.db(), cmd);
}

}  // namespace

boost::optional<BSONObj> CompactStructuredEncryptionDataCoordinator::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept {
    auto bob = basicReportBuilder();

    stdx::lock_guard lg{_docMutex};
    bob.append("escNss", _doc.getEscNss().ns());
    bob.append("eccNss", _doc.getEccNss().ns());
    bob.append("ecocNss", _doc.getEcocNss().ns());
    bob.append("ecocUuid", _doc.getEcocUuid() ? _doc.getEcocUuid().value().toString() : "none");
    bob.append("ecocRenameNss", _doc.getEcocRenameNss().ns());
    bob.append("ecocRenameUuid",
               _doc.getEcocRenameUuid() ? _doc.getEcocRenameUuid().value().toString() : "none");
    return bob.obj();
}

// TODO: SERVER-68373 remove once 7.0 becomes last LTS
void CompactStructuredEncryptionDataCoordinator::_enterPhase(const Phase& newPhase) {
    // Before 6.1, this coordinator persists the result of the doCompactOperation()
    // by reusing the compactionTokens field to store the _response BSON.
    // If newPhase is kDropTempCollection, this override of _enterPhase performs this
    // replacement on the in-memory state document (_doc), before calling the base _enterPhase()
    // which persists _doc to disk. In the event that updating the persisted document fails,
    // the replaced compaction tokens are restored in _doc.
    using Base = RecoverableShardingDDLCoordinator<CompactStructuredEncryptionDataState,
                                                   CompactStructuredEncryptionDataPhaseEnum>;
    bool useOverload = _isPre61Compatible() && (newPhase == Phase::kDropTempCollection);

    if (useOverload) {
        BSONObj compactionTokensCopy;
        {
            stdx::lock_guard lg(_docMutex);
            compactionTokensCopy = _doc.getCompactionTokens().getOwned();
            _doc.setCompactionTokens(_response->toBSON());
        }

        try {
            Base::_enterPhase(newPhase);
        } catch (...) {
            // on error, restore the compaction tokens
            stdx::lock_guard lg(_docMutex);
            _doc.setCompactionTokens(std::move(compactionTokensCopy));
            throw;
        }
    } else {
        Base::_enterPhase(newPhase);
    }
}

ExecutorFuture<void> CompactStructuredEncryptionDataCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then(_buildPhaseHandler(Phase::kRenameEcocForCompact,
                                 [this, anchor = shared_from_this()]() {
                                     doRenameOperation(_doc, &_skipCompact, &_ecocRenameUuid);
                                     stdx::unique_lock ul{_docMutex};
                                     _doc.setSkipCompact(_skipCompact);
                                     _doc.setEcocRenameUuid(_ecocRenameUuid);
                                 }))
        .then(_buildPhaseHandler(Phase::kCompactStructuredEncryptionData,
                                 [this, anchor = shared_from_this()]() {
                                     _response = doCompactOperation(_doc);
                                     if (!_isPre61Compatible()) {
                                         stdx::lock_guard lg(_docMutex);
                                         _doc.setResponse(_response);
                                     }
                                 }))
        .then(_buildPhaseHandler(Phase::kDropTempCollection, [this, anchor = shared_from_this()] {
            if (!_isPre61Compatible()) {
                invariant(_doc.getResponse());
                _response = *_doc.getResponse();
            } else {
                try {
                    // restore the response that was stored in the compactionTokens field
                    IDLParserContext ctxt("response");
                    _response = CompactStructuredEncryptionDataCommandReply::parse(
                        ctxt, _doc.getCompactionTokens());
                } catch (...) {
                    LOGV2_ERROR(6846101,
                                "Failed to parse response from "
                                "CompactStructuredEncryptionDataState document",
                                "response"_attr = _doc.getCompactionTokens());
                    // ignore for compatibility with 6.0.0
                }
            }

            doDropOperation(_doc);
            if (MONGO_unlikely(fleCompactHangAfterDropTempCollection.shouldFail())) {
                LOGV2(6790902, "Hanging due to fleCompactHangAfterDropTempCollection fail point");
                fleCompactHangAfterDropTempCollection.pauseWhileSet();
            }
        }));
}

}  // namespace mongo
