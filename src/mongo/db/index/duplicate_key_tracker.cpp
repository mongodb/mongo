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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include "mongo/db/index/duplicate_key_tracker.h"

#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/keypattern.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

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

Status DuplicateKeyTracker::recordKeys(OperationContext* opCtx, const std::vector<BSONObj>& keys) {
    if (keys.size() == 0)
        return Status::OK();

    std::vector<BSONObj> toInsert;
    toInsert.reserve(keys.size());
    for (auto&& key : keys) {
        BSONObjBuilder builder;
        builder.append(kKeyField, key);

        BSONObj obj = builder.obj();

        toInsert.emplace_back(std::move(obj));
    }

    std::vector<Record> records;
    records.reserve(keys.size());
    for (auto&& obj : toInsert) {
        records.emplace_back(Record{RecordId(), RecordData(obj.objdata(), obj.objsize())});
    }

    LOG(1) << "recording " << records.size() << " duplicate key conflicts on unique index: "
           << _indexCatalogEntry->descriptor()->indexName();

    WriteUnitOfWork wuow(opCtx);
    std::vector<Timestamp> timestamps(records.size());
    Status s = _keyConstraintsTable->rs()->insertRecords(opCtx, &records, timestamps);
    if (!s.isOK())
        return s;

    wuow.commit();

    _duplicateCounter.fetchAndAdd(records.size());

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
        BSONObj conflict = record->data.toBson();
        BSONObj keyObj = conflict[kKeyField].Obj();

        auto status = index->dupKeyCheck(opCtx, keyObj);
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
    LOG(logLevel) << "index build: resolved " << resolved
                  << " duplicate key conflicts for unique index: "
                  << _indexCatalogEntry->descriptor()->indexName();
    return Status::OK();
}

bool DuplicateKeyTracker::areAllConstraintsChecked(OperationContext* opCtx) const {
    auto cursor = _keyConstraintsTable->rs()->getCursor(opCtx);
    auto record = cursor->next();

    // The table is empty only when there are no more constraints to check.
    if (!record)
        return true;

    return false;
}

}  // namespace mongo
