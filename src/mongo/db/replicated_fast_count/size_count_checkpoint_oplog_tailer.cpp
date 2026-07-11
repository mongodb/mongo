// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/replicated_fast_count/size_count_checkpoint_oplog_tailer.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo::replicated_fast_count::oplog_tailer {
namespace {

std::shared_ptr<CappedInsertNotifier> acquireInsertNotifier(OperationContext* opCtx) {
    AutoGetOplogFastPath oplogRead(opCtx, OplogAccessMode::kRead);
    const auto& oplogColl = oplogRead.getCollection();
    tassert(12101805, "oplog collection not found", oplogColl);
    return oplogColl->getRecordStore()->capped()->getInsertNotifier();
}
}  // namespace

void bufferNewOplogEntries(OperationContext* opCtx, SizeCountCheckpointBuffer& buffer) {
    RecoveryUnit* ru(shard_role_details::getRecoveryUnit(opCtx));

    writeConflictRetry(
        opCtx, *ru, "sizeCountOplogTailerScan", NamespaceString::kRsOplogNamespace, [&] {
            const AutoGetCollection oplog(opCtx, NamespaceString::kRsOplogNamespace, MODE_IS);
            tassert(12101803, "oplog collection not found", oplog);
            const auto cursor = oplog->getRecordStore()->getCursor(opCtx, *ru);
            buffer.scanToNoHolesEOF(*cursor);
        });

    ru->abandonSnapshot();
}

void run(OperationContext* opCtx, SizeCountCheckpointBuffer& buffer) {
    std::shared_ptr<CappedInsertNotifier> notifier;

    while (true) {
        try {
            if (!notifier) {
                notifier = acquireInsertNotifier(opCtx);
            }
            opCtx->checkForInterrupt();
            const uint64_t waitVersion = notifier->getVersion();

            bufferNewOplogEntries(opCtx, buffer);

            // Block until the next oplog insert. The version is captured before the scan, so any
            // insert before or during the scan causes waitUntil() to return immediately.
            notifier->waitUntil(opCtx, waitVersion, Date_t::max());
        } catch (const DBException& ex) {
            if (ex.code() == ErrorCodes::InterruptedDueToReplStateChange ||
                ErrorCodes::isShutdownError(ex.code())) {
                LOGV2_DEBUG(12917802,
                            2,
                            "SizeCountCheckpointOplogTailer interrupted",
                            "error"_attr = ex.toStatus());
                return;
            }
            LOGV2_WARNING(12917803,
                          "Unexpected exception handled in SizeCountCheckpointOplogTailer::run()",
                          "error"_attr = ex.toStatus());
        }
    }
}
}  // namespace mongo::replicated_fast_count::oplog_tailer
