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

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/collection_truncate_markers.h"
#include "mongo/db/storage/container.h"
#include "mongo/db/storage/damage_vector.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/record_store_base.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_container.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_cursor.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_size_storer.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/db/validate/validate_results.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include <wiredtiger.h>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

/**
 * Either executes the specified operation and returns it's value or randomly throws a write
 * conflict exception if the WTWriteConflictException failpoint is enabled. This is only checked
 * on cursor methods that make modifications.
 */
#define WT_OP_CHECK(x) \
    (((MONGO_unlikely(WTWriteConflictException.shouldFail()))) ? (WT_ROLLBACK) : (x))

/**
 * Identical to WT_OP_CHECK except this is checked on cursor seeks/advancement.
 */
#define WT_READ_CHECK(x) \
    (((MONGO_unlikely(WTWriteConflictExceptionForReads.shouldFail()))) ? (WT_ROLLBACK) : (x))

namespace mongo {

class RecoveryUnit;
class SpillWiredTigerKVEngine;
class WiredTigerConnection;
class WiredTigerKVEngine;
class WiredTigerSizeStorer;

class WiredTigerRecordStore : public RecordStoreBase {
public:
    class Capped;
    class Oplog;

    // Encapsulates configuration parameters to configure a WiredTiger table.
    struct WiredTigerTableConfig {
        // This specifies the value for the key_format configuration parameter.
        KeyFormat keyFormat{KeyFormat::Long};
        // This specifies the value for the log.enabled configuration parameter.
        bool logEnabled{true};
        // This specifies the value for the block_compressor configuration parameter.
        std::string blockCompressor{"snappy"};
        // Any additional configuration parameters for WT_SESSION::create() in the configuration
        // string format.
        std::string extraCreateOptions;
    };

    struct Params {
        boost::optional<UUID> uuid;
        std::string ident;
        // Canonical engine name to use for retrieving options
        std::string engineName;
        KeyFormat keyFormat;
        // Whether or not to allow writes to overwrite existing records with the same RecordId.
        bool overwrite;
        // True if WiredTiger is logging updates to this table.
        bool isLogged;
        // TODO (SERVER-57482): Remove special handling of skipping "wiredtiger_calc_modify()".
        // True if force to update with the full document, and false otherwise.
        bool forceUpdateWithFullDocument;
        // True if the storage engine is an in-memory storage engine
        bool inMemory;
        WiredTigerSizeStorer* sizeStorer;
        bool tracksSizeAdjustments;
    };

    typedef std::variant<int64_t, WiredTigerItem> CursorKey;

    /**
     * Parses collections options for wired tiger configuration string for table creation.
     * The document 'options' is typically obtained from the 'wiredTiger' field of
     * CollectionOptions::storageEngine.
     */
    static StatusWith<std::string> parseOptionsField(BSONObj options);

    /**
     * Creates a configuration string suitable for 'config' parameter in WT_SESSION::create().
     * It is possible for 'tableName' to be an empty string, in the case of internal-only temporary
     * tables. Configuration string is constructed from the parameters of 'wtTableConfig', with
     * extra settings when 'isOplog' is true.
     */
    static std::string generateCreateString(
        StringData tableName,
        const WiredTigerRecordStore::WiredTigerTableConfig& wtTableConfig,
        bool isOplog = false);

    WiredTigerRecordStore(WiredTigerKVEngineBase* kvEngine, WiredTigerRecoveryUnit&, Params params);

    ~WiredTigerRecordStore() override;

    const char* name() const override {
        return _engineName.c_str();
    }

    KeyFormat keyFormat() const override {
        return std::visit(OverloadedVisitor(
                              [](const WiredTigerIntegerKeyedContainer& v) -> KeyFormat {
                                  return KeyFormat::Long;
                              },
                              [](const WiredTigerStringKeyedContainer& v) -> KeyFormat {
                                  return KeyFormat::String;
                              }),
                          _container);
    }

    StringData getURI() const {
        return std::visit([](const auto& v) -> StringData { return v.uri(); }, _container);
    }

    uint64_t tableId() const {
        return std::visit([](const auto& v) -> uint64_t { return v.tableId(); }, _container);
    }

    long long dataSize() const override;

    long long numRecords() const override;

