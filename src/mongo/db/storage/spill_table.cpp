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

#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/storage/disk_space_monitor.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/util/scopeguard.h"

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
    if (_ru) {
        _ru->setOperationContext(nullptr);
    }
}

void SpillTable::Cursor::reattachToOperationContext(OperationContext* opCtx) {
    if (_ru) {
        _ru->setOperationContext(opCtx);
    }
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
    _monitor.runAction(nullptr, _actionId);
}

SpillTable::DiskState::~DiskState() {
    _monitor.deregisterAction(_actionId);
}

bool SpillTable::DiskState::full() const {
    return _full.load();
}

SpillTable::SpillTable(std::unique_ptr<RecoveryUnit> ru, std::unique_ptr<RecordStore> rs)
    : _ru(std::move(ru)), _rs(std::move(rs)), _storageEngine(nullptr) {}

SpillTable::SpillTable(std::unique_ptr<RecoveryUnit> ru,
                       std::unique_ptr<RecordStore> rs,
                       StorageEngine& storageEngine,
                       DiskSpaceMonitor& diskMonitor,
                       int64_t thresholdBytes)
    : _ru(std::move(ru)),
      _rs(std::move(rs)),
      _storageEngine(&storageEngine),
      _diskState(boost::in_place_init, diskMonitor, thresholdBytes) {
    // Abandon the snapshot right away in case the recovery unit was given to us with a snapshot
    // already open from creating the table.
    _ru->abandonSnapshot();
}

SpillTable::~SpillTable() {
    if (!_storageEngine) {
        return;
    }

    // As an optimization, truncate the table before dropping it so that the checkpoint taken at
    // shutdown never has to do the work to write the data to disk.
    try {
        _ru->setIsolation(RecoveryUnit::Isolation::snapshot);
        StorageWriteTransaction txn{*_ru};
        uassertStatusOK(_rs->truncate(nullptr, *_ru));
        txn.commit();
    } catch (...) {
        LOGV2(10659600,
              "Failed to truncate spill table, ignoring and continuing to drop",
              "ident"_attr = ident(),
              "error"_attr = exceptionToStatus());
    }

    _storageEngine->dropSpillTable(*_ru, ident());
}

StringData SpillTable::ident() const {
    return _rs->getIdent();
}

long long SpillTable::dataSize() const {
    return _rs->dataSize();
}

long long SpillTable::numRecords() const {
    return _rs->numRecords();
}

int64_t SpillTable::storageSize(RecoveryUnit& ru) const {
    // TODO (SERVER-106716): Remove this case.
    if (!_ru) {
        return _rs->storageSize(ru);
    }

    _ru->setIsolation(RecoveryUnit::Isolation::readUncommitted);
    return _rs->storageSize(*_ru);
}

Status SpillTable::insertRecords(OperationContext* opCtx, std::vector<Record>* records) {
    if (auto status = _checkDiskSpace(); !status.isOK()) {
        return status;
    }

    // TODO (SERVER-106716): Remove this case.
    if (!_ru) {
        std::vector<Timestamp> timestamps(records->size());
        return _rs->insertRecords(
            opCtx, *shard_role_details::getRecoveryUnit(opCtx), records, timestamps);
    }

    _ru->setOperationContext(opCtx);
    _ru->setIsolation(RecoveryUnit::Isolation::snapshot);
    ON_BLOCK_EXIT([this] {
        _ru->abandonSnapshot();
        _ru->setOperationContext(nullptr);
    });

    // This function inserts records starting at the provided index until the threshold of data is
    // reached, returning the last index that was inserted.
    auto insert = [this, opCtx, &records](size_t index,
                                          int64_t batchSizeThreshold) -> StatusWith<size_t> {
        StorageWriteTransaction txn{*_ru};
        int64_t bytesInserted = 0;

        for (; index < records->size(); ++index) {
            auto& record = (*records)[index];

            auto rid = _rs->insertRecord(
                opCtx, *_ru, record.id, record.data.data(), record.data.size(), {});
            if (!rid.isOK()) {
                return rid.getStatus();
            }
            record.id = rid.getValue();

            bytesInserted += record.data.size();
            if (bytesInserted >= batchSizeThreshold) {
                break;
            }
        }

        txn.commit();
        return index;
    };

    int64_t batchSizeThreshold = gSpillTableInsertBatchSizeBytes.load();
    for (size_t index = 0; index < records->size(); ++index) {
        auto status = writeConflictRetry(
            opCtx, *_ru, "SpillTable::insertRecords", NamespaceString::kEmpty, [&] {
                try {
                    auto result = insert(index, batchSizeThreshold);
                    if (!result.isOK()) {
                        return result.getStatus();
                    }
                    index = result.getValue();
                } catch (const StorageUnavailableException&) {
                    // Insert the rest of the records one at a time without any batching.
                    batchSizeThreshold = 0;
                    throw;
                }
                return Status::OK();
            });
        if (!status.isOK()) {
            return status;
        }
    }

    return Status::OK();
}

bool SpillTable::findRecord(OperationContext* opCtx, const RecordId& rid, RecordData* out) const {
    // TODO (SERVER-106716): Remove this case.
    if (!_ru) {
        return _rs->findRecord(opCtx, *shard_role_details::getRecoveryUnit(opCtx), rid, out);
    }

    _ru->setOperationContext(opCtx);
    _ru->setIsolation(RecoveryUnit::Isolation::readUncommitted);
    ON_BLOCK_EXIT([this] { _ru->setOperationContext(nullptr); });

    return _rs->findRecord(opCtx, *_ru, rid, out);
}

