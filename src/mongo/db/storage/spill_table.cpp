// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/spill_table.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/storage/disk_space_monitor.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/util/scopeguard.h"

#include <memory>
#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

SpillTable::Cursor::Cursor(RecoveryUnit& ru, std::unique_ptr<SeekableRecordCursor> cursor)
    : _ru(ru), _cursor(std::move(cursor)) {}

boost::optional<Record> SpillTable::Cursor::seekExact(const RecordId& id) {
    return _cursor->seekExact(id);
}

boost::optional<Record> SpillTable::Cursor::next() {
    return _cursor->next();
}

void SpillTable::Cursor::detachFromOperationContext() {
    _cursor->detachFromOperationContext();
    _ru.setOperationContext(nullptr);
}

void SpillTable::Cursor::reattachToOperationContext(OperationContext* opCtx) {
    _ru.setOperationContext(opCtx);
    _cursor->reattachToOperationContext(opCtx);
}

void SpillTable::Cursor::save() {
    _cursor->save();
}

bool SpillTable::Cursor::restore() {
    return _cursor->restore(_ru);
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

SpillTable::SpillTable(std::unique_ptr<RecoveryUnit> ru,
                       std::unique_ptr<RecordStore> rs,
                       StorageEngine& storageEngine,
                       DiskSpaceMonitor& diskMonitor,
                       int64_t thresholdBytes)
    : _ru(std::move(ru)),
      _rs(std::move(rs)),
      _storageEngine(storageEngine),
      _diskState(boost::in_place_init, diskMonitor, thresholdBytes) {
    // Abandon the snapshot right away in case the recovery unit was given to us with a snapshot
    // already open from creating the table.
    _ru->abandonSnapshot();
}

SpillTable::~SpillTable() {
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

    _storageEngine.dropSpillTable(*_ru, ident());
}

std::string_view SpillTable::ident() const {
    return _rs->getIdent();
}

long long SpillTable::dataSize() const {
    return _rs->dataSize();
}

long long SpillTable::numRecords() const {
    return _rs->numRecords();
}

int64_t SpillTable::storageSize() const {
    _ru->setIsolation(RecoveryUnit::Isolation::readCommitted);
    return _rs->storageSize(*_ru);
}

Status SpillTable::insertRecords(OperationContext* opCtx, std::vector<Record>* records) {
    if (auto status = _checkDiskSpace(); !status.isOK()) {
        return status;
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
    _ru->setOperationContext(opCtx);
    _ru->setIsolation(RecoveryUnit::Isolation::readCommitted);
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
    _ru->setOperationContext(opCtx);
    _ru->setIsolation(RecoveryUnit::Isolation::readCommitted);

    return std::make_unique<SpillTable::Cursor>(*_ru, _rs->getCursor(opCtx, *_ru, forward));
}

Status SpillTable::truncate(OperationContext* opCtx) {
    if (auto status = _checkDiskSpace(); !status.isOK()) {
        return status;
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
    return _ru->computeOperationStatisticsSinceLastCall();
}

RecordStore* SpillTable::getRecordStore_forTest() {
    return _rs.get();
}

Status SpillTable::_checkDiskSpace() const {
    return _diskState && _diskState->full()
        ? Status(ErrorCodes::OutOfDiskSpace,
                 "Failed to write to spill table as disk space is too low")
        : Status::OK();
}

}  // namespace mongo
