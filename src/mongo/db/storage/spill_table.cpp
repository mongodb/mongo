/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/storage/spill_table.h"

#include "mongo/db/storage/disk_space_monitor.h"
#include "mongo/db/storage/disk_space_util.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/logv2/log.h"

#include <memory>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

SpillTable::Cursor::Cursor(RecoveryUnit* ru, std::unique_ptr<SeekableRecordCursor> cursor)
    : _ru(ru), _cursor(std::move(cursor)) {}

boost::optional<Record> SpillTable::Cursor::seekExact(const RecordId& id) {
    return _cursor->seekExact(id);
}

boost::optional<Record> SpillTable::Cursor::next() {
    return _cursor->next();
}

void SpillTable::Cursor::detachFromOperationContext() {
    _cursor->detachFromOperationContext();
}

void SpillTable::Cursor::reattachToOperationContext(OperationContext* opCtx) {
    _cursor->reattachToOperationContext(opCtx);
}

void SpillTable::Cursor::save() {
    _cursor->save();
}

bool SpillTable::Cursor::restore(RecoveryUnit& ru) {
    return _cursor->restore(_ru ? *_ru : ru);
}

SpillTable::DiskState::DiskState(DiskSpaceMonitor& monitor, int64_t thresholdBytes)
    : _monitor(monitor) {
    _actionId = _monitor.registerAction(
        [thresholdBytes] { return thresholdBytes; },
        [this](OperationContext* opCtx, int64_t availableBytes, int64_t thresholdBytes) {
            LOGV2(10436000,
                  "Failing writes to spill table because remaining disk space is less than the "
                  "required minimum",
                  "availableBytes"_attr = availableBytes,
                  "thresholdBytes"_attr = thresholdBytes);
            _full.store(true);
        });
    if (getAvailableDiskSpaceBytesInDbPath(storageGlobalParams.dbpath) < thresholdBytes) {
        _full.store(true);
    }
}

SpillTable::DiskState::~DiskState() {
    _monitor.deregisterAction(_actionId);
}

bool SpillTable::DiskState::full() const {
    return _full.load();
}

SpillTable::SpillTable(std::unique_ptr<RecoveryUnit> ru, std::unique_ptr<RecordStore> rs)
    : _ru(std::move(ru)), _rs(std::move(rs)) {}

SpillTable::SpillTable(std::unique_ptr<RecoveryUnit> ru,
                       std::unique_ptr<RecordStore> rs,
                       DiskSpaceMonitor& diskMonitor,
                       int64_t thresholdBytes)
    : _ru(std::move(ru)),
      _rs(std::move(rs)),
      _diskState(boost::in_place_init, diskMonitor, thresholdBytes) {}

long long SpillTable::dataSize() const {
    return _rs->dataSize();
}

long long SpillTable::numRecords() const {
    return _rs->numRecords();
}

int64_t SpillTable::storageSize(RecoveryUnit& ru) const {
    return _rs->storageSize(_ru ? *_ru : ru);
}

Status SpillTable::insertRecords(OperationContext* opCtx, std::vector<Record>* records) {
    if (auto status = _checkDiskSpace(); !status.isOK()) {
        return status;
    }

    if (!_ru) {
        std::vector<Timestamp> timestamps(records->size());
        return _rs->insertRecords(
            opCtx, *storage_details::getRecoveryUnit(opCtx), records, timestamps);
    }

    for (auto&& record : *records) {
        auto status =
            _rs->insertRecord(opCtx, *_ru, record.id, record.data.data(), record.data.size(), {});
        if (!status.isOK()) {
            return status.getStatus();
        }
        record.id = status.getValue();
    }

    return Status::OK();
}

bool SpillTable::findRecord(OperationContext* opCtx, const RecordId& rid, RecordData* out) const {
    return _rs->findRecord(opCtx, _ru ? *_ru : *storage_details::getRecoveryUnit(opCtx), rid, out);
}

Status SpillTable::updateRecord(OperationContext* opCtx,
                                const RecordId& rid,
                                const char* data,
                                int len) {
    if (auto status = _checkDiskSpace(); !status.isOK()) {
        return status;
    }
    return _rs->updateRecord(
        opCtx, _ru ? *_ru : *storage_details::getRecoveryUnit(opCtx), rid, data, len);
}

void SpillTable::deleteRecord(OperationContext* opCtx, const RecordId& rid) {
    uassertStatusOK(_checkDiskSpace());
    _rs->deleteRecord(opCtx, _ru ? *_ru : *storage_details::getRecoveryUnit(opCtx), rid);
}

std::unique_ptr<SpillTable::Cursor> SpillTable::getCursor(OperationContext* opCtx,
                                                          bool forward) const {
    return std::make_unique<SpillTable::Cursor>(
        _ru.get(),
        _rs->getCursor(opCtx, _ru ? *_ru : *storage_details::getRecoveryUnit(opCtx), forward));
}

Status SpillTable::truncate(OperationContext* opCtx) {
    if (auto status = _checkDiskSpace(); !status.isOK()) {
        return status;
    }
    return _rs->truncate(opCtx, _ru ? *_ru : *storage_details::getRecoveryUnit(opCtx));
}

Status SpillTable::rangeTruncate(OperationContext* opCtx,
                                 const RecordId& minRecordId,
                                 const RecordId& maxRecordId,
                                 int64_t hintDataSizeIncrement,
                                 int64_t hintNumRecordsIncrement) {
    if (auto status = _checkDiskSpace(); !status.isOK()) {
        return status;
    }
    return _rs->rangeTruncate(opCtx,
                              _ru ? *_ru : *storage_details::getRecoveryUnit(opCtx),
                              minRecordId,
                              maxRecordId,
                              hintDataSizeIncrement,
                              hintNumRecordsIncrement);
}

Status SpillTable::_checkDiskSpace() const {
    return _diskState && _diskState->full()
        ? Status(ErrorCodes::OutOfDiskSpace,
                 "Failed to write to spill table as disk space is too low")
        : Status::OK();
}

}  // namespace mongo
