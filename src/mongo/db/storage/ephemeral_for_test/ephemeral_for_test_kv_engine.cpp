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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_kv_engine.h"

#include <memory>

#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_recovery_unit.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/sorted_data_interface.h"

namespace mongo {
namespace ephemeral_for_test {
namespace {
static AtomicWord<bool> shuttingDown{false};
}  // namespace

bool KVEngine::instanceExists() {
    return shuttingDown.load();
}

KVEngine::KVEngine()
    : mongo::KVEngine(), _visibilityManager(std::make_unique<VisibilityManager>()) {
    _master = std::make_shared<StringStore>();
    _availableHistory[Timestamp(_masterVersion++, 0)] = _master;
    shuttingDown.store(false);
}

KVEngine::~KVEngine() {
    shuttingDown.store(true);
}

mongo::RecoveryUnit* KVEngine::newRecoveryUnit() {
    return new RecoveryUnit(this, nullptr);
}

Status KVEngine::createRecordStore(OperationContext* opCtx,
                                   StringData ns,
                                   StringData ident,
                                   const CollectionOptions& options) {
    stdx::lock_guard lock(_identsLock);
    _idents[ident.toString()] = true;
    return Status::OK();
}

Status KVEngine::importRecordStore(OperationContext* opCtx,
                                   StringData ident,
                                   const BSONObj& storageMetadata) {
    stdx::lock_guard lock(_identsLock);
    _idents[ident.toString()] = true;
    return Status::OK();
}

std::unique_ptr<mongo::RecordStore> KVEngine::makeTemporaryRecordStore(OperationContext* opCtx,
                                                                       StringData ident) {
    std::unique_ptr<mongo::RecordStore> recordStore =
        std::make_unique<RecordStore>("", ident, false);
    stdx::lock_guard lock(_identsLock);
    _idents[ident.toString()] = true;
    return recordStore;
};


std::unique_ptr<mongo::RecordStore> KVEngine::getRecordStore(OperationContext* unused,
                                                             StringData ns,
                                                             StringData ident,
                                                             const CollectionOptions& options) {
    std::unique_ptr<mongo::RecordStore> recordStore;
    if (options.capped) {
        recordStore = std::make_unique<RecordStore>(
            ns,
            ident,
            options.capped,
            options.cappedSize ? options.cappedSize : kDefaultCappedSizeBytes,
            options.cappedMaxDocs ? options.cappedMaxDocs : -1,
            /*cappedCallback*/ nullptr,
            _visibilityManager.get());
    } else {
        recordStore = std::make_unique<RecordStore>(ns, ident, options.capped);
    }
    stdx::lock_guard lock(_identsLock);
    _idents[ident.toString()] = true;
    return recordStore;
}

bool KVEngine::trySwapMaster(StringStore& newMaster, uint64_t version) {
    stdx::lock_guard<Latch> lock(_masterLock);
    invariant(!newMaster.hasBranch() && !_master->hasBranch());
    if (_masterVersion != version)
        return false;
    // TODO SERVER-48314: replace _masterVersion with a Timestamp of transaction.
    Timestamp commitTimestamp(_masterVersion++, 0);
    auto newMasterPtr = std::make_shared<StringStore>(newMaster);
    _availableHistory[commitTimestamp] = newMasterPtr;
    _master = newMasterPtr;
    _cleanHistory(lock);
    return true;
}


Status KVEngine::createSortedDataInterface(OperationContext* opCtx,
                                           const CollectionOptions& collOptions,
                                           StringData ident,
                                           const IndexDescriptor* desc) {
    stdx::lock_guard lock(_identsLock);
    _idents[ident.toString()] = false;
    return Status::OK();  // I don't think we actually need to do anything here
}

Status KVEngine::importSortedDataInterface(OperationContext* opCtx,
                                           StringData ident,
                                           const BSONObj& storageMetadata) {
    stdx::lock_guard lock(_identsLock);
    _idents[ident.toString()] = false;
    return Status::OK();
}

std::unique_ptr<mongo::SortedDataInterface> KVEngine::getSortedDataInterface(
    OperationContext* opCtx, StringData ident, const IndexDescriptor* desc) {
    {
        stdx::lock_guard lock(_identsLock);
        _idents[ident.toString()] = false;
    }
    if (desc->unique())
        return std::make_unique<SortedDataInterfaceUnique>(opCtx, ident, desc);
    else
        return std::make_unique<SortedDataInterfaceStandard>(opCtx, ident, desc);
}

Status KVEngine::dropIdent(mongo::RecoveryUnit* ru,
                           StringData ident,
                           StorageEngine::DropIdentCallback&& onDrop) {
    Status dropStatus = Status::OK();
    stdx::unique_lock lock(_identsLock);
    if (_idents.count(ident.toString()) > 0) {
        // Check if the ident is a RecordStore or a SortedDataInterface then call the corresponding
        // truncate. A true value in the map means it is a RecordStore, false a SortedDataInterface.
        bool isRecordStore = _idents[ident.toString()] == true;
        lock.unlock();
        if (isRecordStore) {  // ident is RecordStore.
            CollectionOptions s;
            auto rs = getRecordStore(/*opCtx=*/nullptr, ""_sd, ident, s);
            dropStatus =
                checked_cast<RecordStore*>(rs.get())->truncateWithoutUpdatingCount(ru).getStatus();
        } else {  // ident is SortedDataInterface.
            auto sdi =
                std::make_unique<SortedDataInterfaceUnique>(Ordering::make(BSONObj()), ident);
            dropStatus = sdi->truncate(ru);
        }
        lock.lock();
        _idents.erase(ident.toString());
    }
    if (dropStatus.isOK() && onDrop) {
        onDrop();
    }
    return dropStatus;
}

std::pair<uint64_t, std::shared_ptr<StringStore>> KVEngine::getMasterInfo(
    boost::optional<Timestamp> timestamp) {
    stdx::lock_guard<Latch> lock(_masterLock);
    if (timestamp && !timestamp->isNull()) {
        if (timestamp < _getOldestTimestamp(lock)) {
            uasserted(ErrorCodes::SnapshotTooOld,
                      str::stream() << "Read timestamp " << timestamp->toString()
                                    << " is older than the oldest available timestamp.");
        }
        auto it = _availableHistory.lower_bound(timestamp.get());
        return std::make_pair(it->first.asULL(), it->second);
    }
    return std::make_pair(_masterVersion, _master);
}

void KVEngine::cleanHistory() {
    stdx::lock_guard<Latch> lock(_masterLock);
    _cleanHistory(lock);
}

void KVEngine::_cleanHistory(WithLock) {
    for (auto it = _availableHistory.cbegin(); it != _availableHistory.cend();) {
        if (it->second.use_count() == 1) {
            invariant(it->second.get() != _master.get());
            it = _availableHistory.erase(it);
        } else {
            break;
        }
    }

    // Check that pointer to master is not deleted.
    invariant(_availableHistory.size() >= 1);
}

Timestamp KVEngine::getOldestTimestamp() const {
    stdx::lock_guard<Latch> lock(_masterLock);
    return _getOldestTimestamp(lock);
}

void KVEngine::setOldestTimestamp(Timestamp newOldestTimestamp, bool force) {
    stdx::lock_guard<Latch> lock(_masterLock);
    if (newOldestTimestamp > _availableHistory.rbegin()->first) {
        _availableHistory[newOldestTimestamp] = _master;
        // TODO SERVER-48314: Remove when _masterVersion is no longer being used to mock commit
        // timestamps.
        _masterVersion = newOldestTimestamp.asULL();
    }
    for (auto it = _availableHistory.cbegin(); it != _availableHistory.cend();) {
        if (it->first < newOldestTimestamp) {
            it = _availableHistory.erase(it);
        } else {
            break;
        }
    }

    // Check that pointer to master is not deleted.
    invariant(_availableHistory.size() >= 1);
}

std::map<Timestamp, std::shared_ptr<StringStore>> KVEngine::getHistory_forTest() {
    stdx::lock_guard<Latch> lock(_masterLock);
    return _availableHistory;
}

class EmptyRecordCursor final : public SeekableRecordCursor {
public:
    boost::optional<Record> next() final {
        return {};
    }
    boost::optional<Record> seekExact(const RecordId& id) final {
        return {};
    }
    void save() final {}
    bool restore() final {
        return true;
    }
    void detachFromOperationContext() final {}
    void reattachToOperationContext(OperationContext* opCtx) final {}
};
}  // namespace ephemeral_for_test
}  // namespace mongo
