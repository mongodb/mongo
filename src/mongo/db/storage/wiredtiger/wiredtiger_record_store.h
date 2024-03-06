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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <variant>
#include <vector>
#include <wiredtiger.h>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/mutable/damage_vector.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/capped_visibility.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/validate_results.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/stats/resource_consumption_metrics.h"
#include "mongo/db/storage/collection_truncate_markers.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_cursor.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_size_storer.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/uuid.h"

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
class WiredTigerSessionCache;
class WiredTigerSizeStorer;

inline constexpr auto kWiredTigerEngineName = "wiredTiger"_sd;

class WiredTigerRecordStore : public RecordStore {
    friend class WiredTigerRecordStoreCursorBase;

    friend class StandardWiredTigerRecordStore;

public:
    /**
     * Parses collections options for wired tiger configuration string for table creation.
     * The document 'options' is typically obtained from the 'wiredTiger' field of
     * CollectionOptions::storageEngine.
     */
    static StatusWith<std::string> parseOptionsField(BSONObj options);

    /**
     * Creates a configuration string suitable for 'config' parameter in WT_SESSION::create().
     * It is possible for 'ns' to be an empty string, in the case of internal-only temporary tables.
     * Configuration string is constructed from:
     *     built-in defaults
     *     storageEngine.wiredTiger.configString in 'options'
     *     'extraStrings'
     * Performs simple validation on the supplied parameters.
     * Returns error status if validation fails.
     * Note that even if this function returns an OK status, WT_SESSION:create() may still
     * fail with the constructed configuration string.
     */
    static StatusWith<std::string> generateCreateString(const std::string& engineName,
                                                        const NamespaceString& nss,
                                                        StringData ident,
                                                        const CollectionOptions& options,
                                                        StringData extraStrings,
                                                        KeyFormat keyFormat,
                                                        bool loggingEnabled);

    struct Params {
        NamespaceString nss;
        boost::optional<UUID> uuid;
        std::string ident;
        std::string engineName;
        bool isCapped;
        KeyFormat keyFormat;
        bool overwrite;
        bool isEphemeral;
        bool isLogged;
        int64_t oplogMaxSize = 0;
        WiredTigerSizeStorer* sizeStorer;
        bool tracksSizeAdjustments;
        bool forceUpdateWithFullDocument;
    };

    WiredTigerRecordStore(WiredTigerKVEngine* kvEngine, OperationContext* opCtx, Params params);

    virtual void getOplogTruncateStats(BSONObjBuilder& builder) const;

    virtual ~WiredTigerRecordStore();

    // Pass in NamespaceString, it is not possible to resolve the UUID to NamespaceString yet.
    virtual void postConstructorInit(OperationContext* opCtx, const NamespaceString& ns);

    // name of the RecordStore implementation
    virtual const char* name() const;

    virtual KeyFormat keyFormat() const;

    virtual long long dataSize(OperationContext* opCtx) const;

    virtual long long numRecords(OperationContext* opCtx) const;

    virtual bool selfManagedOplogTruncation() const {
        return true;
    }

    virtual int64_t storageSize(OperationContext* opCtx,
                                BSONObjBuilder* extraInfo = nullptr,
                                int infoLevel = 0) const;

    virtual int64_t freeStorageSize(OperationContext* opCtx) const;

    // CRUD related

    virtual bool findRecord(OperationContext* opCtx, const RecordId& id, RecordData* out) const;

    void doDeleteRecord(OperationContext* opCtx, const RecordId& id) final;

    Status doInsertRecords(OperationContext* opCtx,
                           std::vector<Record>* records,
                           const std::vector<Timestamp>& timestamps) final;

    Status doUpdateRecord(OperationContext* opCtx,
                          const RecordId& recordId,
                          const char* data,
                          int len) final;

    virtual bool updateWithDamagesSupported() const;

    StatusWith<RecordData> doUpdateWithDamages(OperationContext* opCtx,
                                               const RecordId& id,
                                               const RecordData& oldRec,
                                               const char* damageSource,
                                               const mutablebson::DamageVector& damages) final;

    virtual void printRecordMetadata(OperationContext* opCtx,
                                     const RecordId& recordId,
                                     std::set<Timestamp>* recordTimestamps) const;

