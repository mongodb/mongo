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
        }
        // the temp ecoc from a previous compact still exists, so skip rename
        return;
    }

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

    // Otherwise, perform the rename so long as the target namespace does not exist.
    RenameCollectionCommand cmd(ecocNss, ecocRenameNss);
    cmd.setDropTarget(false);
    cmd.setCollectionUUID(state.getEcocUuid().value());

    doRunCommand(opCtx.get(), ecocNss.db(), cmd);
    *newEcocRenameUuid = state.getEcocUuid();
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

ExecutorFuture<void> CompactStructuredEncryptionDataCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then(_executePhase(Phase::kRenameEcocForCompact,
                            [this, anchor = shared_from_this()]() {
                                doRenameOperation(_doc, &_skipCompact, &_ecocRenameUuid);
                                stdx::unique_lock ul{_docMutex};
                                _doc.setSkipCompact(_skipCompact);
                                _doc.setEcocRenameUuid(_ecocRenameUuid);
                            }))
        .then(_executePhase(Phase::kCompactStructuredEncryptionData,
                            [this, anchor = shared_from_this()]() {
                                _response = doCompactOperation(_doc);
                                if (!_isPre61Compatible()) {
                                    stdx::lock_guard lg(_docMutex);
                                    _doc.setResponse(_response);
                                }
                            }))
        .then(_executePhase(Phase::kDropTempCollection, [this, anchor = shared_from_this()] {
            if (!_isPre61Compatible()) {
                invariant(_doc.getResponse());
                _response = *_doc.getResponse();
            }
            doDropOperation(_doc);
            if (MONGO_unlikely(fleCompactHangAfterDropTempCollection.shouldFail())) {
                LOGV2(6790902, "Hanging due to fleCompactHangAfterDropTempCollection fail point");
                fleCompactHangAfterDropTempCollection.pauseWhileSet();
            }
        }));
}

}  // namespace mongo
