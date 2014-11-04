// wiredtiger_record_store.h

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
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

#include <string>

#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/capped_callback.h"
#include "mongo/platform/atomic_word.h"

namespace mongo {

    class RecoveryUnit;
    class WiredTigerCursor;
    class WiredTigerRecoveryUnit;
    class WiredTigerSizeStorer;

    class WiredTigerRecordStore : public RecordStore {
    public:

        /**
         * Creates a configuration string suitable for 'config' parameter in WT_SESSION::create().
         * Configuration string is constructed from:
         *     built-in defaults
         *     storageEngine.wiredtiger.configString in 'options'
         *     'extraStrings'
         * Performs simple validation on the supplied parameters.
         * Returns error status if validation fails.
         * Note that even if this function returns an OK status, WT_SESSION:create() may still
         * fail with the constructed configuration string.
         */
        static StatusWith<std::string> generateCreateString(const StringData& ns,
                                                            const CollectionOptions &options,
                                                            const StringData& extraStrings);

        WiredTigerRecordStore(OperationContext* txn,
                              const StringData& ns,
                              const StringData& uri,
                              bool isCapped = false,
                              int64_t cappedMaxSize = -1,
                              int64_t cappedMaxDocs = -1,
                              CappedDocumentDeleteCallback* cappedDeleteCallback = NULL,
                              WiredTigerSizeStorer* sizeStorer = NULL );

        virtual ~WiredTigerRecordStore();

        // name of the RecordStore implementation
        virtual const char* name() const { return "wiredtiger"; }

        virtual long long dataSize( OperationContext *txn ) const;

        virtual long long numRecords( OperationContext* txn ) const;

        virtual bool isCapped() const;

        virtual int64_t storageSize( OperationContext* txn,
                                     BSONObjBuilder* extraInfo = NULL,
                                     int infoLevel = 0 ) const;

        // CRUD related

        virtual RecordData dataFor( OperationContext* txn, const DiskLoc& loc ) const;

        virtual bool findRecord( OperationContext* txn, const DiskLoc& loc, RecordData* out ) const;

        virtual void deleteRecord( OperationContext* txn, const DiskLoc& dl );

        virtual StatusWith<DiskLoc> insertRecord( OperationContext* txn,
                                                  const char* data,
                                                  int len,
                                                  bool enforceQuota );

        virtual StatusWith<DiskLoc> insertRecord( OperationContext* txn,
                                                  const DocWriter* doc,
                                                  bool enforceQuota );

        virtual StatusWith<DiskLoc> updateRecord( OperationContext* txn,
                                                  const DiskLoc& oldLocation,
                                                  const char* data,
                                                  int len,
                                                  bool enforceQuota,
                                                  UpdateMoveNotifier* notifier );

        virtual Status updateWithDamages( OperationContext* txn,
                                          const DiskLoc& loc,
                                          const RecordData& oldRec,
                                          const char* damangeSource,
                                          const mutablebson::DamageVector& damages );

        virtual RecordIterator* getIterator( OperationContext* txn,
                                             const DiskLoc& start = DiskLoc(),
                                             const CollectionScanParams::Direction& dir =
                                             CollectionScanParams::FORWARD ) const;

        virtual RecordIterator* getIteratorForRepair( OperationContext* txn ) const;

        virtual std::vector<RecordIterator*> getManyIterators( OperationContext* txn ) const;

        virtual Status truncate( OperationContext* txn );

        virtual bool compactSupported() const { return true; }

        virtual Status compact( OperationContext* txn,
                                RecordStoreCompactAdaptor* adaptor,
                                const CompactOptions* options,
                                CompactStats* stats );

        virtual Status validate( OperationContext* txn,
                                 bool full, bool scanData,
                                 ValidateAdaptor* adaptor,
                                 ValidateResults* results, BSONObjBuilder* output ) const;

        virtual void appendCustomStats( OperationContext* txn,
                                        BSONObjBuilder* result,
                                        double scale ) const;

        virtual Status touch( OperationContext* txn, BSONObjBuilder* output ) const;

        virtual Status setCustomOption( OperationContext* txn,
                                        const BSONElement& option,
                                        BSONObjBuilder* info = NULL );

        virtual void temp_cappedTruncateAfter(OperationContext* txn,
                                              DiskLoc end,
                                              bool inclusive);

        virtual DiskLoc oplogStartHack(OperationContext* txn,
                                       const DiskLoc& startingPosition) const;

        void setCappedDeleteCallback(CappedDocumentDeleteCallback* cb) {
            _cappedDeleteCallback = cb;
        }
        int64_t cappedMaxDocs() const;
        int64_t cappedMaxSize() const;

        const std::string& GetURI() const { return _uri; }
        uint64_t instanceId() const { return _instanceId; }

        void setSizeStorer( WiredTigerSizeStorer* ss ) { _sizeStorer = ss; }

    private:

        class Iterator : public RecordIterator {
        public:
            Iterator( const WiredTigerRecordStore& rs,
                      OperationContext* txn,
                      const DiskLoc& start,
                      const CollectionScanParams::Direction& dir,
                      bool forParallelCollectionScan );

            virtual ~Iterator();

            virtual bool isEOF();
            virtual DiskLoc curr();
            virtual DiskLoc getNext();
            virtual void invalidate(const DiskLoc& dl);
            virtual void saveState();
            virtual bool restoreState(OperationContext *txn);
            virtual RecordData dataFor( const DiskLoc& loc ) const;

        private:
            bool _forward() const;
            void _getNext();
            void _locate( const DiskLoc &loc, bool exact );
            void _checkStatus();
            DiskLoc _curr() const; // const version of public curr method

            const WiredTigerRecordStore& _rs;
            OperationContext* _txn;
            RecoveryUnit* _savedRecoveryUnit; // only used to sanity check between save/restore
            CollectionScanParams::Direction _dir;
            bool _forParallelCollectionScan;
            scoped_ptr<WiredTigerCursor> _cursor;
            bool _eof;

            DiskLoc _lastLoc; // the last thing returned from getNext()
        };

        class NumRecordsChange;
        class DataSizeChange;

        static WiredTigerRecoveryUnit* _getRecoveryUnit( OperationContext* txn );

        static uint64_t _makeKey(const DiskLoc &loc);
        static DiskLoc _fromKey(uint64_t k);

        DiskLoc _nextId();
        void _setId(DiskLoc loc);
        bool cappedAndNeedDelete(OperationContext* txn) const;
        void cappedDeleteAsNeeded(OperationContext* txn);
        void _changeNumRecords(OperationContext* txn, bool insert);
        void _increaseDataSize(OperationContext* txn, int amount);
        RecordData _getData( const WiredTigerCursor& cursor) const;
        StatusWith<DiskLoc> extractAndCheckLocForOplog(const char* data, int len);

        const std::string _uri;
        const uint64_t _instanceId; // not persisted

        // The capped settings should not be updated once operations have started
        const bool _isCapped;
        const bool _isOplog;
        const int64_t _cappedMaxSize;
        const int64_t _cappedMaxDocs;
        CappedDocumentDeleteCallback* _cappedDeleteCallback;

        const bool _useOplogHack;
        DiskLoc _highestLocForOplogHack;

        AtomicUInt64 _nextIdNum;
        AtomicInt64 _dataSize;
        AtomicInt64 _numRecords;

        WiredTigerSizeStorer* _sizeStorer; // not owned, can be NULL
        int _sizeStorerCounter;
    };
}
