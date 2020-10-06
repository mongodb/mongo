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
#include <mutex>
#include <set>

#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_radix_store.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_record_store.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_sorted_impl.h"
#include "mongo/db/storage/kv/kv_engine.h"

namespace mongo {
namespace ephemeral_for_test {

static constexpr char kEngineName[] = "ephemeralForTest";

class JournalListener;
/**
 * The ephemeral for test storage engine is intended for unit and performance testing.
 */
class KVEngine : public mongo::KVEngine {
public:
    KVEngine();
    virtual ~KVEngine();

    virtual mongo::RecoveryUnit* newRecoveryUnit();

    virtual Status createRecordStore(OperationContext* opCtx,
                                     StringData ns,
                                     StringData ident,
                                     const CollectionOptions& options);

    virtual Status importRecordStore(OperationContext* opCtx,
                                     StringData ident,
                                     const BSONObj& storageMetadata);

    virtual std::unique_ptr<mongo::RecordStore> getRecordStore(OperationContext* opCtx,
                                                               StringData ns,
                                                               StringData ident,
                                                               const CollectionOptions& options);

    virtual std::unique_ptr<mongo::RecordStore> makeTemporaryRecordStore(OperationContext* opCtx,
                                                                         StringData ident) override;

    virtual Status createSortedDataInterface(OperationContext* opCtx,
                                             const CollectionOptions& collOptions,
                                             StringData ident,
                                             const IndexDescriptor* desc);

    virtual Status importSortedDataInterface(OperationContext* opCtx,
                                             StringData ident,
                                             const BSONObj& storageMetadata);

    virtual Status dropGroupedSortedDataInterface(OperationContext* opCtx, StringData ident) {
        return Status::OK();
    }

    virtual std::unique_ptr<mongo::SortedDataInterface> getSortedDataInterface(
        OperationContext* opCtx, StringData ident, const IndexDescriptor* desc);

    virtual Status beginBackup(OperationContext* opCtx) {
        return Status::OK();
    }

    virtual void endBackup(OperationContext* opCtx) {}

    virtual Status dropIdent(mongo::RecoveryUnit* ru, StringData ident);

    virtual void dropIdentForImport(OperationContext* opCtx, StringData ident) {}

    virtual bool supportsDirectoryPerDB() const {
        return false;  // Not persistant so no Directories
    }

    virtual bool supportsCappedCollections() const {
        return true;
    }

    /**
     * Ephemeral for test does not write to disk.
     */
    virtual bool isDurable() const {
        return false;
    }

    virtual bool isEphemeral() const {
        return true;
    }

    virtual int64_t getIdentSize(OperationContext* opCtx, StringData ident) {
        return 0;
    }

    virtual Status repairIdent(OperationContext* opCtx, StringData ident) {
        return Status::OK();
    }

    virtual bool hasIdent(OperationContext* opCtx, StringData ident) const {
        return true;
    }

    std::vector<std::string> getAllIdents(OperationContext* opCtx) const {
        std::vector<std::string> idents;
        for (const auto& i : _idents) {
            idents.push_back(i.first);
        }
        return idents;
    }

    virtual void cleanShutdown(){};

    void setJournalListener(mongo::JournalListener* jl) final {}

    virtual Timestamp getAllDurableTimestamp() const override {
        RecordId id = _visibilityManager->getAllCommittedRecord();
        return Timestamp(id.repr());
    }

    virtual Timestamp getOldestOpenReadTimestamp() const override {
        return Timestamp();
    }

    boost::optional<Timestamp> getOplogNeededForCrashRecovery() const final {
        return boost::none;
    }

    // Ephemeral for test Specific

    /**
     * Returns a pair of the current version and a shared_ptr of tree of the master at the provided
     * timestamp. Null timestamps will return the latest master and timestamps before oldest
     * timestamp will throw SnapshotTooOld exception.
     */
    std::pair<uint64_t, std::shared_ptr<StringStore>> getMasterInfo(
        boost::optional<Timestamp> timestamp = boost::none);

    /**
     * Returns true and swaps _master to newMaster if the version passed in is the same as the
     * masters current version.
     */
    bool trySwapMaster(StringStore& newMaster, uint64_t version);

    VisibilityManager* visibilityManager() {
        return _visibilityManager.get();
    }

    /**
     * History in the map that is older than the oldest timestamp can be removed. Additionally, if
     * the tree at the oldest timestamp is no longer in use by any active transactions it can be
     * cleaned up, up until the point where there's an active transaction in the map. That point
     * also becomes the new oldest timestamp.
     */
    void cleanHistory();

    Timestamp getOldestTimestamp() const override;

    Timestamp getStableTimestamp() const override {
        return Timestamp();
    }

    void setOldestTimestamp(Timestamp newOldestTimestamp, bool force) override;

    std::map<Timestamp, std::shared_ptr<StringStore>> getHistory_forTest();

    static bool instanceExists();

private:
    void _cleanHistory(WithLock);

    Timestamp _getOldestTimestamp(WithLock) const {
        return _availableHistory.begin()->first;
    }

    std::shared_ptr<void> _catalogInfo;
    int _cachePressureForTest = 0;
    mutable Mutex _identsLock = MONGO_MAKE_LATCH("KVEngine::_identsLock");
    std::map<std::string, bool> _idents;  // TODO : replace with a query to _master.
    std::unique_ptr<VisibilityManager> _visibilityManager;

    mutable Mutex _masterLock = MONGO_MAKE_LATCH("KVEngine::_masterLock");
    std::shared_ptr<StringStore> _master;
    // While write transactions aren't implemented, we use the _masterVersion to generate mock
    // commit timestamps. We need to start at 1 to avoid the null timestamp.
    uint64_t _masterVersion = 1;

    // This map contains the different versions of the StringStore's referenced by their commit
    // timestamps.
    std::map<Timestamp, std::shared_ptr<StringStore>> _availableHistory;
};
}  // namespace ephemeral_for_test
}  // namespace mongo
