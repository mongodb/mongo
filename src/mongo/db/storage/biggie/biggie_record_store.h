// biggie_record_store.h

/**
*    Copyright (C) 2018 MongoDB Inc.
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

#include <boost/shared_array.hpp>
#include <map>

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/storage/biggie/biggie_store.h"
#include "mongo/db/storage/capped_callback.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

/**
 * A RecordStore that stores all data in-memory.
 *
 * @param cappedMaxSize - required if isCapped. limit uses dataSize() in this impl.
 */
class BiggieRecordStore : public RecordStore {
    std::shared_ptr<BiggieStore> _data;
    const bool _isCapped;
    const int64_t _cappedMaxSize;
    const int64_t _cappedMaxDocs;
    CappedCallback* _cappedCallback;
    


public:
    explicit BiggieRecordStore(StringData ns,
                                         std::shared_ptr<BiggieStore> data,
                                         bool isCapped = false,
                                         int64_t cappedMaxSize = -1,
                                         int64_t cappedMaxDocs = -1,
                                         CappedCallback* cappedCallback = nullptr);

    virtual const char* name() const;

    virtual long long dataSize(OperationContext* opCtx) const; 
    virtual long long numRecords(OperationContext* opCtx) const;
    virtual bool isCapped() const;
    virtual int64_t storageSize(OperationContext* opCtx,
                                BSONObjBuilder* extraInfo = NULL,
                                int infoLevel = 0) const;
    
    virtual RecordData dataFor(OperationContext* opCtx, const RecordId& loc) const;

    virtual bool findRecord(OperationContext* opCtx, const RecordId& loc, RecordData* rd) const;

    virtual void deleteRecord(OperationContext* opCtx, const RecordId& dl);

    virtual StatusWith<RecordId> insertRecord(
        OperationContext* opCtx, const char* data, int len, Timestamp, bool enforceQuota);
    

    virtual Status insertRecordsWithDocWriter(OperationContext* opCtx,
                                              const DocWriter* const* docs,
                                              const Timestamp*,
                                              size_t nDocs,
                                              RecordId* idsOut);

    virtual Status updateRecord(OperationContext* opCtx,
                                const RecordId& oldLocation,
                                const char* data,
                                int len,
                                bool enforceQuota,
                                UpdateNotifier* notifier);

    virtual bool updateWithDamagesSupported() const;

    virtual StatusWith<RecordData> updateWithDamages(OperationContext* opCtx,
                                                     const RecordId& loc,
                                                     const RecordData& oldRec,
                                                     const char* damageSource,
                                                     const mutablebson::DamageVector& damages);

    std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx,
                                                    bool forward) const final;

    virtual Status truncate(OperationContext* opCtx);

    virtual void cappedTruncateAfter(OperationContext* opCtx, RecordId end, bool inclusive);

    virtual Status validate(OperationContext* opCtx,
                            ValidateCmdLevel level,
                            ValidateAdaptor* adaptor,
                            ValidateResults* results,
                            BSONObjBuilder* output);

    virtual void appendCustomStats(OperationContext* opCtx,
                                   BSONObjBuilder* result,
                                   double scale) const;

    virtual Status touch(OperationContext* opCtx, BSONObjBuilder* output) const;

    // Use default implementation which returns boost::none
    // virtual boost::optional<RecordId> oplogStartHack(OperationContext* opCtx, const RecordId& startingPosition) const;

    // virtual void increaseStorageSize(OperationContext* opCtx, int size, bool enforceQuota);



    void waitForAllEarlierOplogWritesToBeVisible(OperationContext* opCtx) const;

    virtual void updateStatsAfterRepair(OperationContext* opCtx,
                                        long long numRecords,
                                        long long dataSize);

protected:
    uint64_t nextRecordId = 0; //TODO: make atomic for thread safety
    // struct BiggieRecord {
    //     BiggieRecord() : size(0) {}
    //     BiggieRecord(int size) : size(size), data(new char[size]) {}

    //     RecordData toRecordData() const {
    //         return RecordData(data.get(), size);
    //     }

    //     int size;
    //     boost::shared_array<char> data;
    // };

    // virtual const BiggieRecord* recordFor(const RecordId& loc) const;
    // virtual BiggieRecord* recordFor(const RecordId& loc);

// public:
//     //
//     // Not in RecordStore interface
//     //

//     typedef std::map<RecordId, BiggieRecord> Records;

//     bool isCapped() const {
//         return _isCapped;
//     }
//     void setCappedCallback(CappedCallback* cb) {
//         _cappedCallback = cb;
//     }

private:
    // TODO : needs to be changed
    BSONObj _dummy;
//     class InsertChange;
//     class RemoveChange;
//     class TruncateChange;

    class Cursor final : public SeekableRecordCursor {
    public:
        Cursor(OperationContext* opCtx, const BiggieRecordStore& rs);
        boost::optional<Record> next() final;
        boost::optional<Record> seekExact(const RecordId& id) final override;
        void save() final;
        void saveUnpositioned() final override;
        bool restore() final;
        void detachFromOperationContext() final;
        void reattachToOperationContext(OperationContext* opCtx) final;
    };
//     class ReverseCursor;

//     StatusWith<RecordId> extractAndCheckLocForOplog(const char* data, int len) const;

//     RecordId allocateLoc();
//     bool cappedAndNeedDelete_inlock(OperationContext* opCtx) const;
//     void cappedDeleteAsNeeded_inlock(OperationContext* opCtx);
//     void deleteRecord_inlock(OperationContext* opCtx, const RecordId& dl);

//     // TODO figure out a proper solution to metadata


//     // This is the "persistent" data.
//     struct Data {
//         Data(StringData ns, bool isOplog)
//             : dataSize(0), recordsMutex(), nextId(1), isOplog(isOplog) {}

//         int64_t dataSize;
//         stdx::recursive_mutex recordsMutex;
//         Records records;
//         int64_t nextId;
//         const bool isOplog;
//     };

//     Data* const _data;
};

}  // namespace mongo