    int64_t storageSize(RecoveryUnit&,
                        BSONObjBuilder* extraInfo = nullptr,
                        int infoLevel = 0) const override;

    int64_t freeStorageSize(RecoveryUnit& ru) const override;

    bool updateWithDamagesSupported() const override;

    void printRecordMetadata(const RecordId& recordId,
                             std::set<Timestamp>* recordTimestamps) const override;

    std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx,
                                                    RecoveryUnit& ru,
                                                    bool forward = true) const override;

    std::unique_ptr<RecordCursor> getRandomCursor(OperationContext* opCtx,
                                                  RecoveryUnit& ru) const override;

    bool compactSupported() const override {
        return !_inMemory;
    }

    void validate(RecoveryUnit& ru,
                  const CollectionValidation::ValidationOptions& options,
                  ValidateResults* results) override;

    void appendNumericCustomStats(RecoveryUnit& ru,
                                  BSONObjBuilder* result,
                                  double scale) const override;

    void appendAllCustomStats(RecoveryUnit& ru,
                              BSONObjBuilder* result,
                              double scale) const override;

    void updateStatsAfterRepair(long long numRecords, long long dataSize) override;

    RecordId getLargestKey(OperationContext* opCtx, RecoveryUnit& ru) const override;

    void reserveRecordIds(OperationContext* opCtx,
                          RecoveryUnit& ru,
                          std::vector<RecordId>* out,
                          size_t nRecords) override;

    RecordStore::Capped* capped() override;

    RecordStore::Oplog* oplog() override;

    /*
     * Check the size information for this RecordStore. This function opens a cursor on the
     * RecordStore to determine if it is empty. If it is empty, it will mark the collection as
     * needing size adjustment as a result of a rollback or storage recovery event.
     */
    void checkSize(OperationContext* opCtx, RecoveryUnit& ru);

    void setSizeStorer(WiredTigerSizeStorer* ss) {
        _sizeStorer = ss;
    }

    /**
     * Sets the new number of records and flushes the size storer.
     */
    void setNumRecords(long long numRecords);

    /**
     * Sets the new data size and flushes the size storer.
     */
    void setDataSize(long long dataSize);

    RecordStore::RecordStoreContainer getContainer() override;

protected:
    // Encapsulates statistics for an operation performed on this RecordStore instance.
    struct OpStats {
        int64_t keyLength{0};
        int64_t oldValueLength{0};
        int64_t newValueLength{0};
    };

    /**
     * Deletes the specified record from this WiredTiger table. Resets 'opStats' before populating
     * it with statistics corresponding to this operation.
     */
    void wtDeleteRecord(OperationContext*,
                        WiredTigerRecoveryUnit& wtRu,
                        const RecordId&,
                        OpStats& opStats);

    /**
     * Inserts the given record into this WiredTiger table. Returns error if the record could not be
     * inserted due to errors like duplicate key error. Resets 'opStats' before populating it with
     * statistics corresponding to this operation.
     */
    Status wtInsertRecord(OperationContext*,
                          WiredTigerRecoveryUnit& wtRu,
                          WT_CURSOR* c,
                          const Record& record,
                          OpStats& opStats);

    /**
     * Updates the given record in this WiredTiger table. Resets 'opStats' before populating it with
     * statistics corresponding to this operation.
     */
    Status wtUpdateRecord(OperationContext* opCtx,
                          WiredTigerRecoveryUnit& wtRu,
                          const RecordId& id,
                          const char* data,
                          int len,
                          OpStats& opStats);

    /**
     * Deletes all records in this WiredTiger table.
     */
    void wtTruncate(OperationContext* opCtx, WiredTigerRecoveryUnit& wtRu);

    /**
     * Deletes all Records in the range [minRecordId, maxRecordId].
     */
    void wtRangeTruncate(WiredTigerRecoveryUnit& wtRu,
                         const RecordId& minRecordId = RecordId(),
                         const RecordId& maxRecordId = RecordId());

    /**
     * Compacts this WiredTiger table to attempt to reduce its storage space.
     */
    StatusWith<int64_t> wtCompact(OperationContext* opCtx,
                                  WiredTigerRecoveryUnit& wtRu,
                                  const CompactOptions& options);

    void _deleteRecord(OperationContext*, RecoveryUnit&, const RecordId&) override;

