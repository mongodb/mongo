/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/storage/record_store_base.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/transaction_resources.h"

namespace mongo {
namespace {

void validateWriteAllowed(OperationContext* opCtx) {
    uassert(ErrorCodes::IllegalOperation,
            "Cannot execute a write operation in read-only mode",
            !opCtx->readOnly());
}

}  // namespace

RecordStoreBase::RecordStoreBase(boost::optional<UUID> uuid, StringData ident)
    : _ident(std::make_shared<Ident>(std::string{ident})), _uuid(uuid) {}

boost::optional<UUID> RecordStoreBase::uuid() const {
    return _uuid;
}

bool RecordStoreBase::isTemp() const {
    return !_uuid.has_value();
}

std::shared_ptr<Ident> RecordStoreBase::getSharedIdent() const {
    return _ident;
}

const std::string& RecordStoreBase::getIdent() const {
    return _ident->getIdent();
}

void RecordStoreBase::setIdent(std::shared_ptr<Ident> ident) {
    _ident = std::move(ident);
}

RecordData RecordStoreBase::dataFor(OperationContext* opCtx, const RecordId& loc) const {
    return dataFor(opCtx, *shard_role_details::getRecoveryUnit(opCtx), loc);
}
RecordData RecordStoreBase::dataFor(OperationContext* opCtx,
                                    RecoveryUnit& ru,
                                    const RecordId& loc) const {
    RecordData data;
    invariant(findRecord(opCtx, ru, loc, &data),
              str::stream() << "Didn't find RecordId " << loc << " in record store "
                            << (_uuid ? _uuid->toString() : std::string{}));
    return data;
}

bool RecordStoreBase::findRecord(OperationContext* opCtx,
                                 const RecordId& loc,
                                 RecordData* out) const {
    return findRecord(opCtx, *shard_role_details::getRecoveryUnit(opCtx), loc, out);
}
bool RecordStoreBase::findRecord(OperationContext* opCtx,
                                 RecoveryUnit& ru,
                                 const RecordId& loc,
                                 RecordData* out) const {
    auto cursor = getCursor(opCtx, ru);
    auto record = cursor->seekExact(loc);
    if (!record)
        return false;

    record->data.makeOwned();  // Unowned data expires when cursor goes out of scope.
    *out = std::move(record->data);
    return true;
}

void RecordStoreBase::deleteRecord(OperationContext* opCtx, const RecordId& id) {
    deleteRecord(opCtx, *shard_role_details::getRecoveryUnit(opCtx), id);
}
void RecordStoreBase::deleteRecord(OperationContext* opCtx, RecoveryUnit& ru, const RecordId& id) {
    validateWriteAllowed(opCtx);
    _deleteRecord(opCtx, ru, id);
}

Status RecordStoreBase::insertRecords(OperationContext* opCtx,
                                      std::vector<Record>* records,
                                      const std::vector<Timestamp>& timestamps) {
    return insertRecords(opCtx, *shard_role_details::getRecoveryUnit(opCtx), records, timestamps);
}
Status RecordStoreBase::insertRecords(OperationContext* opCtx,
                                      RecoveryUnit& ru,
                                      std::vector<Record>* records,
                                      const std::vector<Timestamp>& timestamps) {
    validateWriteAllowed(opCtx);
    return _insertRecords(opCtx, ru, records, timestamps);
}

StatusWith<RecordId> RecordStoreBase::insertRecord(OperationContext* opCtx,
                                                   const char* data,
                                                   int len,
                                                   Timestamp timestamp) {
    return insertRecord(opCtx, *shard_role_details::getRecoveryUnit(opCtx), data, len, timestamp);
}
StatusWith<RecordId> RecordStoreBase::insertRecord(
    OperationContext* opCtx, RecoveryUnit& ru, const char* data, int len, Timestamp timestamp) {
    // Record stores with the Long key format accept a null RecordId, as the storage engine will
    // generate one.
    invariant(keyFormat() == KeyFormat::Long);
    return insertRecord(opCtx, RecordId(), data, len, timestamp);
}

StatusWith<RecordId> RecordStoreBase::insertRecord(
    OperationContext* opCtx, const RecordId& id, const char* data, int len, Timestamp timestamp) {
    return insertRecord(
        opCtx, *shard_role_details::getRecoveryUnit(opCtx), id, data, len, timestamp);
}
StatusWith<RecordId> RecordStoreBase::insertRecord(OperationContext* opCtx,
                                                   RecoveryUnit& ru,
                                                   const RecordId& id,
                                                   const char* data,
                                                   int len,
                                                   Timestamp timestamp) {
    std::vector<Record> inOutRecords{Record{id, RecordData(data, len)}};
    Status status = insertRecords(opCtx, ru, &inOutRecords, std::vector<Timestamp>{timestamp});
    if (!status.isOK())
        return status;
    return std::move(inOutRecords.front().id);
}

Status RecordStoreBase::updateRecord(OperationContext* opCtx,
                                     const RecordId& id,
                                     const char* data,
                                     int len) {
    return updateRecord(opCtx, *shard_role_details::getRecoveryUnit(opCtx), id, data, len);
}
Status RecordStoreBase::updateRecord(
    OperationContext* opCtx, RecoveryUnit& ru, const RecordId& id, const char* data, int len) {
    validateWriteAllowed(opCtx);
    return _updateRecord(opCtx, ru, id, data, len);
}

StatusWith<RecordData> RecordStoreBase::updateWithDamages(OperationContext* opCtx,
                                                          const RecordId& id,
                                                          const RecordData& data,
                                                          const char* damageSource,
                                                          const DamageVector& damages) {
    return updateWithDamages(
        opCtx, *shard_role_details::getRecoveryUnit(opCtx), id, data, damageSource, damages);
}
StatusWith<RecordData> RecordStoreBase::updateWithDamages(OperationContext* opCtx,
                                                          RecoveryUnit& ru,
                                                          const RecordId& id,
                                                          const RecordData& data,
                                                          const char* damageSource,
                                                          const DamageVector& damages) {
    validateWriteAllowed(opCtx);
    return _updateWithDamages(opCtx, ru, id, data, damageSource, damages);
}

std::unique_ptr<SeekableRecordCursor> RecordStoreBase::getCursor(OperationContext* opCtx,
                                                                 bool forward) const {
    return getCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx), forward);
}

