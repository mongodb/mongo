/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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
class KVEngine : public ::mongo::KVEngine {
    std::shared_ptr<StringStore> _master = std::make_shared<StringStore>();
    std::set<StringData> _idents;  // TODO : replace with a query to _master.
    mutable stdx::mutex _masterLock;

public:
    KVEngine() : ::mongo::KVEngine() {}

    virtual ~KVEngine() {}

    virtual mongo::RecoveryUnit* newRecoveryUnit();

    virtual Status createRecordStore(OperationContext* opCtx,
                                     StringData ns,
                                     StringData ident,
                                     const CollectionOptions& options);

    virtual std::unique_ptr<::mongo::RecordStore> getRecordStore(OperationContext* opCtx,
                                                                 StringData ns,
                                                                 StringData ident,
                                                                 const CollectionOptions& options);

    virtual Status createSortedDataInterface(OperationContext* opCtx,
                                             StringData ident,
                                             const IndexDescriptor* desc);

    virtual mongo::SortedDataInterface* getSortedDataInterface(OperationContext* opCtx,
                                                               StringData ident,
                                                               const IndexDescriptor* desc);

    virtual Status dropIdent(OperationContext* opCtx, StringData ident) {
        return Status::OK();
    }

    virtual bool supportsDocLocking() const {
        return false;  // TODO : do this later.
    }

    virtual bool supportsDirectoryPerDB() const {
        return false;  // TODO : do this later.
    }

    virtual bool supportsCappedCollections() const {
        return false;  // TODO : do this later.
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

    virtual int64_t getIdentSize(OperationContext* opCtx, StringData ident) {
        return 1;  // TODO : implement.
    }

    virtual Status repairIdent(OperationContext* opCtx, StringData ident) {
        return Status::OK();
    }

    virtual bool hasIdent(OperationContext* opCtx, StringData ident) const {
        return true;
    }

    std::vector<std::string> getAllIdents(OperationContext* opCtx) const {
        return std::vector<std::string>();
    }

    virtual void cleanShutdown(){};

    void setJournalListener(mongo::JournalListener* jl) final {}

    virtual Timestamp getAllCommittedTimestamp() const override {
        return Timestamp();
    }

    // Biggie Specific

    /**
     * Used to replace the master branch of the store with an updated copy.
     * Appropriate lock must be taken externally.
     */
    // TODO: should possibly check store version numbers before setting.
    void setMaster_inlock(std::unique_ptr<StringStore> newMaster);

    std::shared_ptr<StringStore> getMaster() const;
    std::shared_ptr<StringStore> getMaster_inlock() const;
    /**
     * Get the lock around the master branch.
     */
    stdx::mutex& getMasterLock() {
        return _masterLock;
    }

private:
    std::shared_ptr<void> _catalogInfo;
    int _cachePressureForTest;
};
}  // namespace biggie
}  // namespace mongo
