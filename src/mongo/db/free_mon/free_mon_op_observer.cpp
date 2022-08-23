/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/free_mon/free_mon_op_observer.h"

#include "mongo/db/free_mon/free_mon_controller.h"
#include "mongo/db/free_mon/free_mon_storage.h"
#include "mongo/db/operation_context.h"

namespace mongo {
namespace {

bool isStandaloneOrPrimary(OperationContext* opCtx) {
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    const bool isReplSet =
        replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet;
    return !isReplSet ||
        (repl::ReplicationCoordinator::get(opCtx)->getMemberState() ==
         repl::MemberState::RS_PRIMARY);
}

const auto getFreeMonDeleteState = OperationContext::declareDecoration<bool>();

}  // namespace

FreeMonOpObserver::FreeMonOpObserver() = default;

FreeMonOpObserver::~FreeMonOpObserver() = default;

repl::OpTime FreeMonOpObserver::onDropCollection(OperationContext* opCtx,
                                                 const NamespaceString& collectionName,
                                                 const UUID& uuid,
                                                 std::uint64_t numRecords,
                                                 const CollectionDropType dropType) {
    if (collectionName == NamespaceString::kServerConfigurationNamespace) {
        auto controller = FreeMonController::get(opCtx->getServiceContext());

        if (controller != nullptr) {
            controller->notifyOnDelete();
        }
    }

    return {};
}

void FreeMonOpObserver::onInserts(OperationContext* opCtx,
                                  const CollectionPtr& coll,
                                  std::vector<InsertStatement>::const_iterator begin,
                                  std::vector<InsertStatement>::const_iterator end,
                                  bool fromMigrate) {
    if (coll->ns() != NamespaceString::kServerConfigurationNamespace) {
        return;
    }

    if (isStandaloneOrPrimary(opCtx)) {
        return;
    }

    for (auto it = begin; it != end; ++it) {
        const auto& insertedDoc = it->doc;

        if (auto idElem = insertedDoc["_id"]) {
            if (idElem.str() == FreeMonStorage::kFreeMonDocIdKey) {
                auto controller = FreeMonController::get(opCtx->getServiceContext());

                if (controller != nullptr) {
                    controller->notifyOnUpsert(insertedDoc.getOwned());
                }
            }
        }
    }
}

void FreeMonOpObserver::onUpdate(OperationContext* opCtx, const OplogUpdateEntryArgs& args) {
    if (args.nss != NamespaceString::kServerConfigurationNamespace) {
        return;
    }

    if (isStandaloneOrPrimary(opCtx)) {
        return;
    }

    if (args.updateArgs->updatedDoc["_id"].str() == FreeMonStorage::kFreeMonDocIdKey) {
        auto controller = FreeMonController::get(opCtx->getServiceContext());

        if (controller != nullptr) {
            controller->notifyOnUpsert(args.updateArgs->updatedDoc.getOwned());
        }
    }
}

void FreeMonOpObserver::aboutToDelete(OperationContext* opCtx,
                                      const NamespaceString& nss,
                                      const UUID& uuid,
                                      const BSONObj& doc) {

    bool isFreeMonDoc = (nss == NamespaceString::kServerConfigurationNamespace) &&
        (doc["_id"].str() == FreeMonStorage::kFreeMonDocIdKey);

    // Set a flag that indicates whether the document to be delete is the free monitoring state
    // document
    getFreeMonDeleteState(opCtx) = isFreeMonDoc;
}

void FreeMonOpObserver::onDelete(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 const UUID& uuid,
                                 StmtId stmtId,
                                 const OplogDeleteEntryArgs& args) {
    if (nss != NamespaceString::kServerConfigurationNamespace) {
        return;
    }

    if (isStandaloneOrPrimary(opCtx)) {
        return;
    }

    if (getFreeMonDeleteState(opCtx) == true) {
        auto controller = FreeMonController::get(opCtx->getServiceContext());

        if (controller != nullptr) {
            controller->notifyOnDelete();
        }
    }
}

void FreeMonOpObserver::_onReplicationRollback(OperationContext* opCtx,
                                               const RollbackObserverInfo& rbInfo) {
    // Invalidate any in-memory auth data if necessary.
    const auto& rollbackNamespaces = rbInfo.rollbackNamespaces;
    if (rollbackNamespaces.count(NamespaceString::kServerConfigurationNamespace) == 1) {
        auto controller = FreeMonController::get(opCtx->getServiceContext());

        if (controller != nullptr) {
            controller->notifyOnRollback();
        }
    }
}


}  // namespace mongo
