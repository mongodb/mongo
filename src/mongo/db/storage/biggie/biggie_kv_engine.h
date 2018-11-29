
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

#include "mongo/db/storage/biggie/biggie_record_store.h"
#include "mongo/db/storage/biggie/biggie_sorted_impl.h"
#include "mongo/db/storage/biggie/store.h"
#include "mongo/db/storage/kv/kv_engine.h"

namespace mongo {
namespace biggie {

class JournalListener;
/**
 * The biggie storage engine is intended for unit and performance testing.
 */
class KVEngine : public mongo::KVEngine {
public:
    KVEngine() : mongo::KVEngine() {}

    virtual ~KVEngine() {}

    virtual mongo::RecoveryUnit* newRecoveryUnit();

    virtual Status createRecordStore(OperationContext* opCtx,
                                     StringData ns,
                                     StringData ident,
                                     const CollectionOptions& options);

    virtual std::unique_ptr<mongo::RecordStore> getRecordStore(OperationContext* opCtx,
                                                               StringData ns,
                                                               StringData ident,
                                                               const CollectionOptions& options);

    virtual std::unique_ptr<mongo::RecordStore> makeTemporaryRecordStore(OperationContext* opCtx,
                                                                         StringData ident) override;

    virtual Status createSortedDataInterface(OperationContext* opCtx,
                                             StringData ident,
                                             const IndexDescriptor* desc);

    virtual mongo::SortedDataInterface* getSortedDataInterface(OperationContext* opCtx,
                                                               StringData ident,
                                                               const IndexDescriptor* desc);

    virtual Status beginBackup(OperationContext* opCtx) {
        return Status::OK();
    }

    virtual void endBackup(OperationContext* opCtx) {}

    virtual Status dropIdent(OperationContext* opCtx, StringData ident);

    virtual bool supportsDocLocking() const {
        return true;
    }

    virtual bool supportsDirectoryPerDB() const {
        return false;  // Not persistant so no Directories
    }

    virtual bool supportsCappedCollections() const {
        return true;
    }

    /**
     * Biggie does not write to disk.
     */
    virtual bool isDurable() const {
        return false;
    }

    virtual bool isEphemeral() const {
        return true;
    }

    virtual bool isCacheUnderPressure(OperationContext* opCtx) const override {
        return false;
    }

    virtual void setCachePressureForTest(int pressure) override;

    // only called by KVDatabaseCatalogEntryBase::sizeOnDisk so return 0
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

    virtual Timestamp getAllCommittedTimestamp() const override {
        return Timestamp();
    }

    virtual Timestamp getOldestOpenReadTimestamp() const override {
        return Timestamp();
    }

    // Biggie Specific

    /**
     * Returns a pair of the current version and copy of tree of the master.
     */
    std::pair<uint64_t, StringStore> getMasterInfo() {
        stdx::lock_guard<stdx::mutex> lock(_masterLock);
        return std::make_pair(_masterVersion, _master);
    }

    /**
     * Returns true and swaps _master to newMaster if the version passed in is the same as the
     * masters current version.
     */
    bool trySwapMaster(StringStore& newMaster, uint64_t version);

private:
    std::shared_ptr<void> _catalogInfo;
    int _cachePressureForTest = 0;
    std::map<std::string, bool> _idents;  // TODO : replace with a query to _master.

    mutable stdx::mutex _masterLock;
    StringStore _master;
    uint64_t _masterVersion = 0;
};
}  // namespace biggie
}  // namespace mongo
