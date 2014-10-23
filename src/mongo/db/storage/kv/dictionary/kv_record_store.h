// kv_record_store.h

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

#include "mongo/db/storage/capped_callback.h"
#include "mongo/db/storage/kv/dictionary/kv_dictionary.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    class CollectionOptions;

    class KVRecordStore : public RecordStore {
    public:
        /**
         * Construct a new KVRecordStore. Ownership of `db' is passed to
         * this object.
         *
         * @param db, the KVDictionary interface that will be used to
         *        store records.
         * @param opCtx, the current operation context.
         * @param ns, the namespace the underlying RecordStore is
         *        constructed with
         * @param options, options for the storage engine, if any are
         *        applicable to the implementation.
         */
        KVRecordStore( KVDictionary *db,
                       OperationContext* opCtx,
                       const StringData& ns,
                       const CollectionOptions& options );

        virtual ~KVRecordStore() { }

        /**
         * Name of the RecordStore implementation.
         */
        virtual const char* name() const { return _db->name(); }

        /**
         * Total size of each record id key plus the records stored.
         *
         * TODO: Does this have to be exact? Sometimes it doesn't, sometimes
         *       it cannot be without major performance issues.
         */
        virtual long long dataSize( OperationContext* txn ) const;

        /**
         * TODO: Does this have to be exact? Sometimes it doesn't, sometimes
         *       it cannot be without major performance issues.
         */
        virtual long long numRecords( OperationContext* txn ) const;

        /**
         * How much space is used on disk by this record store.
         */
        virtual int64_t storageSize( OperationContext* txn,
                                     BSONObjBuilder* extraInfo = NULL,
                                     int infoLevel = 0 ) const;

        // CRUD related

        virtual RecordData dataFor( OperationContext* txn, const DiskLoc& loc ) const;

        virtual bool findRecord( OperationContext* txn,
                                 const DiskLoc& loc,
                                 RecordData* out ) const;

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
                                          const char* damageSource,
                                          const mutablebson::DamageVector& damages );

        virtual RecordIterator* getIterator( OperationContext* txn,
                                             const DiskLoc& start = DiskLoc(),
                                             const CollectionScanParams::Direction& dir =
                                             CollectionScanParams::FORWARD ) const;

        virtual RecordIterator* getIteratorForRepair( OperationContext* txn ) const;

        virtual std::vector<RecordIterator *> getManyIterators( OperationContext* txn ) const;

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

        // KVRecordStore is not capped, KVRecordStoreCapped is capped.

        virtual bool isCapped() const { return false; }

        virtual void temp_cappedTruncateAfter(OperationContext* txn,
                                              DiskLoc end,
                                              bool inclusive) {
            invariant(false);
        }

        void setCappedDeleteCallback(CappedDocumentDeleteCallback* cb) {
            invariant(false);
        }

        bool cappedMaxDocs() const { invariant(false); }

        bool cappedMaxSize() const { invariant(false); }

        /**
         * Used for keeping record stats in a separate dictionary.
         */
        void setStatsMetadataDictionary(OperationContext *opCtx, KVDictionary *metadataDict);

        static void deleteMetadataKeys(OperationContext *opCtx, KVDictionary *metadataDict, const StringData &ident);

    protected:
        class KVRecordIterator : public RecordIterator {
            KVDictionary *_db;
            const CollectionScanParams::Direction _dir;
            DiskLoc _savedLoc;
            Slice _savedVal;

            // May change due to saveState() / restoreState()
            OperationContext *_txn;

            boost::scoped_ptr<KVDictionary::Cursor> _cursor;

            void _setCursor(const DiskLoc loc);

            void _saveLocAndVal();

        public: 
            KVRecordIterator(KVDictionary *db, OperationContext *txn,
                             const DiskLoc &start,
                             const CollectionScanParams::Direction &dir);

            bool isEOF();

            DiskLoc curr();

            DiskLoc getNext();

            void invalidate(const DiskLoc& loc);

            void saveState();

            bool restoreState(OperationContext* txn);

            RecordData dataFor(const DiskLoc& loc) const;
        };

        static std::string numRecordsMetadataKey(const StringData &ident) {
            static const std::string suffix = "-nr";
            return str::stream() << ident << suffix;
        }

        static std::string dataSizeMetadataKey(const StringData &ident) {
            static const std::string suffix = "-ds";
            return str::stream() << ident << suffix;
        }

        void _initializeStatsForKey(OperationContext *opCtx, const std::string &key);
        int64_t _getStats(OperationContext *opCtx, const std::string &key) const;
        void _updateStats(OperationContext *opCtx, int64_t numRecordsDelta, int64_t dataSizeDelta);

        // Internal version of dataFor that takes a KVDictionary - used by
        // the RecordIterator to implement dataFor.
        static RecordData _getDataFor(const KVDictionary* db, OperationContext* txn, const DiskLoc& loc);

        // Generate the next unique DiskLoc key value for new records stored by this record store.
        DiskLoc _nextId();

        // An owned KVDictionary interface used to store records.
        // The key is a modified version of DiskLoc (see RecordIdKey) and
        // the value is the raw record data as provided by insertRecord etc.
        boost::scoped_ptr<KVDictionary> _db;

        // A thread-safe 64 bit integer for generating new unique DiskLoc keys.
        AtomicUInt64 _nextIdNum;

        // Pointer to metadata dictionary used for persistent stats.  This is owned by the KVEngine.
        KVDictionary *_metadataDict;

        // metadata keys, cached
        const std::string _numRecordsMetadataKey;
        const std::string _dataSizeMetadataKey;
    };

} // namespace mongo
