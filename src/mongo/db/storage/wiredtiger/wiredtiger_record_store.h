// wiredtiger_record_store.h


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

#include <memory>
#include <set>
#include <string>
#include <wiredtiger.h>

#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/storage/capped_callback.h"
#include "mongo/db/storage/kv/kv_prefix.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_cursor.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_size_storer.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/fail_point_service.h"

/**
 * Either executes the specified operation and returns it's value or randomly throws a write
 * conflict exception if the WTWriteConflictException failpoint is enabled. This is only checked
 * on cursor methods that make modifications.
 */
#define WT_OP_CHECK(x) (((MONGO_FAIL_POINT(WTWriteConflictException))) ? (WT_ROLLBACK) : (x))

/**
 * Identical to WT_OP_CHECK except this is checked on cursor seeks/advancement.
 */
#define WT_READ_CHECK(x) \
    (((MONGO_FAIL_POINT(WTWriteConflictExceptionForReads))) ? (WT_ROLLBACK) : (x))

namespace mongo {

class RecoveryUnit;
class WiredTigerSessionCache;
class WiredTigerSizeStorer;

extern const std::string kWiredTigerEngineName;

class WiredTigerRecordStore : public RecordStore {
    friend class WiredTigerRecordStoreCursorBase;

    friend class StandardWiredTigerRecordStore;
    friend class PrefixedWiredTigerRecordStore;

public:
    /**
     * Parses collections options for wired tiger configuration string for table creation.
     * The document 'options' is typically obtained from the 'wiredTiger' field of
     * CollectionOptions::storageEngine.
     */
    static StatusWith<std::string> parseOptionsField(const BSONObj options);

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
                                                        StringData ns,
                                                        const CollectionOptions& options,
                                                        StringData extraStrings,
                                                        bool prefixed);

    struct Params {
        StringData ns;
        std::string ident;
        std::string engineName;
        bool isCapped;
        bool isEphemeral;
        int64_t cappedMaxSize;
        int64_t cappedMaxDocs;
        CappedCallback* cappedCallback;
        WiredTigerSizeStorer* sizeStorer;
        bool isReadOnly;
    };

    WiredTigerRecordStore(WiredTigerKVEngine* kvEngine, OperationContext* opCtx, Params params);

    virtual ~WiredTigerRecordStore();

    virtual void postConstructorInit(OperationContext* opCtx);

    // name of the RecordStore implementation
    virtual const char* name() const;

    virtual long long dataSize(OperationContext* opCtx) const;

    virtual long long numRecords(OperationContext* opCtx) const;

    virtual bool isCapped() const;

    virtual int64_t storageSize(OperationContext* opCtx,
                                BSONObjBuilder* extraInfo = NULL,
                                int infoLevel = 0) const;

    // CRUD related

    virtual bool findRecord(OperationContext* opCtx, const RecordId& id, RecordData* out) const;

    virtual void deleteRecord(OperationContext* opCtx, const RecordId& id);

    virtual Status insertRecords(OperationContext* opCtx,
                                 std::vector<Record>* records,
                                 const std::vector<Timestamp>& timestamps);

    virtual Status insertRecordsWithDocWriter(OperationContext* opCtx,
                                              const DocWriter* const* docs,
                                              const Timestamp* timestamps,
                                              size_t nDocs,
                                              RecordId* idsOut);

    virtual Status updateRecord(OperationContext* opCtx,
                                const RecordId& recordId,
                                const char* data,
                                int len);

    virtual bool updateWithDamagesSupported() const;

    virtual StatusWith<RecordData> updateWithDamages(OperationContext* opCtx,
                                                     const RecordId& id,
                                                     const RecordData& oldRec,
                                                     const char* damageSource,
                                                     const mutablebson::DamageVector& damages);