    Status _insertRecords(OperationContext*,
                          RecoveryUnit&,
                          std::vector<Record>*,
                          const std::vector<Timestamp>&) override;

    Status _updateRecord(
        OperationContext*, RecoveryUnit&, const RecordId&, const char* data, int len) override;

    StatusWith<RecordData> _updateWithDamages(OperationContext*,
                                              RecoveryUnit&,
                                              const RecordId&,
                                              const RecordData&,
                                              const char* damageSource,
                                              const DamageVector&) override;

    Status _truncate(OperationContext*, RecoveryUnit&) override;

    Status _rangeTruncate(OperationContext*,
                          RecoveryUnit&,
                          const RecordId& minRecordId = RecordId(),
                          const RecordId& maxRecordId = RecordId(),
                          int64_t hintDataSizeIncrement = 0,
                          int64_t hintNumRecordsIncrement = 0) override;

    StatusWith<int64_t> _compact(OperationContext*, RecoveryUnit&, const CompactOptions&) override;

    long long _reserveIdBlock(OperationContext* opCtx, RecoveryUnit& ru, size_t nRecords);

    /**
     * Adjusts the record count and data size metadata for this record store. The function consults
     * the SizeRecoveryState to determine whether or not to actually change the size metadata if the
     * server is undergoing recovery.
     *
     * For most record stores, we will not update the size metadata during recovery, as we trust
     * that the values in the SizeStorer are accurate with respect to the end state of recovery.
     * However, there are two exceptions:
     *
     *   1. When a record store is created as part of the recovery process. The SizeStorer will have
     *      no information about that newly-created ident.
     *   2. When a record store is created at startup but constains no records as of the stable
     *      checkpoint timestamp. In this scenario, we will assume that the record store has a size
     *      of zero and will discard all cached size metadata. This assumption is incorrect if there
     *      are pending writes to this ident as part of the recovery process, and so we must
     *      always adjust size metadata for these idents.
     */
    void _changeNumRecordsAndDataSize(RecoveryUnit& ru,
                                      int64_t numRecordDiff,
                                      int64_t dataSizeDiff);

    class RandomCursor;

    /**
     * Initialize the largest known RecordId if it is not already. This is designed to be called
     * immediately before operations that may need this Recordid. This is to support lazily
     * initializing the value instead of all at once during startup.
     */
    void _initNextIdIfNeeded(OperationContext* opCtx, RecoveryUnit& ru);

    /**
     * Updates the in-memory largest known RecordId field to ensure that recordIds
     * are not reused. Takes in a 'largestSeen', which is the largest recordId that has been seen
     * by the caller. The in-memory value is updated only if the provided new value is larger.
     *
     * TODO (SERVER-88375): Remove all code related to the record store having to keep track
     * of the largest recordId seen.
     */
    void _updateLargestRecordId(OperationContext* opCtx, RecoveryUnit& ru, long long largestSeen);

    const std::string _engineName;
    std::variant<WiredTigerIntegerKeyedContainer, WiredTigerStringKeyedContainer> _container;
    const bool _overwrite;
    const bool _isLogged;
    const bool _forceUpdateWithFullDocument;
    const bool _inMemory;

    // Protects initialization of the _nextIdNum.
    mutable stdx::mutex _initNextIdMutex;
    AtomicWord<long long> _nextIdNum{0};

    WiredTigerSizeStorer* _sizeStorer;  // not owned, can be NULL
    std::shared_ptr<WiredTigerSizeStorer::SizeInfo> _sizeInfo;
    bool _tracksSizeAdjustments;
    WiredTigerKVEngineBase* _kvEngine;  // not owned.

private:
    std::variant<WiredTigerIntegerKeyedContainer, WiredTigerStringKeyedContainer> _makeContainer(
        Params& params);
};

class WiredTigerRecordStore::Capped : public WiredTigerRecordStore, public RecordStoreBase::Capped {
public:
    Capped(WiredTigerKVEngine*, WiredTigerRecoveryUnit&, Params);

    std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext*,
                                                    RecoveryUnit&,
                                                    bool forward = true) const override;

    int64_t storageSize(RecoveryUnit&,
                        BSONObjBuilder* extraInfo = nullptr,
                        int infoLevel = 0) const override;

    RecordStore::Capped* capped() override;

