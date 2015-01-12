// collection.h

/**
*    Copyright (C) 2012-2014 MongoDB Inc.
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

#include <string>

#include "mongo/base/string_data.h"
#include "mongo/bson/mutable/damage_vector.h"
#include "mongo/db/catalog/collection_info_cache.h"
#include "mongo/db/catalog/cursor_manager.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/capped_callback.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/platform/cstdint.h"

namespace mongo {

    class CollectionCatalogEntry;
    class Database;
    class ExtentManager;
    class IndexCatalog;
    class MultiIndexBlock;
    class OperationContext;

    class RecordIterator;
    class RecordFetcher;

    class OpDebug;

    struct CompactOptions {

        CompactOptions() {
            paddingMode = NONE;
            validateDocuments = true;
            paddingFactor = 1;
            paddingBytes = 0;
        }

        // padding
        enum PaddingMode {
            PRESERVE, NONE, MANUAL
        } paddingMode;

        // only used if _paddingMode == MANUAL
        double paddingFactor; // what to multiple document size by
        int paddingBytes; // what to add to ducment size after multiplication
        unsigned computeRecordSize( unsigned recordSize ) const {
            recordSize = static_cast<unsigned>( paddingFactor * recordSize );
            recordSize += paddingBytes;
            return recordSize;
        }

        // other
        bool validateDocuments;

        std::string toString() const;
    };

    struct CompactStats {
        CompactStats() {
            corruptDocuments = 0;
        }

        long long corruptDocuments;
    };

    /**
     * this is NOT safe through a yield right now
     * not sure if it will be, or what yet
     */
    class Collection : CappedDocumentDeleteCallback, UpdateMoveNotifier {
    public:
        Collection( OperationContext* txn,
                    const StringData& fullNS,
                    CollectionCatalogEntry* details, // does not own
                    RecordStore* recordStore, // does not own
                    Database* database ); // does not own

        ~Collection();

        bool ok() const { return _magic == 1357924; }

        CollectionCatalogEntry* getCatalogEntry() { return _details; }
        const CollectionCatalogEntry* getCatalogEntry() const { return _details; }

        CollectionInfoCache* infoCache() { return &_infoCache; }
        const CollectionInfoCache* infoCache() const { return &_infoCache; }

        const NamespaceString& ns() const { return _ns; }

        const IndexCatalog* getIndexCatalog() const { return &_indexCatalog; }
        IndexCatalog* getIndexCatalog() { return &_indexCatalog; }

        const RecordStore* getRecordStore() const { return _recordStore; }
        RecordStore* getRecordStore() { return _recordStore; }

        CursorManager* getCursorManager() const { return &_cursorManager; }

        bool requiresIdIndex() const;

        BSONObj docFor(OperationContext* txn, const RecordId& loc) const;

        /**
         * @param out - contents set to the right docs if exists, or nothing.
         * @return true iff loc exists
         */
        bool findDoc(OperationContext* txn, const RecordId& loc, BSONObj* out) const;

        // ---- things that should move to a CollectionAccessMethod like thing
        /**
         * Default arguments will return all items in the collection.
         */
        RecordIterator* getIterator( OperationContext* txn,
                                     const RecordId& start = RecordId(),
                                     const CollectionScanParams::Direction& dir = CollectionScanParams::FORWARD ) const;

        /**
         * Returns many iterators that partition the Collection into many disjoint sets. Iterating
         * all returned iterators is equivalent to Iterating the full collection.
         * Caller owns all pointers in the vector.
         */
        std::vector<RecordIterator*> getManyIterators( OperationContext* txn ) const;


        /**
         * does a table scan to do a count
         * this should only be used at a very low level
         * does no yielding, indexes, etc...
         */
        int64_t countTableScan( OperationContext* txn, const MatchExpression* expression );

        void deleteDocument( OperationContext* txn,
                             const RecordId& loc,
                             bool cappedOK = false,
                             bool noWarn = false,
                             BSONObj* deletedId = 0 );

        /**
         * this does NOT modify the doc before inserting
         * i.e. will not add an _id field for documents that are missing it
         *
         * If enforceQuota is false, quotas will be ignored.
         */
        StatusWith<RecordId> insertDocument( OperationContext* txn,
                                            const BSONObj& doc,
                                            bool enforceQuota );

        StatusWith<RecordId> insertDocument( OperationContext* txn,
                                            const DocWriter* doc,
                                            bool enforceQuota );

        StatusWith<RecordId> insertDocument( OperationContext* txn,
                                            const BSONObj& doc,
                                            MultiIndexBlock* indexBlock,
                                            bool enforceQuota );

        /**
         * If the document at 'loc' is unlikely to be in physical memory, the storage
         * engine gives us back a RecordFetcher functor which we can invoke in order
         * to page fault on that record.
         *
         * Returns NULL if the document does not need to be fetched.
         *
         * Caller takes ownership of the returned RecordFetcher*.
         */
        RecordFetcher* documentNeedsFetch( OperationContext* txn,
                                           const RecordId& loc ) const;

        /**
         * updates the document @ oldLocation with newDoc
         * if the document fits in the old space, it is put there
         * if not, it is moved
         * @return the post update location of the doc (may or may not be the same as oldLocation)
         */
        StatusWith<RecordId> updateDocument( OperationContext* txn,
                                             const RecordId& oldLocation,
                                             const BSONObj& oldDoc,
                                             const BSONObj& newDoc,
                                             bool enforceQuota,
                                             bool indexesAffected,
                                             OpDebug* debug );

        /**
         * right now not allowed to modify indexes
         */
        Status updateDocumentWithDamages( OperationContext* txn,
                                          const RecordId& loc,
                                          const RecordData& oldRec,
                                          const char* damageSource,
                                          const mutablebson::DamageVector& damages );

        // -----------

        StatusWith<CompactStats> compact(OperationContext* txn, const CompactOptions* options);

        /**
         * removes all documents as fast as possible
         * indexes before and after will be the same
         * as will other characteristics
         */
        Status truncate(OperationContext* txn);

        /**
         * @param full - does more checks
         * @param scanData - scans each document
         * @return OK if the validate run successfully
         *         OK will be returned even if corruption is found
         *         deatils will be in result
         */
        Status validate( OperationContext* txn,
                         bool full, bool scanData,
                         ValidateResults* results, BSONObjBuilder* output );

        /**
         * forces data into cache
         */
        Status touch( OperationContext* txn,
                      bool touchData, bool touchIndexes,
                      BSONObjBuilder* output ) const;

        /**
         * Truncate documents newer than the document at 'end' from the capped
         * collection.  The collection cannot be completely emptied using this
         * function.  An assertion will be thrown if that is attempted.
         * @param inclusive - Truncate 'end' as well iff true
         * XXX: this will go away soon, just needed to move for now
         */
        void temp_cappedTruncateAfter( OperationContext* txn, RecordId end, bool inclusive );

        // -----------

        //
        // Stats
        //

        bool isCapped() const;

        uint64_t numRecords( OperationContext* txn ) const;

        uint64_t dataSize( OperationContext* txn ) const;

        int averageObjectSize( OperationContext* txn ) const {
            uint64_t n = numRecords( txn );
            if ( n == 0 )
                return 5;
            return static_cast<int>( dataSize( txn ) / n );
        }

        uint64_t getIndexSize(OperationContext* opCtx,
                              BSONObjBuilder* details = NULL,
                              int scale = 1);

        // --- end suspect things

    private:

        Status recordStoreGoingToMove( OperationContext* txn,
                                       const RecordId& oldLocation,
                                       const char* oldBuffer,
                                       size_t oldSize );

        Status aboutToDeleteCapped( OperationContext* txn, const RecordId& loc );

        /**
         * same semantics as insertDocument, but doesn't do:
         *  - some user error checks
         *  - adjust padding
         */
        StatusWith<RecordId> _insertDocument( OperationContext* txn,
                                             const BSONObj& doc,
                                             bool enforceQuota );

        bool _enforceQuota( bool userEnforeQuota ) const;

        int _magic;

        NamespaceString _ns;
        CollectionCatalogEntry* _details;
        RecordStore* _recordStore;
        Database* _database;
        CollectionInfoCache _infoCache;
        IndexCatalog _indexCatalog;

        // this is mutable because read only users of the Collection class
        // use it keep state.  This seems valid as const correctness of Collection
        // should be about the data.
        mutable CursorManager _cursorManager;

        friend class Database;
        friend class IndexCatalog;
        friend class NamespaceDetails;
    };

}