    virtual std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx,
                                                            bool forward) const = 0;

    std::unique_ptr<RecordCursor> getRandomCursor(OperationContext* opCtx) const final;

    Status doTruncate(OperationContext* opCtx) final;
    Status doRangeTruncate(OperationContext* opCtx,
                           const RecordId& minRecordId,
                           const RecordId& maxRecordId,
                           int64_t hintDataSizeDiff,
                           int64_t hintNumRecordsDiff) final;

    virtual bool compactSupported() const {
        return !_isEphemeral;
    }
    virtual bool supportsOnlineCompaction() const {
        return true;
    }

    virtual Timestamp getPinnedOplog() const final;

    Status doCompact(OperationContext* opCtx, boost::optional<int64_t> freeSpaceTargetMB) final;

    virtual void validate(OperationContext* opCtx, bool full, ValidateResults* results);

    virtual void appendNumericCustomStats(OperationContext* opCtx,
                                          BSONObjBuilder* result,
                                          double scale) const;

    virtual void appendAllCustomStats(OperationContext* opCtx,
                                      BSONObjBuilder* result,
                                      double scale) const;

    void doCappedTruncateAfter(OperationContext* opCtx,
                               const RecordId& end,
                               bool inclusive,
                               const AboutToDeleteRecordCallback& aboutToDelete) final;

    virtual void updateStatsAfterRepair(OperationContext* opCtx,
                                        long long numRecords,
                                        long long dataSize);


    Status updateOplogSize(OperationContext* opCtx, long long newOplogSize) final;

    const std::string& getURI() const {
        return _uri;
    }

    uint64_t tableId() const {
        return _tableId;
    }

    NamespaceString ns(OperationContext* opCtx) const final;

    /*
     * Check the size information for this RecordStore. This function opens a cursor on the
     * RecordStore to determine if it is empty. If it is empty, it will mark the collection as
     * needing size adjustment as a result of a rollback or storage recovery event.
     */
    void checkSize(OperationContext* opCtx);

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

    bool isOpHidden_forTest(const RecordId& id) const;

    bool yieldAndAwaitOplogDeletionRequest(OperationContext* opCtx) override;

    /**
     * Attempts to truncate oplog entries before the pinned oplog timestamp. Truncation will occur
     * if the oplog is at capacity and the maximum retention time has elapsed.
     */
    void reclaimOplog(OperationContext* opCtx) override;

    StatusWith<Timestamp> getLatestOplogTimestamp(OperationContext* opCtx) const override;
    StatusWith<Timestamp> getEarliestOplogTimestamp(OperationContext* opCtx) override;

    class OplogTruncateMarkers;

    // Exposed only for testing.
    OplogTruncateMarkers* oplogTruncateMarkers() {
        return _oplogTruncateMarkers.get();
    };

    typedef std::variant<int64_t, WiredTigerItem> CursorKey;

    RecordId getLargestKey(OperationContext* opCtx) const final;

    void reserveRecordIds(OperationContext* opCtx,
                          std::vector<RecordId>* out,
                          size_t nRecords) final;

protected:
    virtual RecordId getKey(WT_CURSOR* cursor) const = 0;

    virtual void setKey(WT_CURSOR* cursor, const CursorKey* key) const = 0;

    Status oplogDiskLocRegisterImpl(OperationContext* opCtx,
                                    const Timestamp& opTime,
                                    bool orderedCommit) override;

    void waitForAllEarlierOplogWritesToBeVisibleImpl(OperationContext* opCtx) const override;

private:
    class RandomCursor;

    Status _insertRecords(OperationContext* opCtx,
                          Record* records,
                          const Timestamp* timestamps,
                          size_t nRecords);
    long long _reserveIdBlock(OperationContext* opCtx, size_t nRecords);
    RecordData _getData(const WiredTigerCursor& cursor) const;


    /**
     * Initialize the largest known RecordId if it is not already. This is designed to be called
     * immediately before operations that may need this Recordid. This is to support lazily
     * initializing the value instead of all at once during startup.
     */
    void _initNextIdIfNeeded(OperationContext* opCtx);

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
    void _changeNumRecordsAndDataSize(OperationContext* opCtx,
                                      int64_t numRecordDiff,
                                      int64_t dataSizeDiff);

    const std::string _uri;
    const uint64_t _tableId;  // not persisted

    // Canonical engine name to use for retrieving options
    const std::string _engineName;
    // The capped settings should not be updated once operations have started
    const bool _isCapped;
    // The format of this RecordStore's RecordId keys.
    const KeyFormat _keyFormat;
    // Whether or not to allow writes to overwrite existing records with the same RecordId.
    const bool _overwrite;
    // True if the storage engine is an in-memory storage engine
    const bool _isEphemeral;
    // True if WiredTiger is logging updates to this table
    const bool _isLogged;
    // True if the namespace of this record store starts with "local.oplog.", and false otherwise.
    const bool _isOplog;
    // True if the namespace of this record store starts with "config.system.change_collection", and
    // false otherwise.
    const bool _isChangeCollection;

    // TODO (SERVER-57482): Remove special handling of skipping "wiredtiger_calc_modify()".
    // True if force to update with the full document, and false otherwise.
    const bool _forceUpdateWithFullDocument;
    AtomicWord<int64_t> _oplogMaxSize;
    AtomicWord<Timestamp> _oplogFirstRecordTimestamp{Timestamp()};

    // Protects initialization of the _nextIdNum.
    mutable Mutex _initNextIdMutex = MONGO_MAKE_LATCH("WiredTigerRecordStore::_initNextIdMutex");
    AtomicWord<long long> _nextIdNum{0};

    WiredTigerSizeStorer* _sizeStorer;  // not owned, can be NULL
    std::shared_ptr<WiredTigerSizeStorer::SizeInfo> _sizeInfo;
    bool _tracksSizeAdjustments;
    WiredTigerKVEngine* _kvEngine;  // not owned.

    // Non-null if this record store is underlying the active oplog.
    std::shared_ptr<OplogTruncateMarkers> _oplogTruncateMarkers;

    AtomicWord<int64_t>
        _totalTimeTruncating;            // Cumulative amount of time spent truncating the oplog.
    AtomicWord<int64_t> _truncateCount;  // Cumulative number of truncates of the oplog.
};


