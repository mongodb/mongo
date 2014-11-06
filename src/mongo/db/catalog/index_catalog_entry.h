// index_catalog_entry.h

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

#include <string>

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/diskloc.h"

namespace mongo {

    class CollectionCatalogEntry;
    class CollectionInfoCache;
    class HeadManager;
    class IndexAccessMethod;
    class IndexDescriptor;
    class OperationContext;

    class IndexCatalogEntry {
        MONGO_DISALLOW_COPYING( IndexCatalogEntry );
    public:
        IndexCatalogEntry( const StringData& ns,
                           CollectionCatalogEntry* collection, // not owned
                           IndexDescriptor* descriptor, // ownership passes to me
                           CollectionInfoCache* infoCache ); // not owned, optional

        ~IndexCatalogEntry();

        const string& ns() const { return _ns; }

        void init( OperationContext* txn,
                   IndexAccessMethod* accessMethod );

        IndexDescriptor* descriptor() { return _descriptor; }
        const IndexDescriptor* descriptor() const { return _descriptor; }

        IndexAccessMethod* accessMethod() { return _accessMethod; }
        const IndexAccessMethod* accessMethod() const { return _accessMethod; }

        const Ordering& ordering() const { return _ordering; }

        /// ---------------------

        const DiskLoc& head( OperationContext* txn ) const;

        void setHead( OperationContext* txn, DiskLoc newHead );

        void setIsReady( bool newIsReady );

        HeadManager* headManager() const { return _headManager; }

        // --

        bool isMultikey( OperationContext* txn ) const;

        void setMultikey( OperationContext* txn );

        // if this ready is ready for queries
        bool isReady( OperationContext* txn ) const;

        bool wantToSetIsMultikey() const { return _wantToSetIsMultikey; }

    private:

        class SetMultikeyChange;

        bool _catalogIsReady( OperationContext* txn ) const;
        DiskLoc _catalogHead( OperationContext* txn ) const;
        bool _catalogIsMultikey( OperationContext* txn ) const;

        // -----

        string _ns;

        CollectionCatalogEntry* _collection; // not owned here

        IndexDescriptor* _descriptor; // owned here

        CollectionInfoCache* _infoCache; // not owned here

        IndexAccessMethod* _accessMethod; // owned here

        // Owned here.
        HeadManager* _headManager;

        // cached stuff

        Ordering _ordering; // TODO: this might be b-tree specific
        bool _isReady; // cache of NamespaceDetails info
        DiskLoc _head; // cache of IndexDetails
        bool _isMultikey; // cache of NamespaceDetails info

        bool _wantToSetIsMultikey; // see ::setMultikey
    };

    class IndexCatalogEntryContainer {
    public:

        typedef std::vector<IndexCatalogEntry*>::const_iterator const_iterator;
        typedef std::vector<IndexCatalogEntry*>::const_iterator iterator;

        const_iterator begin() const { return _entries.vector().begin(); }
        const_iterator end() const { return _entries.vector().end(); }

        iterator begin() { return _entries.vector().begin(); }
        iterator end() { return _entries.vector().end(); }

        // TODO: these have to be SUPER SUPER FAST
        // maybe even some pointer trickery is in order
        const IndexCatalogEntry* find( const IndexDescriptor* desc ) const;
        IndexCatalogEntry* find( const IndexDescriptor* desc );

        IndexCatalogEntry* find( const std::string& name );


        unsigned size() const { return _entries.size(); }
        // -----------------

        /**
         * Removes from _entries and returns the matching entry or NULL if none matches.
         */
        IndexCatalogEntry* release( const IndexDescriptor* desc );

        bool remove( const IndexDescriptor* desc ) {
            IndexCatalogEntry* entry = release(desc);
            delete entry;
            return entry;
        }

        // pass ownership to EntryContainer
        void add( IndexCatalogEntry* entry ) { _entries.mutableVector().push_back( entry ); }

    private:
        OwnedPointerVector<IndexCatalogEntry> _entries;
    };

}