private:
    TruncateAfterResult _truncateAfter(OperationContext*,
                                       RecoveryUnit&,
                                       const RecordId&,
                                       bool inclusive) override;

    virtual void _handleTruncateAfter(WiredTigerRecoveryUnit&, const RecordId& lastKeptId);
};

class WiredTigerRecordStore::Oplog final : public WiredTigerRecordStore::Capped,
                                           public RecordStoreBase::Oplog {
public:
    struct Params {
        UUID uuid;
        std::string ident;
        std::string engineName;
        bool inMemory;
        int64_t oplogMaxSize;
        WiredTigerSizeStorer* sizeStorer;
        bool tracksSizeAdjustments;
        bool isLogged;
        bool forceUpdateWithFullDocument;
    };

    Oplog(WiredTigerKVEngine*, WiredTigerRecoveryUnit&, Params);

    ~Oplog() override;

    std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext*,
                                                    RecoveryUnit&,
                                                    bool forward = true) const override;

    void validate(RecoveryUnit&,
                  const CollectionValidation::ValidationOptions&,
                  ValidateResults*) override;

    RecordStore::Capped* capped() override;

    RecordStore::Oplog* oplog() override;

    Status updateSize(long long size) override;

    int64_t getMaxSize() const override;

    std::unique_ptr<SeekableRecordCursor> getRawCursor(OperationContext* opCtx,
                                                       RecoveryUnit& ru,
                                                       bool forward) const override;

    StatusWith<Timestamp> getLatestTimestamp(RecoveryUnit&) const override;

    StatusWith<Timestamp> getEarliestTimestamp(RecoveryUnit&) override;

private:
    Status _insertRecords(OperationContext*,
                          RecoveryUnit&,
                          std::vector<Record>*,
                          const std::vector<Timestamp>&) override;

    void _handleTruncateAfter(WiredTigerRecoveryUnit&, const RecordId& lastKeptId) override;

    AtomicWord<int64_t> _maxSize;
};

class WiredTigerRecordStoreCursorBase : public SeekableRecordCursor {
public:
    WiredTigerRecordStoreCursorBase(OperationContext* opCtx,
                                    RecoveryUnit& ru,
                                    const WiredTigerRecordStore& rs,
                                    bool forward);

    /**
     * Initialize the cursor. We use this separate method for initialization to avoid calling
     * virtual methods in the constuctor.
     */
    void init();

    boost::optional<Record> next() override;

    boost::optional<Record> seek(const RecordId& start, BoundInclusion boundInclusion) override;

    boost::optional<Record> seekExact(const RecordId& id) override;

    void save() override;

    void saveUnpositioned() override;

    bool restore(RecoveryUnit& ru, bool tolerateCappedRepositioning = true) override;

    void detachFromOperationContext() override;

    void reattachToOperationContext(OperationContext* opCtx) override;

    void setSaveStorageCursorOnDetachFromOperationContext(bool saveCursor) override {
        _saveStorageCursorOnDetachFromOperationContext = saveCursor;
    }

protected:
    /**
     * Resets the cursor.
     */
    void resetCursor();

    /**
     * For next() implementations, ensures that keys are returned in-order. Should be called before
     * trackReturn().
     */
    void checkOrder(const RecordId& id) const;

    /**
     * For public functions that return data, must be called before returning a Record.
     */
    void trackReturn(const Record& record);

    /**
     * Advances the cursor and returns the next RecordId, if one exists.
     */
    RecordId nextIdCommon();

    /**
     * Perform a bounded seek on the cursor, and return the next matching RecordId, if one exists.
     *
     * If countSeekMetric is false, does not record this seek towards metrics collection.
     * If restoring is true, this seek is part of a restore operation.
     */
    RecordId seekIdCommon(const RecordId& id,
                          BoundInclusion boundInclusion,
                          bool restoring = false);

    /**
     * Perform an exact seek on the cursor, and return the Record, if one exists.
     */
    boost::optional<Record> seekExactCommon(const RecordId& id);

