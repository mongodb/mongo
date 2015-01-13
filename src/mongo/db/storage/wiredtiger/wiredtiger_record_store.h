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

#include <set>
#include <string>

#include <boost/scoped_ptr.hpp>
#include <boost/thread/mutex.hpp>

#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/capped_callback.h"
#include "mongo/platform/atomic_word.h"

namespace mongo {

    class RecoveryUnit;
    class WiredTigerCursor;
    class WiredTigerRecoveryUnit;
    class WiredTigerSizeStorer;

    extern const std::string kWiredTigerEngineName;

    class WiredTigerRecordStore : public RecordStore {
    public:

        /**
         * During record store creation, if size storer reports a record count under
         * 'kCollectionScanOnCreationThreshold', perform a collection scan to update size storer
         * as well as internal record and data size counters.
         */
        static const long long kCollectionScanOnCreationThreshold;

        /**
         * Creates a configuration string suitable for 'config' parameter in WT_SESSION::create().
         * Configuration string is constructed from:
         *     built-in defaults
         *     storageEngine.wiredTiger.configString in 'options'
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
        virtual const char* name() const;

        virtual long long dataSize( OperationContext *txn ) const;

        virtual long long numRecords( OperationContext* txn ) const;

        virtual bool isCapped() const;

        virtual int64_t storageSize( OperationContext* txn,
                                     BSONObjBuilder* extraInfo = NULL,
                                     int infoLevel = 0 ) const;

        // CRUD related

        virtual RecordData dataFor( OperationContext* txn, const RecordId& loc ) const;

        virtual bool findRecord( OperationContext* txn, const RecordId& loc, RecordData* out ) const;

        virtual void deleteRecord( OperationContext* txn, const RecordId& dl );

        virtual StatusWith<RecordId> insertRecord( OperationContext* txn,
                                                  const char* data,
                                                  int len,
                                                  bool enforceQuota );

        virtual StatusWith<RecordId> insertRecord( OperationContext* txn,
                                                  const DocWriter* doc,
                                                  bool enforceQuota );

        virtual StatusWith<RecordId> updateRecord( OperationContext* txn,
                                                  const RecordId& oldLocation,
                                                  const char* data,
                                                  int len,
                                                  bool enforceQuota,
                                                  UpdateMoveNotifier* notifier );

        virtual bool updateWithDamagesSupported() const;

        virtual Status updateWithDamages( OperationContext* txn,
                                          const RecordId& loc,
                                          const RecordData& oldRec,
                                          const char* damageSource,
                                          const mutablebson::DamageVector& damages );

        virtual RecordIterator* getIterator( OperationContext* txn,
                                             const RecordId& start = RecordId(),
                                             const CollectionScanParams::Direction& dir =
                                             CollectionScanParams::FORWARD ) const;

        virtual std::vector<RecordIterator*> getManyIterators( OperationContext* txn ) const;

        virtual Status truncate( OperationContext* txn );

        virtual bool compactSupported() const { return true; }

        virtual Status compact( OperationContext* txn,
                                RecordStoreCompactAdaptor* adaptor,
                                const CompactOptions* options,
                                CompactStats* stats );

        virtual Status validate( OperationContext* txn,
                                 bool full,
                                 bool scanData,
                                 ValidateAdaptor* adaptor,
                                 ValidateResults* results,
                                 BSONObjBuilder* output );

        virtual void appendCustomStats( OperationContext* txn,
                                        BSONObjBuilder* result,
                                        double scale ) const;

        virtual Status setCustomOption( OperationContext* txn,
                                        const BSONElement& option,
                                        BSONObjBuilder* info = NULL );

        virtual void temp_cappedTruncateAfter(OperationContext* txn,
                                              RecordId end,
                                              bool inclusive);

        virtual boost::optional<RecordId> oplogStartHack(OperationContext* txn,
                                                         const RecordId& startingPosition) const;

        virtual Status oplogDiskLocRegister( OperationContext* txn,
                                             const OpTime& opTime );

        virtual void updateStatsAfterRepair(OperationContext* txn,
                                            long long numRecords,
                                            long long dataSize);

        bool isOplog() const { return _isOplog; }
        bool usingOplogHack() const { return _useOplogHack; }

        void setCappedDeleteCallback(CappedDocumentDeleteCallback* cb) {
            _cappedDeleteCallback = cb;
        }
        int64_t cappedMaxDocs() const;
        int64_t cappedMaxSize() const;

        const std::string& getURI() const { return _uri; }
        uint64_t instanceId() const { return _instanceId; }

        void setSizeStorer( WiredTigerSizeStorer* ss ) { _sizeStorer = ss; }

        void dealtWithCappedLoc( const RecordId& loc );
        bool isCappedHidden( const RecordId& loc ) const;

    private:

        class Iterator : public RecordIterator {
        public:
            Iterator( const WiredTigerRecordStore& rs,
                      OperationContext* txn,
                      const RecordId& start,
                      const CollectionScanParams::Direction& dir,
                      bool forParallelCollectionScan );

            virtual ~Iterator();

            virtual bool isEOF();
            virtual RecordId curr();
            virtual RecordId getNext();
            virtual void invalidate(const RecordId& dl);
            virtual void saveState();
            virtual bool restoreState(OperationContext *txn);
            virtual RecordData dataFor( const RecordId& loc ) const;

        private:
            void _getNext();
            void _locate( const RecordId &loc, bool exact );
            RecordId _curr() const; // const version of public curr method

            const WiredTigerRecordStore& _rs;
            OperationContext* _txn;
            RecoveryUnit* _savedRecoveryUnit; // only used to sanity check between save/restore
            const bool _forward;
            bool _forParallelCollectionScan;
            boost::scoped_ptr<WiredTigerCursor> _cursor;
            bool _eof;
            const RecordId _readUntilForOplog;

            RecordId _loc; // Cached key of _cursor. Update any time _cursor is moved.
            RecordId _lastLoc; // the last thing returned from getNext()
        };

        class CappedInsertChange;
        class NumRecordsChange;
        class DataSizeChange;

        static WiredTigerRecoveryUnit* _getRecoveryUnit( OperationContext* txn );

        static int64_t _makeKey(const RecordId &loc);
        static RecordId _fromKey(int64_t k);

        void _addUncommitedDiskLoc_inlock( OperationContext* txn, const RecordId& loc );

        RecordId _nextId();
        void _setId(RecordId loc);
        bool cappedAndNeedDelete() const;
        void cappedDeleteAsNeeded(OperationContext* txn, const RecordId& justInserted );
        void _changeNumRecords(OperationContext* txn, int64_t diff);
        void _increaseDataSize(OperationContext* txn, int amount);
        RecordData _getData( const WiredTigerCursor& cursor) const;
        StatusWith<RecordId> extractAndCheckLocForOplog(const char* data, int len);
        void _oplogSetStartHack( WiredTigerRecoveryUnit* wru ) const;

        const std::string _uri;
        const uint64_t _instanceId; // not persisted

        // The capped settings should not be updated once operations have started
        const bool _isCapped;
        const bool _isOplog;
        const int64_t _cappedMaxSize;
        const int64_t _cappedMaxDocs;
        CappedDocumentDeleteCallback* _cappedDeleteCallback;
        int _cappedDeleteCheckCount; // see comment in ::cappedDeleteAsNeeded
        boost::mutex _cappedDeleterMutex; // see comment in ::cappedDeleteAsNeeded

        const bool _useOplogHack;

        typedef std::vector<RecordId> SortedDiskLocs;
        SortedDiskLocs _uncommittedDiskLocs;
        RecordId _oplog_visibleTo;
        RecordId _oplog_highestSeen;
        mutable boost::mutex _uncommittedDiskLocsMutex;

        AtomicInt64 _nextIdNum;
        AtomicInt64 _dataSize;
        AtomicInt64 _numRecords;

        WiredTigerSizeStorer* _sizeStorer; // not owned, can be NULL
        int _sizeStorerCounter;
    };
}
