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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_runtime.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/client_cursor/cursor_manager.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/range_deleter_service.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/future.h"
#include "mongo/util/str.h"

#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kShardingRangeDeleter

namespace mongo {
namespace {

void registerTaskWithOngoingQueriesOnOpLogEntryCommit(OperationContext* opCtx,
                                                      const RangeDeletionTask& rdt) {

    shard_role_details::getRecoveryUnit(opCtx)->onCommit([rdt](OperationContext* opCtx,
                                                               boost::optional<Timestamp>) {
        try {
            auto waitForActiveQueriesToComplete =
                CollectionShardingRuntime::acquireShared(opCtx, rdt.getNss())
                    ->getOngoingQueriesCompletionFuture(rdt.getCollectionUuid(), rdt.getRange())
                    .semi();
            if (!waitForActiveQueriesToComplete.isReady()) {
                const auto openCursorsIds =
                    CursorManager::get(opCtx)->getCursorIdsForNamespace(rdt.getNss());
                LOGV2_INFO(
                    6180600,
                    "Range deletion will be scheduled after all possibly dependent queries finish",
                    logAttrs(rdt.getNss()),
                    "collectionUUID"_attr = rdt.getCollectionUuid(),
                    "range"_attr = redact(rdt.getRange().toString()),
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

void invalidateRangePreservers(OperationContext* opCtx, const RangeDeletionTask& rdt) {
    auto preMigrationShardVersion = rdt.getPreMigrationShardVersion();

    if (preMigrationShardVersion && preMigrationShardVersion.get() != ChunkVersion::IGNORED()) {
        auto scopedScr = CollectionShardingRuntime::acquireExclusive(opCtx, rdt.getNss());
        scopedScr->invalidateRangePreserversOlderThanShardVersion(opCtx,
                                                                  preMigrationShardVersion.get());
    }
}

}  // namespace

RangeDeleterServiceOpObserver::RangeDeleterServiceOpObserver() = default;
RangeDeleterServiceOpObserver::~RangeDeleterServiceOpObserver() = default;

void RangeDeleterServiceOpObserver::onInserts(OperationContext* opCtx,
                                              const CollectionPtr& coll,
                                              std::vector<InsertStatement>::const_iterator begin,
                                              std::vector<InsertStatement>::const_iterator end,
                                              const std::vector<RecordId>& recordIds,
                                              std::vector<bool> fromMigrate,
                                              bool defaultFromMigrate,
                                              OpStateAccumulator* opAccumulator) {
    if (coll->ns() == NamespaceString::kRangeDeletionNamespace) {
        for (auto it = begin; it != end; ++it) {
            auto deletionTask = RangeDeletionTask::parse(
                it->doc, IDLParserContext("RangeDeleterServiceOpObserver"));
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
        const bool processingFieldUpdatedToTrue = [&] {
            const auto newValueProcessingForField = update_oplog_entry::extractNewValueForField(
                args.updateArgs->update, RangeDeletionTask::kProcessingFieldName);
            return (!newValueProcessingForField.eoo() && newValueProcessingForField.Bool());
        }();

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

        if (processingFieldUpdatedToTrue || pendingFieldIsRemoved || pendingFieldUpdatedToFalse) {
            auto deletionTask = RangeDeletionTask::parse(
                args.updateArgs->updatedDoc, IDLParserContext("RangeDeleterServiceOpObserver"));
            if (processingFieldUpdatedToTrue) {
                // Invalidates all RangePreservers when shardPlacementVersion is lower than or
                // equal to the preMigrationShardVersion. This ensures that reads on secondaries are
                // terminated, preventing them from potentially targeting orphaned documents.
                invalidateRangePreservers(opCtx, deletionTask);
            }
            if (pendingFieldIsRemoved || pendingFieldUpdatedToFalse) {
                registerTaskWithOngoingQueriesOnOpLogEntryCommit(opCtx, deletionTask);
            }
        }
    }
}

}  // namespace mongo