    const uint64_t _tableId;
    RecordId _lastReturnedId;  // If null, need to seek to first/last record.
    OperationContext* _opCtx;
    RecoveryUnit* _ru;
    const std::string _uri;
    const std::string _ident;
    boost::optional<WiredTigerCursor> _cursor;
    const KeyFormat _keyFormat;
    const bool _forward;
    const boost::optional<UUID> _uuid;
    bool _skipNextAdvance = false;
    bool _eof = false;
    bool _hasRestored = true;
    bool _boundSet = false;
    // Whether or not the underlying WT cursor is positioned on a record.
    bool _positioned = false;
    const bool _assertOutOfOrderForTest = false;

private:
    void reportOutOfOrderRead(const RecordId& id, bool failWithOutOfOrderForTest) const;

    bool _saveStorageCursorOnDetachFromOperationContext = false;
};

/**
 * The sole purpose of this class is to take a constructor param of type WiredTigerRecordStore and
 * thus ensure that only WiredTigerRecordStore attempts to create an instance of this class.
 */
class WiredTigerRecordStoreCursor : public WiredTigerRecordStoreCursorBase {
public:
    WiredTigerRecordStoreCursor(OperationContext* opCtx,
                                RecoveryUnit& ru,
                                const WiredTigerRecordStore& rs,
                                bool forward);
};

/**
 * WiredTigerCappedCursorBase is an abstract class for a capped cursors. Derived classes must
 * implement a few "visibility" functions to ensure Records are returned correctly following a set
 * of implementation rules.
 *
 * Note: forward cursors respect visibility rules, reverse cursors do not.
 */
class WiredTigerCappedCursorBase : public WiredTigerRecordStoreCursor {
public:
    WiredTigerCappedCursorBase(OperationContext* opCtx,
                               RecoveryUnit& ru,
                               const WiredTigerRecordStore& rs,
                               bool forward);

    boost::optional<Record> next() override;

    boost::optional<Record> seek(const RecordId& start, BoundInclusion boundInclusion) override;

    boost::optional<Record> seekExact(const RecordId& id) override;

    void save() override;

    bool restore(RecoveryUnit& ru, bool tolerateCappedRepositioning = true) override;

protected:
    /**
     * Initialize any state required to enforce visibility constraints on this cursor.
     */
    virtual void initVisibility() {};

    /**
     * Returns true if a RecordId should be visible to this cursor.
     */
    virtual bool isVisible(const RecordId& id) {
        return true;
    };

    /**
     * Reset any visibility state on the cursor.
     */
    virtual void resetVisibility() {};
};

/**
 * WiredTigerStandardCappedCursor implements a cursor on a non-oplog capped collection.
 *
 * Note: forward cursors respect visibility rules, reverse cursors do not.
 */
class WiredTigerStandardCappedCursor final : public WiredTigerCappedCursorBase {
public:
    WiredTigerStandardCappedCursor(OperationContext* opCtx,
                                   RecoveryUnit& ru,
                                   const WiredTigerRecordStore& rs,
                                   bool forward);
};

/**
 * WiredTigerStandardCappedCursor implements a cursor on the oplog.
 *
 * Note: forward cursors respect visibility rules, reverse cursors do not.
 */
class WiredTigerOplogCursor final : public WiredTigerCappedCursorBase {
public:
    WiredTigerOplogCursor(OperationContext* opCtx,
                          RecoveryUnit& ru,
                          const WiredTigerRecordStore& rs,
                          bool forward);

    boost::optional<Record> next() override;
    boost::optional<Record> seek(const RecordId& start, BoundInclusion boundInclusion) override;

protected:
    void initVisibility() override;
    bool isVisible(const RecordId& id) override;
    void resetVisibility() override;

private:
    /**
     * This value is used for visibility calculations on what oplog entries can be returned to a
     * client. This value *must* be initialized/updated *before* a WiredTiger snapshot is
     * established.
     */
    boost::optional<std::int64_t> _oplogVisibleTs = boost::none;

    /**
     * With WT-8601, WiredTiger no longer maintains commit_timestamp information on writes to logged
     * tables, such as the oplog. There are occasions where the server applies a TimestampReadsource
     * (e.g: majority) and expects the storage engine to obey the prior semantics. When a cursor is
     * opened against the oplog, we will populate this variable with the recovery unit's read
     * timestamp to apply a visibility check.
     */
    boost::optional<std::int64_t> _readTimestampForOplog = boost::none;
};

// WT failpoint to throw write conflict exceptions randomly
extern FailPoint WTWriteConflictException;
extern FailPoint WTWriteConflictExceptionForReads;
}  // namespace mongo
