// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/index_builds/duplicate_key_tracker.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/collection_crud/container_write.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index_builds/primary_driven/enabled.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/lazy_record_store.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/progress_meter.h"

#include <string_view>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex


namespace mongo {

namespace {
using namespace std::literals::string_view_literals;
static constexpr std::string_view kKeyField = "key"sv;
}  // namespace

DuplicateKeyTracker::DuplicateKeyTracker(OperationContext* opCtx,
                                         std::string_view ident,
                                         LazyRecordStore::CreateMode createMode)
    : _keyConstraintsTable(opCtx, ident, createMode) {}

void DuplicateKeyTracker::createDeferredTable(OperationContext* opCtx) {
    _keyConstraintsTable.getOrCreateTable(opCtx);
}

Status DuplicateKeyTracker::recordKey(OperationContext* opCtx,
                                      const CollectionPtr& coll,
                                      const IndexCatalogEntry* indexCatalogEntry,
                                      const key_string::View& key) {
    invariant(shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

    LOGV2_DEBUG(20676,
                1,
                "Index build: recording duplicate key conflict on unique index",
                "index"_attr = indexCatalogEntry->descriptor()->indexName());

    auto& rs = _keyConstraintsTable.getOrCreateTable(opCtx);

    // The key_string::Value will be serialized in the format [KeyString][TypeBits]. We need to
    // store the TypeBits for error reporting later on. The RecordId does not need to be stored, so
    // we exclude it from the serialization.
    StackBufBuilder builder;
    key.serializeWithoutRecordId(builder);

    if (index_builds::primary_driven::enabled(opCtx)) {
        LOGV2_DEBUG(10966700,
                    1,
                    "Index build: writing to duplicate key tracker container for primary-driven "
                    "index build.",
                    "index"_attr = indexCatalogEntry->descriptor()->indexName());
        invariant(rs.keyFormat() == KeyFormat::Long);
        IntegerKeyedContainer& container =
            std::get<std::reference_wrapper<IntegerKeyedContainer>>(rs.getContainer()).get();


        std::vector<RecordId> reservedRidBlock;
        rs.reserveRecordIds(
            opCtx, *shard_role_details::getRecoveryUnit(opCtx), &reservedRidBlock, 1);
        invariant(reservedRidBlock.size() == 1);
        auto status = container_write::insert(opCtx,
                                              *shard_role_details::getRecoveryUnit(opCtx),
                                              container,
                                              reservedRidBlock[0].getLong(),
                                              std::span<const char>(builder.buf(), builder.len()),
                                              container_write::NonexistentKeyGuarantee{});
        if (!status.isOK())
            return status;
    } else {
        auto status = rs.insertRecord(opCtx,
                                      *shard_role_details::getRecoveryUnit(opCtx),
                                      builder.buf(),
                                      builder.len(),
                                      Timestamp());
        if (!status.isOK())
            return status.getStatus();
    }

    auto numDuplicates = _duplicateCounter.addAndFetch(1);
    shard_role_details::getRecoveryUnit(opCtx)->onRollback(
        [this](OperationContext*) { _duplicateCounter.fetchAndAdd(-1); });

    if (numDuplicates % 1000 == 0) {
        LOGV2_INFO(4806700,
                   "Index build: high number of duplicate keys on unique index",
                   "index"_attr = indexCatalogEntry->descriptor()->indexName(),
                   "numDuplicateKeys"_attr = numDuplicates);
    }

    return Status::OK();
}

boost::optional<SortedDataInterface::DuplicateKey> DuplicateKeyTracker::checkConstraints(
    OperationContext* opCtx,
    const CollectionPtr& coll,
    const IndexCatalogEntry* indexCatalogEntry) const {
    invariant(!shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());
    if (!_keyConstraintsTable.tableExists()) {
        return boost::none;
    }
    auto& rs = _keyConstraintsTable.getTableOrThrow();

    auto constraintsCursor = rs.getCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx));
    auto record = constraintsCursor->next();

    auto index = indexCatalogEntry->accessMethod()->asSortedData()->getSortedDataInterface();

    static const char* curopMessage = "Index Build: checking for duplicate keys";
    ProgressMeterHolder progress;
    {
        std::unique_lock<Client> lk(*opCtx->getClient());
        progress.set(lk,
                     CurOp::get(opCtx)->setProgress(lk, curopMessage, _duplicateCounter.load(), 1),
                     opCtx);
    }

    const bool primaryDrivenIndexBuild = index_builds::primary_driven::enabled(opCtx);

    int resolved = 0;
    while (record) {
        resolved++;

        BufReader reader(record->data.data(), record->data.size());
        auto key = key_string::View::deserialize(reader, index->getKeyStringVersion(), boost::none);
        if (auto duplicateKey =
                index->dupKeyCheck(opCtx, *shard_role_details::getRecoveryUnit(opCtx), key)) {
            return duplicateKey;
        }

        constraintsCursor->save();
        writeConflictRetry(opCtx, "DuplicateKeyTracker::checkConstraints", coll->ns(), [&] {
            WriteUnitOfWork wuow{opCtx};
            if (primaryDrivenIndexBuild) {
                uassertStatusOK(container_write::remove(
                    opCtx,
                    *shard_role_details::getRecoveryUnit(opCtx),
                    std::get<std::reference_wrapper<IntegerKeyedContainer>>(rs.getContainer())
                        .get(),
                    record->id.getLong()));
            } else {
                rs.deleteRecord(opCtx, *shard_role_details::getRecoveryUnit(opCtx), record->id);
            }
            wuow.commit();
        });
        constraintsCursor->restore(*shard_role_details::getRecoveryUnit(opCtx));

        {
            std::unique_lock<Client> lk(*opCtx->getClient());
            progress.get(lk)->hit();
        }
        record = constraintsCursor->next();
    }

    {
        std::unique_lock<Client> lk(*opCtx->getClient());
        progress.get(lk)->finished();
    }

    invariant(resolved == _duplicateCounter.load());

    int logLevel = (resolved > 0) ? 0 : 1;
    LOGV2_DEBUG(20677,
                logLevel,
                "index build: resolved duplicate key conflicts for unique index",
                "numResolved"_attr = resolved,
                "indexName"_attr = indexCatalogEntry->descriptor()->indexName());
    return boost::none;
}

}  // namespace mongo
