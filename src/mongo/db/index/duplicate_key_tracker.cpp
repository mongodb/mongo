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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include "mongo/db/index/duplicate_key_tracker.h"

#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/storage/execution_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

namespace mongo {

namespace {
static constexpr StringData kKeyField = "key"_sd;
}

DuplicateKeyTracker::DuplicateKeyTracker(OperationContext* opCtx, const IndexCatalogEntry* entry)
    : _indexCatalogEntry(entry),
      _keyConstraintsTable(
          opCtx->getServiceContext()->getStorageEngine()->makeTemporaryRecordStore(opCtx)) {

    invariant(_indexCatalogEntry->descriptor()->unique());
}

void DuplicateKeyTracker::finalizeTemporaryTable(OperationContext* opCtx,
                                                 TemporaryRecordStore::FinalizationAction action) {
    _keyConstraintsTable->finalizeTemporaryTable(opCtx, action);
}

Status DuplicateKeyTracker::recordKey(OperationContext* opCtx, const KeyString::Value& key) {
    invariant(opCtx->lockState()->inAWriteUnitOfWork());

    LOGV2_DEBUG(20676,
                1,
                "Index build: recording duplicate key conflict on unique index",
                "index"_attr = _indexCatalogEntry->descriptor()->indexName());

    // The KeyString::Value will be serialized in the format [KeyString][TypeBits]. We need to
    // store the TypeBits for error reporting later on. The RecordId does not need to be stored, so
    // we exclude it from the serialization.
    BufBuilder builder;
    key.serializeWithoutRecordId(builder);

    auto status =
        _keyConstraintsTable->rs()->insertRecord(opCtx, builder.buf(), builder.len(), Timestamp());
    if (!status.isOK())
        return status.getStatus();

    auto numDuplicates = _duplicateCounter.addAndFetch(1);
    opCtx->recoveryUnit()->onRollback([this]() { _duplicateCounter.fetchAndAdd(-1); });

    if (numDuplicates % 1000 == 0) {
        LOGV2_INFO(4806700,
                   "Index build: high number of duplicate keys on unique index",
                   "index"_attr = _indexCatalogEntry->descriptor()->indexName(),
                   "numDuplicateKeys"_attr = numDuplicates);
    }

    return Status::OK();
}

Status DuplicateKeyTracker::checkConstraints(OperationContext* opCtx) const {
    invariant(!opCtx->lockState()->inAWriteUnitOfWork());

    auto constraintsCursor = _keyConstraintsTable->rs()->getCursor(opCtx);
    auto record = constraintsCursor->next();

    auto index = _indexCatalogEntry->accessMethod()->getSortedDataInterface();

    static const char* curopMessage = "Index Build: checking for duplicate keys";
    ProgressMeterHolder progress;
    {
        stdx::unique_lock<Client> lk(*opCtx->getClient());
        progress.set(
            CurOp::get(opCtx)->setProgress_inlock(curopMessage, _duplicateCounter.load(), 1));
    }

    int resolved = 0;
    while (record) {
        resolved++;

        BufReader reader(record->data.data(), record->data.size());
        auto key = KeyString::Value::deserialize(reader, index->getKeyStringVersion());

        auto status = index->dupKeyCheck(opCtx, key);
        if (!status.isOK())
            return status;

        WriteUnitOfWork wuow(opCtx);
        _keyConstraintsTable->rs()->deleteRecord(opCtx, record->id);

        constraintsCursor->save();
        wuow.commit();
        constraintsCursor->restore();

        progress->hit();
        record = constraintsCursor->next();
    }
    progress->finished();

    invariant(resolved == _duplicateCounter.load());

    int logLevel = (resolved > 0) ? 0 : 1;
    LOGV2_DEBUG(20677,
                logLevel,
                "index build: resolved duplicate key conflicts for unique index",
                "numResolved"_attr = resolved,
                "indexName"_attr = _indexCatalogEntry->descriptor()->indexName());
    return Status::OK();
}

}  // namespace mongo
