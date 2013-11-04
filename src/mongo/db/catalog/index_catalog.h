// index_catalog.h

/**
*    Copyright (C) 2013 10gen Inc.
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

#include <vector>

#include "mongo/db/diskloc.h"
#include "mongo/db/jsobj.h"

namespace mongo {

    class Collection;
    class NamespaceDetails;

    class IndexDescriptor;
    class IndexDetails;
    class IndexAccessMethod;
    class BtreeAccessMethod;
    class BtreeBasedAccessMethod;

    /**
     * how many: 1 per Collection
     * lifecycle: attached to a Collection
     */
    class IndexCatalog {
    public:
        IndexCatalog( Collection* collection, NamespaceDetails* details );
        ~IndexCatalog();

        // ---- accessors -----

        int numIndexesTotal() const;
        int numIndexesReady() const;
        int numIndexesInProgress() const { return numIndexesTotal() - numIndexesReady(); }

        /**
         * this is in "alive" until the Collection goes away
         * in which case everything from this tree has to go away
         */

        IndexDescriptor* findIdIndex();

        /**
         * @return null if cannot find
         */
        IndexDescriptor* findIndexByName( const StringData& name,
                                          bool includeUnfinishedIndexes = false );

        /**
         * @return null if cannot find
         */
        IndexDescriptor* findIndexByKeyPattern( const BSONObj& key,
                                                bool includeUnfinishedIndexes = false );

        /* Returns the index entry for the first index whose prefix contains
         * 'keyPattern'. If 'requireSingleKey' is true, skip indices that contain
         * array attributes. Otherwise, returns NULL.
         */
        IndexDescriptor* findIndexByPrefix( const BSONObj &keyPattern,
                                            bool requireSingleKey );


        // throws
        // never returns NULL
        IndexDescriptor* getDescriptor( int idxNo );

        // never returns NULL
        IndexAccessMethod* getIndex( IndexDescriptor* desc );

        BtreeBasedAccessMethod* getBtreeBasedIndex( IndexDescriptor* desc );

        IndexAccessMethod* getBtreeIndex( IndexDescriptor* desc );

        // TODO: add iterator, search methods

        // ---- index modifiers ------

        Status ensureHaveIdIndex();

        Status createIndex( BSONObj spec, bool mayInterrupt );

        Status okToAddIndex( const BSONObj& spec ) const;

        Status dropAllIndexes( bool includingIdIndex );

        Status dropIndex( IndexDescriptor* desc );
        Status dropIndex( int idxNo );

        /**
         * drops ALL uncompleted indexes
         * this is meant to only run at startup after a crash
         */
        Status blowAwayInProgressIndexEntries();

        /**
         * will drop an uncompleted index and return spec
         * @return the info for a single index to retry
         */
        BSONObj prepOneUnfinishedIndex();

        void markMultikey( IndexDescriptor* idx, bool isMultikey = true );

        // --- these probably become private?

        class IndexBuildBlock {
        public:
            IndexBuildBlock( IndexCatalog* catalog, const StringData& indexName, const DiskLoc& loc );
            ~IndexBuildBlock();

            IndexDetails* indexDetails() { return _indexDetails; }

            void success();

        private:
            IndexCatalog* _catalog;
            string _ns;
            string _indexName;

            NamespaceDetails* _nsd;
            IndexDetails* _indexDetails;
        };

        // ----- data modifiers ------

        // this throws for now
        void indexRecord( const BSONObj& obj, const DiskLoc &loc );

        void unindexRecord( const BSONObj& obj, const DiskLoc& loc, bool noWarn );

        /**
         * checks all unique indexes and checks for conflicts
         * should not throw
         */
        Status checkNoIndexConflicts( const BSONObj& obj );

        // ------- temp internal -------

        int _removeFromSystemIndexes( const StringData& indexName );

        string getAccessMethodName(const BSONObj& keyPattern) {
            return _getAccessMethodName( keyPattern );
        }

        // public static helpers

        static bool validKeyPattern( const BSONObj& obj );

        static BSONObj fixIndexSpec( const BSONObj& spec );

        static BSONObj fixIndexKey( const BSONObj& key );

    private:

        void _deleteCacheEntry( unsigned i );
        void _fixDescriptorCacheNumbers();

        Status _upgradeDatabaseMinorVersionIfNeeded( const string& newPluginName );

        /**
         * this is just an attempt to clean up old orphaned stuff on a delete all indexes
         * call. repair database is the clean solution, but this gives one a lighter weight
         * partial option.  see dropIndexes()
         * @param idIndex - can be NULL
         * @return how many things were deleted, should be 0
         */
        int _assureSysIndexesEmptied( IndexDetails* idIndex );


        bool _shouldOverridePlugin( const BSONObj& keyPattern );

        /**
         * This differs from IndexNames::findPluginName in that returns the plugin name we *should*
         * use, not the plugin name inside of the provided key pattern.  To understand when these
         * differ, see shouldOverridePlugin.
         */
        string _getAccessMethodName(const BSONObj& keyPattern);

        void _checkMagic() const;

        Status _indexRecord( int idxNo, const BSONObj& obj, const DiskLoc &loc );
        Status _unindexRecord( int idxNo, const BSONObj& obj, const DiskLoc &loc, bool logIfError );

        /**
         * this does no sanity checks
         */
        Status _dropIndex( int idxNo );

        int _magic;
        Collection* _collection;
        NamespaceDetails* _details;

        // these are caches, not source of truth
        // they should be treated as such

        std::vector<IndexDescriptor*> _descriptorCache;
        std::vector<IndexAccessMethod*> _accessMethodCache;
        std::vector<BtreeAccessMethod*> _forcedBtreeAccessMethodCache;

        static const BSONObj _idObj; // { _id : 1 }
    };

}