class StandardWiredTigerRecordStore final : public WiredTigerRecordStore {
public:
    StandardWiredTigerRecordStore(WiredTigerKVEngine* kvEngine,
                                  OperationContext* opCtx,
                                  Params params);

    virtual std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx,
                                                            bool forward) const override;

protected:
    virtual RecordId getKey(WT_CURSOR* cursor) const override;

    virtual void setKey(WT_CURSOR* cursor, const CursorKey* key) const override;
};

class WiredTigerRecordStoreCursorBase : public SeekableRecordCursor {
public:
    WiredTigerRecordStoreCursorBase(OperationContext* opCtx,
                                    const WiredTigerRecordStore& rs,
                                    bool forward);

    boost::optional<Record> next();

    boost::optional<Record> seek(const RecordId& start, BoundInclusion boundInclusion);

    boost::optional<Record> seekExact(const RecordId& id);

    boost::optional<Record> seekNear(const RecordId& start);

    void save();

    void saveUnpositioned();

    bool restore(bool tolerateCappedRepositioning = true);

    void detachFromOperationContext();

    void reattachToOperationContext(OperationContext* opCtx);

    void setSaveStorageCursorOnDetachFromOperationContext(bool saveCursor) override {
        _saveStorageCursorOnDetachFromOperationContext = saveCursor;
    }

    /**
     *  Returns the checkpoint ID for checkpoint cursors, otherwise 0.
     */
    uint64_t getCheckpointId() const override {
        return _cursor->getCheckpointId();
    }

protected:
    virtual RecordId getKey(WT_CURSOR* cursor) const = 0;

    virtual void setKey(WT_CURSOR* cursor, const WiredTigerRecordStore::CursorKey* key) const = 0;

    /**
     * Called when restoring a cursor that has not been advanced.
     */
    virtual void initCursorToBeginning() = 0;

    const uint64_t _tableId;
    RecordId _lastReturnedId;  // If null, need to seek to first/last record.
    OperationContext* _opCtx;
    ResourceConsumption::MetricsCollector* _metrics;
    const std::string _uri;
    const std::string _ident;
    boost::optional<WiredTigerCursor> _cursor;
    const KeyFormat _keyFormat;
    const bool _forward;
    const bool _isOplog;
    const bool _isCapped;
    const boost::optional<UUID> _uuid;
    bool _skipNextAdvance = false;
    bool _eof = false;
    bool _hasRestored = true;

private:
    bool isVisible(const RecordId& id);
    void initCappedVisibility(OperationContext* opCtx);
    void reportOutOfOrderRead(RecordId& id, bool failWithOutOfOrderForTest);
    Record getRecord(WT_CURSOR* c, const RecordId& id);
    void resetCursor();

    /**
     * This value is used for visibility calculations on what oplog entries can be returned to a
     * client. This value *must* be initialized/updated *before* a WiredTiger snapshot is
     * established.
     */
    boost::optional<std::int64_t> _oplogVisibleTs = boost::none;
    boost::optional<CappedVisibilitySnapshot> _cappedSnapshot;

    /**
     * With WT-8601, WiredTiger no longer maintains commit_timestamp information on writes to logged
     * tables, such as the oplog. There are occasions where the server applies a TimestampReadsource
     * (e.g: majority) and expects the storage engine to obey the prior semantics. When a cursor is
     * opened against the oplog, we will populate this variable with the recovery unit's read
     * timestamp to apply a visibility check.
     */
    boost::optional<std::int64_t> _readTimestampForOplog = boost::none;
    bool _saveStorageCursorOnDetachFromOperationContext = false;
    bool _boundSet = false;
};

class WiredTigerRecordStoreStandardCursor final : public WiredTigerRecordStoreCursorBase {
public:
    WiredTigerRecordStoreStandardCursor(OperationContext* opCtx,
                                        const WiredTigerRecordStore& rs,
                                        bool forward = true);

protected:
    virtual RecordId getKey(WT_CURSOR* cursor) const override;

    virtual void setKey(WT_CURSOR* cursor,
                        const WiredTigerRecordStore::CursorKey* key) const override;

    virtual void initCursorToBeginning() override{};
};

// WT failpoint to throw write conflict exceptions randomly
extern FailPoint WTWriteConflictException;
extern FailPoint WTWriteConflictExceptionForReads;

// Prevents oplog writes from becoming visible asynchronously. Once activated, new writes will not
// be seen by regular readers until deactivated. It is unspecified whether writes that commit before
// activation will become visible while active.
extern FailPoint WTPauseOplogVisibilityUpdateLoop;
}  // namespace mongo
