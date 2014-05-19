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

#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/platform/unordered_map.h"

namespace mongo {

    class Client;
    class Collection;

    class IndexDescriptor;
    class IndexAccessMethod;

    /**
     * how many: 1 per Collection
     * lifecycle: attached to a Collection
     */
    class IndexCatalog {
    public:
        IndexCatalog( Collection* collection );
        ~IndexCatalog();

        // must be called before used
        Status init(OperationContext* txn);

        bool ok() const;

        // ---- accessors -----

        int numIndexesTotal() const;
        int numIndexesReady() const;
        int numIndexesInProgress() const { return numIndexesTotal() - numIndexesReady(); }

        /**
         * this is in "alive" until the Collection goes away
         * in which case everything from this tree has to go away
         */

        bool haveIdIndex() const;

        IndexDescriptor* findIdIndex() const;

        /**
         * @return null if cannot find
         */
        IndexDescriptor* findIndexByName( const StringData& name,
                                          bool includeUnfinishedIndexes = false ) const;

        /**
         * @return null if cannot find
         */
        IndexDescriptor* findIndexByKeyPattern( const BSONObj& key,
                                                bool includeUnfinishedIndexes = false ) const;

        /* Returns the index entry for the first index whose prefix contains
         * 'keyPattern'. If 'requireSingleKey' is true, skip indices that contain
         * array attributes. Otherwise, returns NULL.
         */
        IndexDescriptor* findIndexByPrefix( const BSONObj &keyPattern,
                                            bool requireSingleKey ) const;

        void findIndexByType( const std::string& type,
                              std::vector<IndexDescriptor*>& matches,
                              bool includeUnfinishedIndexes = false ) const;

        // never returns NULL
        const IndexCatalogEntry* getEntry( const IndexDescriptor* desc ) const;

        IndexAccessMethod* getIndex( const IndexDescriptor* desc );
        const IndexAccessMethod* getIndex( const IndexDescriptor* desc ) const;

        class IndexIterator {
        public:
            bool more();
            IndexDescriptor* next();

            // returns the access method for the last return IndexDescriptor
            IndexAccessMethod* accessMethod( IndexDescriptor* desc );
        private:
            IndexIterator( const IndexCatalog* cat, bool includeUnfinishedIndexes );

            void _advance();

            bool _includeUnfinishedIndexes;
            const IndexCatalog* _catalog;
            IndexCatalogEntryContainer::const_iterator _iterator;

            bool _start; // only true before we've called next() or more()

            IndexCatalogEntry* _prev;
            IndexCatalogEntry* _next;

            friend class IndexCatalog;
        };

        IndexIterator getIndexIterator( bool includeUnfinishedIndexes ) const {
            return IndexIterator( this, includeUnfinishedIndexes );
        };

        // ---- index set modifiers ------

        Status ensureHaveIdIndex(OperationContext* txn);

        enum ShutdownBehavior {
            SHUTDOWN_CLEANUP, // fully clean up this build
            SHUTDOWN_LEAVE_DIRTY // leave as if kill -9 happened, so have to deal with on restart
        };

        Status createIndex( OperationContext* txn,
                            BSONObj spec,
                            bool mayInterrupt,
                            ShutdownBehavior shutdownBehavior = SHUTDOWN_CLEANUP );

        StatusWith<BSONObj> prepareSpecForCreate( const BSONObj& original ) const;

        Status dropAllIndexes(OperationContext* txn,
                              bool includingIdIndex );

        Status dropIndex(OperationContext* txn,
                         IndexDescriptor* desc );

        /**
         * will drop all incompleted indexes and return specs
         * after this, the indexes can be rebuilt
         */
        std::vector<BSONObj> getAndClearUnfinishedIndexes(OperationContext* txn);


        struct IndexKillCriteria {
            std::string ns;
            std::string name;
            BSONObj key;
        };

        /**
         * Given some criteria, will search through all in-progress index builds
         * and will kill ones that match. (namespace, index name, and/or index key spec)
         * Returns the list of index specs that were killed, for use in restarting them later.
         */
        std::vector<BSONObj> killMatchingIndexBuilds(const IndexKillCriteria& criteria);

        // ---- modify single index