Status SpillTable::updateRecord(OperationContext* opCtx,
                                const RecordId& rid,
                                const char* data,
                                int len) {
    if (auto status = _checkDiskSpace(); !status.isOK()) {
        return status;
    }

    // TODO (SERVER-106716): Remove this case.
    if (!_ru) {
        return _rs->updateRecord(
            opCtx, *shard_role_details::getRecoveryUnit(opCtx), rid, data, len);
    }

    _ru->setOperationContext(opCtx);
    _ru->setIsolation(RecoveryUnit::Isolation::snapshot);
    ON_BLOCK_EXIT([this] {
        _ru->abandonSnapshot();
        _ru->setOperationContext(nullptr);
    });

    return writeConflictRetry(
        opCtx, *_ru, "SpillTable::updateRecord", NamespaceString::kEmpty, [&] {
            StorageWriteTransaction txn{*_ru};
            auto status = _rs->updateRecord(opCtx, *_ru, rid, data, len);
            if (!status.isOK()) {
                return status;
            }
            txn.commit();
            return Status::OK();
        });
}

void SpillTable::deleteRecord(OperationContext* opCtx, const RecordId& rid) {
    uassertStatusOK(_checkDiskSpace());

    // TODO (SERVER-106716): Remove this case.
    if (!_ru) {
        _rs->deleteRecord(opCtx, *shard_role_details::getRecoveryUnit(opCtx), rid);
        return;
    }

    _ru->setOperationContext(opCtx);
    _ru->setIsolation(RecoveryUnit::Isolation::snapshot);
    ON_BLOCK_EXIT([this] {
        _ru->abandonSnapshot();
        _ru->setOperationContext(nullptr);
    });

    writeConflictRetry(opCtx, *_ru, "SpillTable::deleteRecord", NamespaceString::kEmpty, [&] {
        StorageWriteTransaction txn{*_ru};
        _rs->deleteRecord(opCtx, *_ru, rid);
        txn.commit();
    });
}

std::unique_ptr<SpillTable::Cursor> SpillTable::getCursor(OperationContext* opCtx,
                                                          bool forward) const {
    // TODO (SERVER-106716): Remove this case.
    if (!_ru) {
        return std::make_unique<SpillTable::Cursor>(
            _ru.get(), _rs->getCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx), forward));
    }

    _ru->setOperationContext(opCtx);
    _ru->setIsolation(RecoveryUnit::Isolation::readUncommitted);

    return std::make_unique<SpillTable::Cursor>(_ru.get(), _rs->getCursor(opCtx, *_ru, forward));
}

Status SpillTable::truncate(OperationContext* opCtx) {
    if (auto status = _checkDiskSpace(); !status.isOK()) {
        return status;
    }

    // TODO (SERVER-106716): Remove this case.
    if (!_ru) {
        return _rs->truncate(opCtx, *shard_role_details::getRecoveryUnit(opCtx));
    }

    _ru->setOperationContext(opCtx);
    _ru->setIsolation(RecoveryUnit::Isolation::snapshot);
    ON_BLOCK_EXIT([this] {
        _ru->abandonSnapshot();
        _ru->setOperationContext(nullptr);
    });

    return writeConflictRetry(opCtx, *_ru, "SpillTable::truncate", NamespaceString::kEmpty, [&] {
        StorageWriteTransaction txn{*_ru};
        auto status = _rs->truncate(opCtx, *_ru);
        if (!status.isOK()) {
            return status;
        }
        txn.commit();
        return Status::OK();
    });
}

Status SpillTable::rangeTruncate(OperationContext* opCtx,
                                 const RecordId& minRecordId,
                                 const RecordId& maxRecordId,
                                 int64_t hintDataSizeIncrement,
                                 int64_t hintNumRecordsIncrement) {
    if (auto status = _checkDiskSpace(); !status.isOK()) {
        return status;
    }

    // TODO (SERVER-106716): Remove this case.
    if (!_ru) {
        return _rs->rangeTruncate(opCtx,
                                  *shard_role_details::getRecoveryUnit(opCtx),
                                  minRecordId,
                                  maxRecordId,
                                  hintDataSizeIncrement,
                                  hintNumRecordsIncrement);
    }

    _ru->setOperationContext(opCtx);
    _ru->setIsolation(RecoveryUnit::Isolation::snapshot);
    ON_BLOCK_EXIT([this] {
        _ru->abandonSnapshot();
        _ru->setOperationContext(nullptr);
    });

    return writeConflictRetry(
        opCtx, *_ru, "SpillTable::rangeTruncate", NamespaceString::kEmpty, [&] {
            StorageWriteTransaction txn{*_ru};
            auto status = _rs->rangeTruncate(opCtx,
                                             *_ru,
                                             minRecordId,
                                             maxRecordId,
                                             hintDataSizeIncrement,
                                             hintNumRecordsIncrement);
            if (!status.isOK()) {
                return status;
            }
            txn.commit();
            return Status::OK();
        });
}

std::unique_ptr<StorageStats> SpillTable::computeOperationStatisticsSinceLastCall() {
    // TODO (SERVER-106716): Remove this case.
    if (!_ru) {
        return nullptr;
    }
    return _ru->computeOperationStatisticsSinceLastCall();
}

Status SpillTable::_checkDiskSpace() const {
    return _diskState && _diskState->full()
        ? Status(ErrorCodes::OutOfDiskSpace,
                 "Failed to write to spill table as disk space is too low")
        : Status::OK();
}

}  // namespace mongo
