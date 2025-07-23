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

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/compact_options.h"
#include "mongo/db/storage/damage_vector.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace mongo {

class RecordStoreBase : public RecordStore {
public:
    class Capped;
    class Oplog;

    RecordStoreBase(boost::optional<UUID>, StringData ident);

    boost::optional<UUID> uuid() const final;

    bool isTemp() const final;

    std::shared_ptr<Ident> getSharedIdent() const final;

    StringData getIdent() const final;

    void setIdent(std::shared_ptr<Ident>) final;

    RecordData dataFor(OperationContext* opCtx, RecoveryUnit&, const RecordId& loc) const final;

    bool findRecord(OperationContext* opCtx,
                    RecoveryUnit&,
                    const RecordId& loc,
                    RecordData* out) const final;

    void deleteRecord(OperationContext*, RecoveryUnit&, const RecordId&) final;

    Status insertRecords(OperationContext*,
                         RecoveryUnit&,
                         std::vector<Record>*,
                         const std::vector<Timestamp>&) final;

    StatusWith<RecordId> insertRecord(
        OperationContext*, RecoveryUnit&, const char* data, int len, Timestamp) final;

    StatusWith<RecordId> insertRecord(OperationContext*,
                                      RecoveryUnit&,
                                      const RecordId&,
                                      const char* data,
                                      int len,
                                      Timestamp) final;

    Status updateRecord(
        OperationContext*, RecoveryUnit&, const RecordId&, const char* data, int len) final;

    StatusWith<RecordData> updateWithDamages(OperationContext*,
                                             RecoveryUnit&,
                                             const RecordId&,
                                             const RecordData&,
                                             const char* damageSource,
                                             const DamageVector&) final;

    std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext*,
                                                    RecoveryUnit&,
                                                    bool forward = true) const override = 0;

    std::unique_ptr<RecordCursor> getRandomCursor(OperationContext*,
                                                  RecoveryUnit&) const override = 0;

    Status truncate(OperationContext*, RecoveryUnit&) final;

    Status rangeTruncate(OperationContext*,
                         RecoveryUnit&,
                         const RecordId& minRecordId = RecordId(),
                         const RecordId& maxRecordId = RecordId(),
                         int64_t hintDataSizeIncrement = 0,
                         int64_t hintNumRecordsIncrement = 0) final;

    StatusWith<int64_t> compact(OperationContext*, RecoveryUnit&, const CompactOptions&) final;

    RecordId getLargestKey(OperationContext*, RecoveryUnit&) const override = 0;

    void reserveRecordIds(OperationContext*,
                          RecoveryUnit&,
                          std::vector<RecordId>*,
                          size_t numRecords) override = 0;

private:
    virtual void _deleteRecord(OperationContext*, RecoveryUnit&, const RecordId&) = 0;

    virtual Status _insertRecords(OperationContext*,
                                  RecoveryUnit&,
                                  std::vector<Record>*,
                                  const std::vector<Timestamp>&) = 0;

    virtual Status _updateRecord(
        OperationContext*, RecoveryUnit&, const RecordId&, const char* data, int len) = 0;

    virtual StatusWith<RecordData> _updateWithDamages(OperationContext* opCtx,
                                                      RecoveryUnit&,
                                                      const RecordId& loc,
                                                      const RecordData& oldRec,
                                                      const char* damageSource,
                                                      const DamageVector& damages) = 0;

    virtual Status _truncate(OperationContext*, RecoveryUnit&) = 0;

    virtual Status _rangeTruncate(OperationContext*,
                                  RecoveryUnit&,
                                  const RecordId& minRecordId = RecordId(),
                                  const RecordId& maxRecordId = RecordId(),
                                  int64_t hintDataSizeIncrement = 0,
                                  int64_t hintNumRecordsIncrement = 0) = 0;

    virtual StatusWith<int64_t> _compact(OperationContext*,
                                         RecoveryUnit&,
                                         const CompactOptions&) = 0;

    std::shared_ptr<Ident> _ident;
    const boost::optional<UUID> _uuid;
};

class RecordStoreBase::Capped : public RecordStore::Capped {
public:
    Capped();

    std::shared_ptr<CappedInsertNotifier> getInsertNotifier() const final;

    bool hasWaiters() const final;

    void notifyWaitersIfNeeded() final;

    TruncateAfterResult truncateAfter(OperationContext*,
                                      RecoveryUnit&,
                                      const RecordId&,
                                      bool inclusive) final;

private:
    virtual TruncateAfterResult _truncateAfter(OperationContext*,
                                               RecoveryUnit&,
                                               const RecordId&,
                                               bool inclusive) = 0;

    std::shared_ptr<CappedInsertNotifier> _cappedInsertNotifier;
};

class RecordStoreBase::Oplog : public RecordStore::Oplog {
public:
    std::unique_ptr<SeekableRecordCursor> getRawCursor(OperationContext* opCtx,
                                                       RecoveryUnit& ru,
                                                       bool forward = true) const override = 0;
};

};  // namespace mongo
