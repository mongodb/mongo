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

#include <atomic>
#include <map>

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/storage/capped_callback.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_radix_store.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_visibility_manager.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"

namespace mongo {
namespace ephemeral_for_test {

/**
 * A RecordStore that stores all data in-memory.
 */
class RecordStore final : public ::mongo::RecordStore {
public:
    explicit RecordStore(StringData ns,
                         StringData ident,
                         KeyFormat keyFormat,
                         bool isCapped = false,
                         CappedCallback* cappedCallback = nullptr,
                         VisibilityManager* visibilityManager = nullptr);
    ~RecordStore() = default;

    virtual const char* name() const;
    virtual KeyFormat keyFormat() const {
        return _keyFormat;
    }
    virtual long long dataSize(OperationContext* opCtx) const;
    virtual long long numRecords(OperationContext* opCtx) const;
    virtual void setCappedCallback(CappedCallback*);
    virtual int64_t storageSize(OperationContext* opCtx,
                                BSONObjBuilder* extraInfo = nullptr,
                                int infoLevel = 0) const;

    virtual bool findRecord(OperationContext* opCtx, const RecordId& loc, RecordData* rd) const;

    void doDeleteRecord(OperationContext* opCtx, const RecordId& dl) final;

    Status doInsertRecords(OperationContext* opCtx,
                           std::vector<Record>* inOutRecords,
                           const std::vector<Timestamp>& timestamps) final;

    Status doUpdateRecord(OperationContext* opCtx,
                          const RecordId& oldLocation,
                          const char* data,
                          int len) final;

    virtual bool updateWithDamagesSupported() const;

    StatusWith<RecordData> doUpdateWithDamages(OperationContext* opCtx,
                                               const RecordId& loc,
                                               const RecordData& oldRec,
                                               const char* damageSource,
                                               const mutablebson::DamageVector& damages) final;

    virtual void printRecordMetadata(OperationContext* opCtx,
                                     const RecordId& recordId,
                                     std::set<Timestamp>* recordTimestamps) const {}

    std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx,
                                                    bool forward) const final;

    Status doTruncate(OperationContext* opCtx) final;
    StatusWith<int64_t> truncateWithoutUpdatingCount(RecoveryUnit* ru);

    void doCappedTruncateAfter(OperationContext* opCtx, RecordId end, bool inclusive) final;

    virtual void appendNumericCustomStats(OperationContext* opCtx,
                                          BSONObjBuilder* result,
                                          double scale) const {}

    virtual void updateStatsAfterRepair(OperationContext* opCtx,
                                        long long numRecords,
                                        long long dataSize);

protected:
    Status oplogDiskLocRegisterImpl(OperationContext* opCtx,
                                    const Timestamp& opTime,
                                    bool orderedCommit) override;

    void waitForAllEarlierOplogWritesToBeVisibleImpl(OperationContext* opCtx) const override;

private:
    friend class VisibilityManagerChange;

    void _initHighestIdIfNeeded(OperationContext* opCtx);

    /**
     * This gets the next (guaranteed) unique record id.
     */
    int64_t _nextRecordId(OperationContext* opCtx);

    const KeyFormat _keyFormat;
    const bool _isCapped;

    StringData _ident;

    std::string _prefix;
    std::string _postfix;

    mutable Mutex _cappedCallbackMutex =
        MONGO_MAKE_LATCH("RecordStore::_cappedCallbackMutex");  // Guards _cappedCallback
    CappedCallback* _cappedCallback;

    mutable Mutex _cappedDeleterMutex = MONGO_MAKE_LATCH("RecordStore::_cappedDeleterMutex");

    mutable Mutex _initHighestIdMutex = MONGO_MAKE_LATCH("RecordStore::_initHighestIdMutex");
    AtomicWord<long long> _highestRecordId{0};
    AtomicWord<long long> _numRecords{0};
    AtomicWord<long long> _dataSize{0};

