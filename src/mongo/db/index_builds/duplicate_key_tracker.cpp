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


#include "mongo/db/index_builds/duplicate_key_tracker.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/progress_meter.h"
#include "mongo/util/str.h"

#include <mutex>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex


namespace mongo {

namespace {
static constexpr StringData kKeyField = "key"_sd;
}

DuplicateKeyTracker::DuplicateKeyTracker(OperationContext* opCtx,
                                         const IndexCatalogEntry* entry,
                                         StringData ident,
                                         bool tableExists)
    : _keyConstraintsTable([&]() {
          auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
          if (tableExists) {
              return storageEngine->makeTemporaryRecordStoreFromExistingIdent(
                  opCtx, ident, KeyFormat::Long);
          } else {
              return storageEngine->makeTemporaryRecordStore(opCtx, ident, KeyFormat::Long);
          }
      }()) {
    invariant(entry->descriptor()->unique());
}

void DuplicateKeyTracker::keepTemporaryTable() {
    _keyConstraintsTable->keep();
}

Status DuplicateKeyTracker::recordKey(OperationContext* opCtx,
                                      const IndexCatalogEntry* indexCatalogEntry,
                                      const key_string::View& key) {
    invariant(shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

    LOGV2_DEBUG(20676,
                1,
                "Index build: recording duplicate key conflict on unique index",
                "index"_attr = indexCatalogEntry->descriptor()->indexName());

    // The key_string::Value will be serialized in the format [KeyString][TypeBits]. We need to
    // store the TypeBits for error reporting later on. The RecordId does not need to be stored, so
    // we exclude it from the serialization.
    StackBufBuilder builder;
    key.serializeWithoutRecordId(builder);

    auto status =
        _keyConstraintsTable->rs()->insertRecord(opCtx,
                                                 *shard_role_details::getRecoveryUnit(opCtx),
                                                 builder.buf(),
                                                 builder.len(),
                                                 Timestamp());
    if (!status.isOK())
        return status.getStatus();

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
    OperationContext* opCtx, const IndexCatalogEntry* indexCatalogEntry) const {
    invariant(!shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

    auto constraintsCursor =
        _keyConstraintsTable->rs()->getCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx));
    auto record = constraintsCursor->next();

    auto index = indexCatalogEntry->accessMethod()->asSortedData()->getSortedDataInterface();

    static const char* curopMessage = "Index Build: checking for duplicate keys";
    ProgressMeterHolder progress;
    {
        stdx::unique_lock<Client> lk(*opCtx->getClient());
        progress.set(lk,
                     CurOp::get(opCtx)->setProgress(lk, curopMessage, _duplicateCounter.load(), 1),
                     opCtx);
    }

    int resolved = 0;
    while (record) {
        resolved++;

        BufReader reader(record->data.data(), record->data.size());
        auto key = key_string::View::deserialize(reader, index->getKeyStringVersion(), boost::none);
        if (auto duplicateKey =
                index->dupKeyCheck(opCtx, *shard_role_details::getRecoveryUnit(opCtx), key)) {
            return duplicateKey;
        }

        WriteUnitOfWork wuow(opCtx);
        _keyConstraintsTable->rs()->deleteRecord(
            opCtx, *shard_role_details::getRecoveryUnit(opCtx), record->id);

        constraintsCursor->save();
        wuow.commit();
        constraintsCursor->restore(*shard_role_details::getRecoveryUnit(opCtx));

        {
            stdx::unique_lock<Client> lk(*opCtx->getClient());
            progress.get(lk)->hit();
        }
        record = constraintsCursor->next();
    }

    {
        stdx::unique_lock<Client> lk(*opCtx->getClient());
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
