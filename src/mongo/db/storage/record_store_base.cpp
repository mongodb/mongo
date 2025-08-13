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

#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/operation_context.h"

namespace mongo {
namespace {

void validateWriteAllowed(OperationContext* opCtx) {
    uassert(ErrorCodes::IllegalOperation,
            "Cannot execute a write operation in read-only mode",
            !opCtx || !opCtx->readOnly());
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

StringData RecordStoreBase::getIdent() const {
    return _ident->getIdent();
}

void RecordStoreBase::setIdent(std::shared_ptr<Ident> ident) {
    _ident = std::move(ident);
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

void RecordStoreBase::deleteRecord(OperationContext* opCtx, RecoveryUnit& ru, const RecordId& id) {
    validateWriteAllowed(opCtx);
    _deleteRecord(opCtx, ru, id);
}

Status RecordStoreBase::insertRecords(OperationContext* opCtx,
                                      RecoveryUnit& ru,
                                      std::vector<Record>* records,
                                      const std::vector<Timestamp>& timestamps) {
    validateWriteAllowed(opCtx);
    return _insertRecords(opCtx, ru, records, timestamps);
}

StatusWith<RecordId> RecordStoreBase::insertRecord(
    OperationContext* opCtx, RecoveryUnit& ru, const char* data, int len, Timestamp timestamp) {
    // Record stores with the Long key format accept a null RecordId, as the storage engine will
    // generate one.
    invariant(keyFormat() == KeyFormat::Long);
    return insertRecord(opCtx, ru, RecordId(), data, len, timestamp);
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

Status RecordStoreBase::updateRecord(
    OperationContext* opCtx, RecoveryUnit& ru, const RecordId& id, const char* data, int len) {
    validateWriteAllowed(opCtx);
    return _updateRecord(opCtx, ru, id, data, len);
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

Status RecordStoreBase::truncate(OperationContext* opCtx, RecoveryUnit& ru) {
    validateWriteAllowed(opCtx);
    return _truncate(opCtx, ru);
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
                                             RecoveryUnit& ru,
                                             const CompactOptions& options) {
    validateWriteAllowed(opCtx);
    return _compact(opCtx, ru, options);
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
    OperationContext* opCtx, RecoveryUnit& ru, const RecordId& id, bool inclusive) {
    validateWriteAllowed(opCtx);
    return _truncateAfter(opCtx, ru, id, inclusive);
}
}  // namespace mongo