    std::string generateKey(const uint8_t* key, size_t key_len) const;

    bool _isOplog;
    VisibilityManager* _visibilityManager;

    /**
     * Automatically adjust the record count and data size based on the size in change of the
     * underlying radix store during the life time of the SizeAdjuster.
     */
    friend class SizeAdjuster;
    class SizeAdjuster {
    public:
        SizeAdjuster(OperationContext* opCtx, RecordStore* rs);
        ~SizeAdjuster();

    private:
        OperationContext* const _opCtx;
        RecordStore* const _rs;
        const StringStore* _workingCopy;
        const int64_t _origNumRecords;
        const int64_t _origDataSize;
    };

    class Cursor final : public SeekableRecordCursor {
        OperationContext* opCtx;
        const RecordStore& _rs;
        StringStore::const_iterator it;
        boost::optional<std::string> _savedPosition;
        bool _needFirstSeek = true;
        bool _lastMoveWasRestore = false;
        VisibilityManager* _visibilityManager;
        RecordId _oplogVisibility;

        // Each cursor holds a strong reference to the recovery unit's working copy at the time of
        // the last next()/seek() on the cursor. This ensures that in between calls to
        // next()/seek(), the data pointed to by the cursor remains valid, even if the client
        // finishes a transaction and advances the snapshot.
        std::shared_ptr<StringStore> _workingCopy;

    public:
        Cursor(OperationContext* opCtx,
               const RecordStore& rs,
               VisibilityManager* visibilityManager);
        boost::optional<Record> next() final;
        boost::optional<Record> seekExact(const RecordId& id) final override;
        boost::optional<Record> seekNear(const RecordId& id) final override;
        void save() final;
        void saveUnpositioned() final override;
        bool restore(bool tolerateCappedRepositioning = true) final;
        void detachFromOperationContext() final;
        void reattachToOperationContext(OperationContext* opCtx) final;
        void setSaveStorageCursorOnDetachFromOperationContext(bool) override {
            // Noop for EFT, since we always keep the cursor's contents valid across save/restore.
        }

    private:
        bool inPrefix(const std::string& key_string);

        // Updates '_workingCopy' and 'it' to use the snapshot associated with the recovery unit,
        // if it has changed.
        void advanceSnapshotIfNeeded();
    };

    class ReverseCursor final : public SeekableRecordCursor {
        OperationContext* opCtx;
        const RecordStore& _rs;
        StringStore::const_reverse_iterator it;
        boost::optional<std::string> _savedPosition;
        bool _needFirstSeek = true;
        bool _lastMoveWasRestore = false;
        VisibilityManager* _visibilityManager;

        // Each cursor holds a strong reference to the recovery unit's working copy at the time of
        // the last next()/seek() on the cursor. This ensures that in between calls to
        // next()/seek(), the data pointed to by the cursor remains valid, even if the client
        // finishes a transaction and advances the snapshot.
        std::shared_ptr<StringStore> _workingCopy;

    public:
        ReverseCursor(OperationContext* opCtx,
                      const RecordStore& rs,
                      VisibilityManager* visibilityManager);
        boost::optional<Record> next() final;
        boost::optional<Record> seekExact(const RecordId& id) final override;
        boost::optional<Record> seekNear(const RecordId& id) final override;
        void save() final;
        void saveUnpositioned() final override;
        bool restore(bool tolerateCappedRepositioning = true) final;
        void detachFromOperationContext() final;
        void reattachToOperationContext(OperationContext* opCtx) final;
        void setSaveStorageCursorOnDetachFromOperationContext(bool) override {
            // Noop for EFT, since we always keep the cursor's contents valid across save/restore.
        }

    private:
        bool inPrefix(const std::string& key_string);

        // Updates '_workingCopy' and 'it' to use the snapshot associated with the recovery unit,
        // if it has changed.
        void advanceSnapshotIfNeeded();
    };
};

}  // namespace ephemeral_for_test
}  // namespace mongo
