// collection.h

/**
*    Copyright (C) 2012 10gen Inc.
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
#include "mongo/db/catalog/collection_cursor_cache.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/structure/record_store.h"
#include "mongo/db/catalog/collection_info_cache.h"
#include "mongo/platform/cstdint.h"

namespace mongo {

    class Database;
    class ExtentManager;
    class NamespaceDetails;
    class IndexCatalog;
    class MultiIndexBlock;

    class CollectionIterator;
    class FlatIterator;
    class CappedIterator;

    class OpDebug;

    class DocWriter {
    public:
        virtual ~DocWriter() {}
        virtual void writeDocument( char* buf ) const = 0;
        virtual size_t documentSize() const = 0;
        virtual bool addPadding() const { return true; }
    };

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
    class Collection {
    public:
        Collection( const StringData& fullNS,
                        NamespaceDetails* details,
                        Database* database );

        ~Collection();

        bool ok() const { return _magic == 1357924; }

        NamespaceDetails* details() { return _details; } // TODO: remove
        const NamespaceDetails* details() const { return _details; }

        CollectionInfoCache* infoCache() { return &_infoCache; }
        const CollectionInfoCache* infoCache() const { return &_infoCache; }

        const NamespaceString& ns() const { return _ns; }

        const IndexCatalog* getIndexCatalog() const { return &_indexCatalog; }
        IndexCatalog* getIndexCatalog() { return &_indexCatalog; }

        const RecordStore* getRecordStore() const { return _recordStore.get(); }

        CollectionCursorCache* cursorCache() const { return &_cursorCache; }

        bool requiresIdIndex() const;

        BSONObj docFor( const DiskLoc& loc );

        // ---- things that should move to a CollectionAccessMethod like thing
        /**
         * canonical to get all would be
         * getIterator( DiskLoc(), false, CollectionScanParams::FORWARD )
         */
        CollectionIterator* getIterator( const DiskLoc& start, bool tailable,
                                         const CollectionScanParams::Direction& dir) const;


        /**
         * does a table scan to do a count
         * this should only be used at a very low level
         * does no yielding, indexes, etc...
         */
        int64_t countTableScan( const MatchExpression* expression );

        void deleteDocument( const DiskLoc& loc,
                             bool cappedOK = false,
                             bool noWarn = false,
                             BSONObj* deletedId = 0 );

        /**
         * this does NOT modify the doc before inserting
         * i.e. will not add an _id field for documents that are missing it
         */
        StatusWith<DiskLoc> insertDocument( const BSONObj& doc, bool enforceQuota );

        StatusWith<DiskLoc> insertDocument( const DocWriter* doc, bool enforceQuota );

        StatusWith<DiskLoc> insertDocument( const BSONObj& doc, MultiIndexBlock& indexBlock );

        /**
         * updates the document @ oldLocation with newDoc
         * if the document fits in the old space, it is put there
         * if not, it is moved
         * @return the post update location of the doc (may or may not be the same as oldLocation)
         */
        StatusWith<DiskLoc> updateDocument( const DiskLoc& oldLocation,
                                            const BSONObj& newDoc,
                                            bool enforceQuota,
                                            OpDebug* debug );

        int64_t storageSize( int* numExtents = NULL, BSONArrayBuilder* extentInfo = NULL ) const;

        // -----------

        StatusWith<CompactStats> compact( const CompactOptions* options );

        /**
         * removes all documents as fast as possible
         * indexes before and after will be the same
         * as will other characteristics
         */
        Status truncate();
        // -----------


        // this is temporary, moving up from DB for now
        // this will add a new extent the collection
        // the new extent will be returned
        // it will have been added to the linked list already
        void increaseStorageSize( int size, bool enforceQuota );

        //
        // Stats
        //

        bool isCapped() const;

        uint64_t numRecords() const;

        uint64_t dataSize() const;

        int averageObjectSize() const {
            uint64_t n = numRecords();
            if ( n == 0 )
                return 5;
            return static_cast<int>( dataSize() / n );
        }

    private:
        /**
         * same semantics as insertDocument, but doesn't do:
         *  - some user error checks
         *  - adjust padding
         */
        StatusWith<DiskLoc> _insertDocument( const BSONObj& doc,
                                             bool enforceQuota );

        void _compactExtent(const DiskLoc diskloc, int extentNumber,
                            MultiIndexBlock& indexesToInsertTo,
                            const CompactOptions* compactOptions, CompactStats* stats );

        // @return 0 for inf., otherwise a number of files
        int largestFileNumberInQuota() const;

        ExtentManager* getExtentManager();
        const ExtentManager* getExtentManager() const;

        int _magic;

        NamespaceString _ns;
        NamespaceDetails* _details;
        Database* _database;
        scoped_ptr<RecordStore> _recordStore;
        CollectionInfoCache _infoCache;
        IndexCatalog _indexCatalog;

        // this is mutable because read only users of the Collection class
        // use it keep state.  This seems valid as const correctness of Collection
        // should be about the data.
        mutable CollectionCursorCache _cursorCache;

        friend class Database;
        friend class FlatIterator;
        friend class CappedIterator;
        friend class IndexCatalog;
        friend class NamespaceDetails;
    };

}
