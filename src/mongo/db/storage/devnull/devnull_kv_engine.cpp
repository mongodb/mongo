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

#include "mongo/platform/basic.h"

#include "mongo/db/storage/devnull/devnull_kv_engine.h"

#include "mongo/base/disallow_copying.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_record_store.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/stdx/memory.h"

namespace mongo {

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
    void reattachToOperationContext(OperationContext* txn) final {}
};

class DevNullRecordStore : public RecordStore {
public:
    DevNullRecordStore(StringData ns, const CollectionOptions& options)
        : RecordStore(ns), _options(options) {
        _numInserts = 0;
        _dummy = BSON("_id" << 1);
    }

    virtual const char* name() const {
        return "devnull";
    }

    virtual void setCappedCallback(CappedCallback*) {}

    virtual long long dataSize(OperationContext* txn) const {
        return 0;
    }

    virtual long long numRecords(OperationContext* txn) const {
        return 0;
    }

    virtual bool isCapped() const {
        return _options.capped;
    }

    virtual int64_t storageSize(OperationContext* txn,
                                BSONObjBuilder* extraInfo = NULL,
                                int infoLevel = 0) const {
        return 0;
    }

    virtual RecordData dataFor(OperationContext* txn, const RecordId& loc) const {
        return RecordData(_dummy.objdata(), _dummy.objsize());
    }

    virtual bool findRecord(OperationContext* txn, const RecordId& loc, RecordData* rd) const {
        return false;
    }

    virtual void deleteRecord(OperationContext* txn, const RecordId& dl) {}

    virtual StatusWith<RecordId> insertRecord(OperationContext* txn,
                                              const char* data,
                                              int len,
                                              bool enforceQuota) {
        _numInserts++;
        return StatusWith<RecordId>(RecordId(6, 4));
    }

    virtual Status insertRecordsWithDocWriter(OperationContext* txn,
                                              const DocWriter* const* docs,
                                              size_t nDocs,
                                              RecordId* idsOut) {
        _numInserts += nDocs;
        if (idsOut) {
            for (size_t i = 0; i < nDocs; i++) {
                idsOut[i] = RecordId(6, 4);
            }
        }
        return Status::OK();
    }

    virtual Status updateRecord(OperationContext* txn,
                                const RecordId& oldLocation,
                                const char* data,
                                int len,
                                bool enforceQuota,
                                UpdateNotifier* notifier) {
        return Status::OK();
    }

    virtual bool updateWithDamagesSupported() const {
        return false;
    }

    virtual StatusWith<RecordData> updateWithDamages(OperationContext* txn,
                                                     const RecordId& loc,
                                                     const RecordData& oldRec,
                                                     const char* damageSource,
                                                     const mutablebson::DamageVector& damages) {
        invariant(false);
    }


    std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* txn,
                                                    bool forward) const final {
        return stdx::make_unique<EmptyRecordCursor>();
    }

    virtual Status truncate(OperationContext* txn) {
        return Status::OK();
    }

    virtual void temp_cappedTruncateAfter(OperationContext* txn, RecordId end, bool inclusive) {}

    virtual Status validate(OperationContext* txn,
                            ValidateCmdLevel level,
                            ValidateAdaptor* adaptor,
                            ValidateResults* results,
                            BSONObjBuilder* output) {
        return Status::OK();
    }

    virtual void appendCustomStats(OperationContext* txn,
                                   BSONObjBuilder* result,
                                   double scale) const {
        result->appendNumber("numInserts", _numInserts);
    }

    virtual Status touch(OperationContext* txn, BSONObjBuilder* output) const {
        return Status::OK();
    }

    virtual void updateStatsAfterRepair(OperationContext* txn,
                                        long long numRecords,
                                        long long dataSize) {}

private:
    CollectionOptions _options;
    long long _numInserts;
    BSONObj _dummy;
};

class DevNullSortedDataBuilderInterface : public SortedDataBuilderInterface {
    MONGO_DISALLOW_COPYING(DevNullSortedDataBuilderInterface);

public:
    DevNullSortedDataBuilderInterface() {}

    virtual Status addKey(const BSONObj& key, const RecordId& loc) {
        return Status::OK();
    }
};

class DevNullSortedDataInterface : public SortedDataInterface {
public:
    virtual ~DevNullSortedDataInterface() {}

    virtual SortedDataBuilderInterface* getBulkBuilder(OperationContext* txn, bool dupsAllowed) {
        return new DevNullSortedDataBuilderInterface();
    }

    virtual Status insert(OperationContext* txn,
                          const BSONObj& key,
                          const RecordId& loc,
                          bool dupsAllowed) {
        return Status::OK();
    }

    virtual void unindex(OperationContext* txn,
                         const BSONObj& key,
                         const RecordId& loc,
                         bool dupsAllowed) {}

    virtual Status dupKeyCheck(OperationContext* txn, const BSONObj& key, const RecordId& loc) {
        return Status::OK();
    }

    virtual void fullValidate(OperationContext* txn,
                              long long* numKeysOut,
                              ValidateResults* fullResults) const {}

    virtual bool appendCustomStats(OperationContext* txn,
                                   BSONObjBuilder* output,
                                   double scale) const {
        return false;
    }

    virtual long long getSpaceUsedBytes(OperationContext* txn) const {
        return 0;
    }

    virtual bool isEmpty(OperationContext* txn) {
        return true;
    }

    virtual std::unique_ptr<SortedDataInterface::Cursor> newCursor(OperationContext* txn,
                                                                   bool isForward) const {
        return {};
    }

    virtual Status initAsEmpty(OperationContext* txn) {
        return Status::OK();
    }
};


RecordStore* DevNullKVEngine::getRecordStore(OperationContext* opCtx,
                                             StringData ns,
                                             StringData ident,
                                             const CollectionOptions& options) {
    if (ident == "_mdb_catalog") {
        return new EphemeralForTestRecordStore(ns, &_catalogInfo);
    }
    return new DevNullRecordStore(ns, options);
}

SortedDataInterface* DevNullKVEngine::getSortedDataInterface(OperationContext* opCtx,
                                                             StringData ident,
                                                             const IndexDescriptor* desc) {
    return new DevNullSortedDataInterface();
}
}
