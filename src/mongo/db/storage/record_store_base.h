// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
#include "mongo/util/modules.h"

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace mongo {

class RecordStoreBase : public RecordStore {
public:
    class Capped;
    class Oplog;

    RecordStoreBase(boost::optional<UUID>, std::string_view ident);

    boost::optional<UUID> uuid() const final;

    bool isTemp() const final;

    std::shared_ptr<Ident> getSharedIdent() const final;

    std::string_view getIdent() const final;

    bool isColdCollection() const override {
        return false;
    }

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
                                             const DamageVector&,
                                             const SeekableRecordCursor* cursor) final;

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
                                                      const DamageVector& damages,
                                                      const SeekableRecordCursor* cursor) = 0;

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