    virtual std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx,
                                                            bool forward) const = 0;

    std::unique_ptr<RecordCursor> getRandomCursor(OperationContext* opCtx) const final;

    virtual std::unique_ptr<RecordCursor> getRandomCursorWithOptions(
        OperationContext* opCtx, StringData extraConfig) const = 0;

    virtual Status truncate(OperationContext* opCtx);

    virtual bool compactSupported() const {
        return !_isEphemeral;
    }
    virtual bool compactsInPlace() const {
        return true;
    }

    virtual Timestamp getPinnedOplog() const final;

    virtual Status compact(OperationContext* opCtx) final;

    virtual bool isInRecordIdOrder() const override {
        return true;
    }

    virtual Status validate(OperationContext* opCtx,
                            ValidateCmdLevel level,
                            ValidateAdaptor* adaptor,
                            ValidateResults* results,
                            BSONObjBuilder* output);

    virtual void appendCustomStats(OperationContext* opCtx,
                                   BSONObjBuilder* result,
                                   double scale) const;

    virtual Status touch(OperationContext* opCtx, BSONObjBuilder* output) const;

    virtual void cappedTruncateAfter(OperationContext* opCtx, RecordId end, bool inclusive);

    virtual boost::optional<RecordId> oplogStartHack(OperationContext* opCtx,
                                                     const RecordId& startingPosition) const;

    virtual Status oplogDiskLocRegister(OperationContext* opCtx,
                                        const Timestamp& opTime,
                                        bool orderedCommit);

    virtual void updateStatsAfterRepair(OperationContext* opCtx,
                                        long long numRecords,
                                        long long dataSize);


    void waitForAllEarlierOplogWritesToBeVisible(OperationContext* opCtx) const override;

    Status updateCappedSize(OperationContext* opCtx, long long cappedSize) final;

    void setCappedCallback(CappedCallback* cb) {
        stdx::lock_guard<stdx::mutex> lk(_cappedCallbackMutex);
        _cappedCallback = cb;
    }

    int64_t cappedMaxDocs() const;
    int64_t cappedMaxSize() const;

    const std::string& getURI() const {
        return _uri;
    }

    const std::string& getIdent() const override {
        return _ident;
    }

    uint64_t tableId() const {
        return _tableId;
    }

    void setSizeStorer(WiredTigerSizeStorer* ss) {
        _sizeStorer = ss;
    }

    bool isOpHidden_forTest(const RecordId& id) const;

    bool inShutdown() const;

    void reclaimOplog(OperationContext* opCtx);

    /**
     * The `recoveryTimestamp` is when replication recovery would need to replay from for
     * recoverable rollback, or restart for durable engines. `reclaimOplog` will not
     * truncate oplog entries in front of this time.
     */
    void reclaimOplog(OperationContext* opCtx, Timestamp recoveryTimestamp);

    // Returns false if the oplog was dropped while waiting for a deletion request.
    bool yieldAndAwaitOplogDeletionRequest(OperationContext* opCtx);

    bool haveCappedWaiters();

    void notifyCappedWaitersIfNeeded();

    class OplogStones;

    // Exposed only for testing.
    OplogStones* oplogStones() {
        return _oplogStones.get();
    };

protected:
    virtual RecordId getKey(WT_CURSOR* cursor) const = 0;

    virtual void setKey(WT_CURSOR* cursor, RecordId id) const = 0;