std::unique_ptr<RecordCursor> RecordStoreBase::getRandomCursor(OperationContext* opCtx) const {
    return getRandomCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx));
}

Status RecordStoreBase::truncate(OperationContext* opCtx) {
    return truncate(opCtx, *shard_role_details::getRecoveryUnit(opCtx));
}
Status RecordStoreBase::truncate(OperationContext* opCtx, RecoveryUnit& ru) {
    validateWriteAllowed(opCtx);
    return _truncate(opCtx, ru);
}

Status RecordStoreBase::rangeTruncate(OperationContext* opCtx,
                                      const RecordId& minRecordId,
                                      const RecordId& maxRecordId,
                                      int64_t hintDataSizeIncrement,
                                      int64_t hintNumRecordsIncrement) {
    return rangeTruncate(opCtx,
                         *shard_role_details::getRecoveryUnit(opCtx),
                         minRecordId,
                         maxRecordId,
                         hintDataSizeIncrement,
                         hintNumRecordsIncrement);
}
Status RecordStoreBase::rangeTruncate(OperationContext* opCtx,
                                      RecoveryUnit& ru,
                                      const RecordId& minRecordId,
                                      const RecordId& maxRecordId,
                                      int64_t hintDataSizeIncrement,
                                      int64_t hintNumRecordsIncrement) {
    validateWriteAllowed(opCtx);
    invariant(minRecordId != RecordId() || maxRecordId != RecordId(),
              "Ranged truncate must have one bound defined");
    invariant(minRecordId <= maxRecordId, "Start position cannot be after end position");
    return _rangeTruncate(
        opCtx, ru, minRecordId, maxRecordId, hintDataSizeIncrement, hintNumRecordsIncrement);
}

StatusWith<int64_t> RecordStoreBase::compact(OperationContext* opCtx,
                                             const CompactOptions& options) {
    return compact(opCtx, *shard_role_details::getRecoveryUnit(opCtx), options);
}
StatusWith<int64_t> RecordStoreBase::compact(OperationContext* opCtx,
                                             RecoveryUnit& ru,
                                             const CompactOptions& options) {
    validateWriteAllowed(opCtx);
    return _compact(opCtx, ru, options);
}

RecordId RecordStoreBase::getLargestKey(OperationContext* opCtx) const {
    return getLargestKey(opCtx, *shard_role_details::getRecoveryUnit(opCtx));
}

void RecordStoreBase::reserveRecordIds(OperationContext* opCtx,
                                       std::vector<RecordId>* rids,
                                       size_t numRecords) {
    reserveRecordIds(opCtx, *shard_role_details::getRecoveryUnit(opCtx), rids, numRecords);
}

RecordStoreBase::Capped::Capped()
    : _cappedInsertNotifier(std::make_shared<CappedInsertNotifier>()) {}

std::shared_ptr<CappedInsertNotifier> RecordStoreBase::Capped::getInsertNotifier() const {
    return _cappedInsertNotifier;
}

bool RecordStoreBase::Capped::hasWaiters() const {
    return _cappedInsertNotifier && _cappedInsertNotifier.use_count() > 1;
}

void RecordStoreBase::Capped::notifyWaitersIfNeeded() {
    if (hasWaiters()) {
        _cappedInsertNotifier->notifyAll();
    }
}

RecordStoreBase::Capped::TruncateAfterResult RecordStoreBase::Capped::truncateAfter(
    OperationContext* opCtx, const RecordId& id, bool inclusive) {
    return truncateAfter(opCtx, *shard_role_details::getRecoveryUnit(opCtx), id, inclusive);
}
RecordStoreBase::Capped::TruncateAfterResult RecordStoreBase::Capped::truncateAfter(
    OperationContext* opCtx, RecoveryUnit& ru, const RecordId& id, bool inclusive) {
    validateWriteAllowed(opCtx);
    return _truncateAfter(opCtx, ru, id, inclusive);
}

std::unique_ptr<SeekableRecordCursor> RecordStoreBase::Oplog::getRawCursor(OperationContext* opCtx,
                                                                           bool forward) const {
    return getRawCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx), forward);
}

}  // namespace mongo
