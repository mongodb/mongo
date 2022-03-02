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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/compact_structured_encryption_data_coordinator.h"

#include "mongo/base/checked_cast.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/commands/rename_collection_gen.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/drop_gen.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace {

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

void doRenameOperation(const CompactStructuredEncryptionDataState& state) {
    const auto& ecocNss = state.getEcocNss();
    auto opCtx = cc().makeOperationContext();
    auto catalog = CollectionCatalog::get(opCtx.get());
    auto uuid = catalog->lookupUUIDByNSS(opCtx.get(), ecocNss);
    if (!uuid) {
        // Nothing to rename.
        LOGV2_DEBUG(6350492,
                    5,
                    "Skipping rename stage as there is no source collection",
                    "ecocNss"_attr = ecocNss);
        return;
    } else if (uuid.get() != state.getEcocUuid()) {
        // The generation of the collection to be compacted is different than the one which was
        // requested.
        LOGV2_DEBUG(6350491,
                    5,
                    "Skipping rename of mismatched collection uuid",
                    "ecocNss"_attr = ecocNss,
                    "uuid"_attr = uuid.get(),
                    "expectedUUID"_attr = state.getEcocUuid());
        return;
    }

    // Otherwise, perform the rename so long as the target namespace does not exist.
    RenameCollectionCommand cmd(ecocNss, state.getEcocRenameNss());
    cmd.setDropTarget(false);
    cmd.setCollectionUUID(state.getEcocUuid());

    doRunCommand(opCtx.get(), ecocNss.db(), cmd);
}

void doDropOperation(const CompactStructuredEncryptionDataState& state) {
    auto ecocNss = state.getEcocRenameNss();
    Drop cmd(ecocNss);
    cmd.setCollectionUUID(state.getEcocUuid());
    auto opCtx = cc().makeOperationContext();
    doRunCommand(opCtx.get(), ecocNss.db(), cmd);
}

}  // namespace

boost::optional<BSONObj> CompactStructuredEncryptionDataCoordinator::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept {
    BSONObjBuilder bob;
    bob.append("type", "op");
    bob.append("desc", "CompactStructuredEncryptionDataCoordinator");
    bob.append("op", "command");
    bob.append("nss", _doc.getId().getNss().ns());
    bob.append("ecocNss", _doc.getEcocNss().ns());
    bob.append("ecocUuid", _doc.getEcocUuid().toString());
    bob.append("ecocRenameNss", _doc.getEcocRenameNss().ns());
    bob.append("currentPhase", _doc.getPhase());
    bob.append("active", true);
    return bob.obj();
}

void CompactStructuredEncryptionDataCoordinator::_enterPhase(Phase newPhase) {
    StateDoc doc(_doc);
    doc.setPhase(newPhase);

    LOGV2_DEBUG(6350490,
                2,
                "Transitioning phase for CompactStructuredEncryptionDataCoordinator",
                "nss"_attr = _doc.getId().getNss().ns(),
                "ecocNss"_attr = _doc.getEcocNss().ns(),
                "ecocUuid"_attr = _doc.getEcocUuid(),
                "ecocRenameNss"_attr = _doc.getEcocRenameNss().ns(),
                "compactionTokens"_attr = _doc.getCompactionTokens(),
                "oldPhase"_attr = CompactStructuredEncryptionDataPhase_serializer(_doc.getPhase()),
                "newPhase"_attr = CompactStructuredEncryptionDataPhase_serializer(newPhase));

    if (_doc.getPhase() == Phase::kRenameEcocForCompact) {
        _doc = _insertStateDocument(std::move(doc));
        return;
    }
    auto opCtx = cc().makeOperationContext();
    _doc = _updateStateDocument(opCtx.get(), std::move(doc));
}

ExecutorFuture<void> CompactStructuredEncryptionDataCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then(_executePhase(Phase::kRenameEcocForCompact, doRenameOperation))
        .then(_executePhase(Phase::kCompactStructuredEncryptionData,
                            [this, anchor = shared_from_this()](const auto&) {
                                // This will be implemented in a later phase.
                                ECOCStats ecocStats(1, 2);
                                ECStats eccStats(3, 4, 5, 6);
                                ECStats escStats(7, 8, 9, 10);
                                _response = CompactStats(ecocStats, eccStats, escStats);
                            }))
        .then(_executePhase(Phase::kDropTempCollection, doDropOperation));
}

}  // namespace mongo
