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

    LOG(1) << "index build recording " << records.size()
           << " duplicate key conflicts for unique index: "
           << _indexCatalogEntry->descriptor()->indexName();

    WriteUnitOfWork wuow(opCtx);
    std::vector<Timestamp> timestamps(records.size());
    Status s = _keyConstraintsTable->rs()->insertRecords(opCtx, &records, timestamps);
    if (!s.isOK())
        return s;

    wuow.commit();

    return Status::OK();
}

Status DuplicateKeyTracker::checkConstraints(OperationContext* opCtx) const {
    auto constraintsCursor = _keyConstraintsTable->rs()->getCursor(opCtx);
    auto record = constraintsCursor->next();

    auto index = _indexCatalogEntry->accessMethod()->getSortedDataInterface();

    int count = 0;
    while (record) {
        count++;
        BSONObj conflict = record->data.toBson();
        BSONObj keyObj = conflict[kKeyField].Obj();

        auto status = index->dupKeyCheck(opCtx, keyObj);
        if (!status.isOK())
            return status;

        record = constraintsCursor->next();
    }

    log() << "index build resolved " << count << " duplicate key conflicts for unique index: "
          << _indexCatalogEntry->descriptor()->indexName();
    return Status::OK();
}

}  // namespace mongo
