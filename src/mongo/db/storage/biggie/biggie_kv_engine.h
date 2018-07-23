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

#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/db/storage/biggie/biggie_record_store.h"
#include "mongo/db/storage/biggie/biggie_sorted_impl.h"

namespace mongo {

class JournalListener;

/**
 * The biggie storage engine is intended for unit and performance testing.
 */
class BiggieKVEngine : public KVEngine {
    std::shared_ptr<BiggieStore> _store; //ALL our data
public:

    BiggieKVEngine() : KVEngine() {}
   
    virtual ~BiggieKVEngine() {}

    virtual RecoveryUnit* newRecoveryUnit() {
        return new RecoveryUnitNoop();
    }

    virtual Status createRecordStore(OperationContext* opCtx,
                                     StringData ns,
                                     StringData ident,
                                     const CollectionOptions& options) {
        // TODO: implement
        return Status::OK();
    }

    virtual std::unique_ptr<RecordStore> getRecordStore(OperationContext* opCtx,
                                                        StringData ns,
                                                        StringData ident,
                                                        const CollectionOptions& options) {
        // The engine passes it a new RecordStore based on the single instance
        // of the underlying data structure
        // why is this a unique pointer
        return std::make_unique<BiggieRecordStore>(ns, _store); // TODO deal with ident, options
    }

    virtual Status createSortedDataInterface(OperationContext* opCtx,
                                             StringData ident,
                                             const IndexDescriptor* desc) {
        return Status::OK();
    }

    virtual SortedDataInterface* getSortedDataInterface(OperationContext* opCtx,
                                                        StringData ident,
                                                        const IndexDescriptor* desc) {
        return new BiggieSortedImpl(); // TODO : implement later                                                            
    }

    virtual Status dropIdent(OperationContext* opCtx, StringData ident) {
        return Status::OK();
    }

    virtual bool supportsDocLocking() const {
        return true;
    }

    virtual bool supportsDirectoryPerDB() const {
        return false;
    }

    virtual bool supportsCappedCollections() const {
        return false;
    }

    /**
     * biggie does no journaling, so don't report the engine as durable.
     */
    virtual bool isDurable() const {
        return false;
    }

    virtual bool isEphemeral() const {
        return true;
    }

    virtual bool isCacheUnderPressure(OperationContext* opCtx) const override;

    virtual void setCachePressureForTest(int pressure) override;

    virtual int64_t getIdentSize(OperationContext* opCtx, StringData ident) {
        return 1;
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

    void setJournalListener(JournalListener* jl) final {}

    virtual Timestamp getAllCommittedTimestamp() const override {
        return Timestamp();
    }

private:
    std::shared_ptr<void> _catalogInfo;

    int _cachePressureForTest;

    BSONObj _dummy;
};
}