private:
    class RandomCursor;

    class NumRecordsChange;
    class DataSizeChange;

    static WiredTigerRecoveryUnit* _getRecoveryUnit(OperationContext* opCtx);

    Status _insertRecords(OperationContext* opCtx,
                          Record* records,
                          const Timestamp* timestamps,
                          size_t nRecords);

    RecordId _nextId();
    void _setId(RecordId id);
    bool cappedAndNeedDelete() const;
    RecordData _getData(const WiredTigerCursor& cursor) const;

    /**
     * Position the cursor at the first key. The previously known first key is
     * provided, as well as an indicator that this is being positioned for
     * use by a truncate call.
     */
    void _positionAtFirstRecordId(OperationContext* opCtx,
                                  WT_CURSOR* cursor,
                                  const RecordId& firstKey,
                                  bool forTruncate) const;

    /**
     * Adjusts the record count and data size metadata for this record store, respectively. These
     * functions consult the SizeRecoveryState to determine whether or not to actually change the
     * size metadata if the server is undergoing recovery.
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
    void _changeNumRecords(OperationContext* opCtx, int64_t diff);
    void _increaseDataSize(OperationContext* opCtx, int64_t amount);

    /**
     * Delete records from this record store as needed while _cappedMaxSize or _cappedMaxDocs is
     * exceeded.
     *
     * _inlock version to be called once a lock has been acquired.
     */
    int64_t _cappedDeleteAsNeeded(OperationContext* opCtx, const RecordId& justInserted);
    int64_t _cappedDeleteAsNeeded_inlock(OperationContext* opCtx, const RecordId& justInserted);

    const std::string _uri;
    const std::string _ident;
    const uint64_t _tableId;  // not persisted

    // Canonical engine name to use for retrieving options
    const std::string _engineName;
    // The capped settings should not be updated once operations have started
    const bool _isCapped;
    // True if the storage engine is an in-memory storage engine
    const bool _isEphemeral;
    // True if the namespace of this record store starts with "local.oplog.", and false otherwise.
    const bool _isOplog;
    int64_t _cappedMaxSize;
    const int64_t _cappedMaxSizeSlack;  // when to start applying backpressure
    const int64_t _cappedMaxDocs;
    RecordId _cappedFirstRecord;
    AtomicInt64 _cappedSleep;
    AtomicInt64 _cappedSleepMS;
    CappedCallback* _cappedCallback;
    bool _shuttingDown;
    mutable stdx::mutex _cappedCallbackMutex;  // guards _cappedCallback and _shuttingDown

    // See comment in ::cappedDeleteAsNeeded
    int _cappedDeleteCheckCount;
    mutable stdx::timed_mutex _cappedDeleterMutex;

    AtomicInt64 _nextIdNum;

    WiredTigerSizeStorer* _sizeStorer;  // not owned, can be NULL
    std::shared_ptr<WiredTigerSizeStorer::SizeInfo> _sizeInfo;
    WiredTigerKVEngine* _kvEngine;  // not owned.

    // Non-null if this record store is underlying the active oplog.
    std::shared_ptr<OplogStones> _oplogStones;
};


class StandardWiredTigerRecordStore final : public WiredTigerRecordStore {
public:
    StandardWiredTigerRecordStore(WiredTigerKVEngine* kvEngine,
                                  OperationContext* opCtx,
                                  Params params);

    virtual std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx,
                                                            bool forward) const override;

    virtual std::unique_ptr<RecordCursor> getRandomCursorWithOptions(
        OperationContext* opCtx, StringData extraConfig) const override;

protected:
    virtual RecordId getKey(WT_CURSOR* cursor) const;

    virtual void setKey(WT_CURSOR* cursor, RecordId id) const;
};

class PrefixedWiredTigerRecordStore final : public WiredTigerRecordStore {
public:
    PrefixedWiredTigerRecordStore(WiredTigerKVEngine* kvEngine,
                                  OperationContext* opCtx,
                                  Params params,
                                  KVPrefix prefix);

    virtual std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx,
                                                            bool forward) const override;

    virtual std::unique_ptr<RecordCursor> getRandomCursorWithOptions(
        OperationContext* opCtx, StringData extraConfig) const override;

    virtual KVPrefix getPrefix() const {
        return _prefix;
    }

protected:
    virtual RecordId getKey(WT_CURSOR* cursor) const;

    virtual void setKey(WT_CURSOR* cursor, RecordId id) const;

private:
    KVPrefix _prefix;
};

class WiredTigerRecordStoreCursorBase : public SeekableRecordCursor {
public:
    WiredTigerRecordStoreCursorBase(OperationContext* opCtx,
                                    const WiredTigerRecordStore& rs,
                                    bool forward);

