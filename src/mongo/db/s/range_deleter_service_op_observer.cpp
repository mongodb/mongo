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

#include "mongo/db/s/range_deleter_service_op_observer.h"

#include "mongo/db/catalog_raii.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/range_deleter_service.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kShardingRangeDeleter

namespace mongo {
namespace {
// Small hack used to be able to retrieve the full removed document in the `onDelete` method
const auto deletedDocumentDecoration = OperationContext::declareDecoration<BSONObj>();
void registerTaskWithOngoingQueriesOnOpLogEntryCommit(OperationContext* opCtx,
                                                      const RangeDeletionTask& rdt) {

    opCtx->recoveryUnit()->onCommit([rdt](OperationContext* opCtx, boost::optional<Timestamp>) {
        try {
            AutoGetCollection autoColl(opCtx, rdt.getNss(), MODE_IS);
            auto waitForActiveQueriesToComplete =
                CollectionShardingRuntime::assertCollectionLockedAndAcquireShared(opCtx,
                                                                                  rdt.getNss())
                    ->getOngoingQueriesCompletionFuture(rdt.getCollectionUuid(), rdt.getRange())
                    .semi();
            if (!waitForActiveQueriesToComplete.isReady()) {
                const auto openCursorsIds =
                    CursorManager::get(opCtx)->getCursorIdsForNamespace(rdt.getNss());
                LOGV2_INFO(
                    7179200,
                    "Range deletion will be scheduled after all possibly dependent queries finish",
                    logAttrs(rdt.getNss()),
                    "range"_attr = rdt.getRange(),
                    "cursorsDirectlyReferringTheNamespace"_attr = openCursorsIds);
            }
            (void)RangeDeleterService::get(opCtx)->registerTask(
                rdt, std::move(waitForActiveQueriesToComplete));
        } catch (const DBException& ex) {
            if (ex.code() != ErrorCodes::NotYetInitialized &&
                !ErrorCodes::isA<ErrorCategory::NotPrimaryError>(ex.code())) {
                LOGV2_WARNING(7092800,
                              "No error different from `NotYetInitialized` or `NotPrimaryError` "
                              "category is expected to be propagated to the range deleter "
                              "observer. Range deletion task not registered.",
                              "error"_attr = redact(ex),
                              "task"_attr = rdt);
            }
        }
    });
}

}  // namespace

RangeDeleterServiceOpObserver::RangeDeleterServiceOpObserver() = default;
RangeDeleterServiceOpObserver::~RangeDeleterServiceOpObserver() = default;

void RangeDeleterServiceOpObserver::onInserts(OperationContext* opCtx,
                                              const CollectionPtr& coll,
                                              std::vector<InsertStatement>::const_iterator begin,
                                              std::vector<InsertStatement>::const_iterator end,
                                              std::vector<bool> fromMigrate,
                                              bool defaultFromMigrate) {
    if (coll->ns() == NamespaceString::kRangeDeletionNamespace) {
        for (auto it = begin; it != end; ++it) {
            auto deletionTask = RangeDeletionTask::parse(
                IDLParserContext("RangeDeleterServiceOpObserver"), it->doc);
            if (!deletionTask.getPending() || !*(deletionTask.getPending())) {
                registerTaskWithOngoingQueriesOnOpLogEntryCommit(opCtx, deletionTask);
            }
        }
    }
}

void RangeDeleterServiceOpObserver::onUpdate(OperationContext* opCtx,
                                             const OplogUpdateEntryArgs& args,
                                             OpStateAccumulator* opAccumulator) {
    if (args.coll->ns() == NamespaceString::kRangeDeletionNamespace) {
        const bool pendingFieldIsRemoved = [&] {
            return update_oplog_entry::isFieldRemovedByUpdate(
                       args.updateArgs->update, RangeDeletionTask::kPendingFieldName) ==
                update_oplog_entry::FieldRemovedStatus::kFieldRemoved;
        }();

        const bool pendingFieldUpdatedToFalse = [&] {
            const auto newValueForPendingField = update_oplog_entry::extractNewValueForField(
                args.updateArgs->update, RangeDeletionTask::kPendingFieldName);
            return (!newValueForPendingField.eoo() && newValueForPendingField.Bool() == false);
        }();

        if (pendingFieldIsRemoved || pendingFieldUpdatedToFalse) {
            auto deletionTask = RangeDeletionTask::parse(
                IDLParserContext("RangeDeleterServiceOpObserver"), args.updateArgs->updatedDoc);
            registerTaskWithOngoingQueriesOnOpLogEntryCommit(opCtx, deletionTask);
        }
    }
}

void RangeDeleterServiceOpObserver::aboutToDelete(OperationContext* opCtx,
                                                  const CollectionPtr& coll,
                                                  BSONObj const& doc) {
    if (coll->ns() == NamespaceString::kRangeDeletionNamespace) {
        deletedDocumentDecoration(opCtx) = doc;
    }
}

void RangeDeleterServiceOpObserver::onDelete(OperationContext* opCtx,
                                             const CollectionPtr& coll,
                                             StmtId stmtId,
                                             const OplogDeleteEntryArgs& args,
                                             OpStateAccumulator* opAccumulator) {
    if (coll->ns() == NamespaceString::kRangeDeletionNamespace) {
        const auto& deletedDoc = deletedDocumentDecoration(opCtx);

        auto deletionTask =
            RangeDeletionTask::parse(IDLParserContext("RangeDeleterServiceOpObserver"), deletedDoc);
        try {
            RangeDeleterService::get(opCtx)->deregisterTask(deletionTask.getCollectionUuid(),
                                                            deletionTask.getRange());
        } catch (const DBException& ex) {
            dassert(ex.code() == ErrorCodes::NotYetInitialized,
                    str::stream() << "No error different from `NotYetInitialized` is expected "
                                     "to be propagated to the range deleter observer. Got error: "
                                  << ex.toStatus());
        }
    }
}

}  // namespace mongo