        bool isMultikey( const IndexDescriptor* idex );

        // --- these probably become private?


        /**
         * disk creation order
         * 1) system.indexes entry
         * 2) collection's NamespaceDetails
         *    a) info + head
         *    b) _indexBuildsInProgress++
         * 3) indexes entry in .ns file
         * 4) system.namespaces entry for index ns
         */
        class IndexBuildBlock {
        public:
            IndexBuildBlock(OperationContext* txn,
                            Collection* collection,
                            const BSONObj& spec );

            ~IndexBuildBlock();

            Status init();

            void success();

            /**
             * index build failed, clean up meta data
             */
            void fail();

            /**
             * we're stopping the build
             * do NOT cleanup, leave meta data as is
             */
            void abort();

            IndexCatalogEntry* getEntry() { return _entry; }

        private:

            Collection* _collection;
            IndexCatalog* _catalog;
            std::string _ns;

            BSONObj _spec;

            std::string _indexName;
            std::string _indexNamespace;

            IndexCatalogEntry* _entry;
            bool _inProgress;

            OperationContext* _txn;
        };

        // ----- data modifiers ------

        // this throws for now
        void indexRecord(OperationContext* txn, const BSONObj& obj, const DiskLoc &loc);

        void unindexRecord(OperationContext* txn,
                           const BSONObj& obj,
                           const DiskLoc& loc,
                           bool noWarn);

        /**
         * checks all unique indexes and checks for conflicts
         * should not throw
         */
        Status checkNoIndexConflicts( const BSONObj& obj );

        // ------- temp internal -------

        std::string getAccessMethodName(const BSONObj& keyPattern) {
            return _getAccessMethodName( keyPattern );
        }

        Status _upgradeDatabaseMinorVersionIfNeeded( OperationContext* txn,
                                                     const std::string& newPluginName );

        // public static helpers

        static BSONObj fixIndexKey( const BSONObj& key );

    private:
        typedef unordered_map<IndexDescriptor*, Client*> InProgressIndexesMap;

        bool _shouldOverridePlugin( const BSONObj& keyPattern ) const;

        /**
         * This differs from IndexNames::findPluginName in that returns the plugin name we *should*
         * use, not the plugin name inside of the provided key pattern.  To understand when these
         * differ, see shouldOverridePlugin.
         */
        std::string _getAccessMethodName(const BSONObj& keyPattern) const;

        void _checkMagic() const;


        // checks if there is anything in _leftOverIndexes
        // meaning we shouldn't modify catalog
        Status _checkUnfinished() const;

        Status _indexRecord(OperationContext* txn,
                            IndexCatalogEntry* index,
                            const BSONObj& obj,
                            const DiskLoc &loc );

        Status _unindexRecord(OperationContext* txn,
                              IndexCatalogEntry* index,
                              const BSONObj& obj,
                              const DiskLoc &loc,
                              bool logIfError);

        /**
         * this does no sanity checks
         */
        Status _dropIndex(OperationContext* txn,
                          IndexCatalogEntry* entry );

        // just does disk hanges
        // doesn't change memory state, etc...
        void _deleteIndexFromDisk( OperationContext* txn,
                                   const std::string& indexName,
                                   const std::string& indexNamespace );

        // descriptor ownership passes to _setupInMemoryStructures
        IndexCatalogEntry* _setupInMemoryStructures(OperationContext* txn,
                                                    IndexDescriptor* descriptor );

        static BSONObj _fixIndexSpec( const BSONObj& spec );

        Status _isSpecOk( const BSONObj& spec ) const;

        Status _doesSpecConflictWithExisting( const BSONObj& spec ) const;

        int _magic;
        Collection* _collection;

        IndexCatalogEntryContainer _entries;

        // These are the index specs of indexes that were "leftover"
        // "Leftover" means they were unfinished when a mongod shut down
        // Certain operations are prohibted until someone fixes
        // get by calling getAndClearUnfinishedIndexes
        std::vector<BSONObj> _unfinishedIndexes;

        static const BSONObj _idObj; // { _id : 1 }

        // Track in-progress index builds, in order to find and stop them when necessary.
        InProgressIndexesMap _inProgressIndexes;
    };

}