    boost::optional<Record> next();

    boost::optional<Record> seekExact(const RecordId& id);

    void save();

    void saveUnpositioned();

    bool restore();

    void detachFromOperationContext();

    void reattachToOperationContext(OperationContext* opCtx);

protected:
    virtual RecordId getKey(WT_CURSOR* cursor) const = 0;

    virtual void setKey(WT_CURSOR* cursor, RecordId id) const = 0;

    /**
     * Callers must have already checked the return value of a positioning method against
     * 'WT_NOTFOUND'. This method allows for additional predicates to be considered on a validly
     * positioned cursor. 'id' is an out parameter. Implementations are not required to fill it
     * in. It's simply a possible optimization to avoid a future 'getKey' call if 'hasWrongPrefix'
     * already did one.
     */
    virtual bool hasWrongPrefix(WT_CURSOR* cursor, RecordId* id) const = 0;

    /**
     * Called when restoring a cursor that has not been advanced.
     */
    virtual void initCursorToBeginning() = 0;

    const WiredTigerRecordStore& _rs;
    OperationContext* _opCtx;
    const bool _forward;
    bool _skipNextAdvance = false;
    boost::optional<WiredTigerCursor> _cursor;
    bool _eof = false;
    RecordId _lastReturnedId;  // If null, need to seek to first/last record.

private:
    bool isVisible(const RecordId& id);
};

class WiredTigerRecordStoreStandardCursor final : public WiredTigerRecordStoreCursorBase {
public:
    WiredTigerRecordStoreStandardCursor(OperationContext* opCtx,
                                        const WiredTigerRecordStore& rs,
                                        bool forward = true);

protected:
    virtual RecordId getKey(WT_CURSOR* cursor) const override;

    virtual void setKey(WT_CURSOR* cursor, RecordId id) const override;

    /**
     * Callers must have already checked the return value of a positioning method against
     * 'WT_NOTFOUND'. This method allows for additional predicates to be considered on a validly
     * positioned cursor. 'id' is an out parameter. Implementations are not required to fill it
     * in. It's simply a possible optimization to avoid a future 'getKey' call if 'hasWrongPrefix'
     * already did one.
     */
    virtual bool hasWrongPrefix(WT_CURSOR* cursor, RecordId* id) const override;

    virtual void initCursorToBeginning(){};
};

class WiredTigerRecordStorePrefixedCursor final : public WiredTigerRecordStoreCursorBase {
public:
    WiredTigerRecordStorePrefixedCursor(OperationContext* opCtx,
                                        const WiredTigerRecordStore& rs,
                                        KVPrefix prefix,
                                        bool forward = true);

protected:
    virtual RecordId getKey(WT_CURSOR* cursor) const override;

    virtual void setKey(WT_CURSOR* cursor, RecordId id) const override;

    /**
     * Callers must have already checked the return value of a positioning method against
     * 'WT_NOTFOUND'. This method allows for additional predicates to be considered on a validly
     * positioned cursor. 'id' is an out parameter. Implementations are not required to fill it
     * in. It's simply a possible optimization to avoid a future 'getKey' call if 'hasWrongPrefix'
     * already did one.
     */
    virtual bool hasWrongPrefix(WT_CURSOR* cursor, RecordId* id) const override;

    virtual void initCursorToBeginning() override;

private:
    KVPrefix _prefix;
};


// WT failpoint to throw write conflict exceptions randomly
MONGO_FAIL_POINT_DECLARE(WTWriteConflictException);
MONGO_FAIL_POINT_DECLARE(WTWriteConflictExceptionForReads);

// Prevents oplog writes from being considered durable on the primary. Once activated, new writes
// will not be considered durable until deactivated. It is unspecified whether writes that commit
// before activation will become visible while active.
MONGO_FAIL_POINT_DECLARE(WTPausePrimaryOplogDurabilityLoop);
}
