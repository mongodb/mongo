// index_catalog_internal.h

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

namespace mongo {

    class IndexDescriptor;
    class BtreeInMemoryState;
    class IndexAccessMethod;

    class IndexCatalogEntry {
    public:
        IndexCatalogEntry( IndexDescriptor* descriptor,
                           BtreeInMemoryState* state,
                           IndexAccessMethod* accessMethod );

        ~IndexCatalogEntry();

        IndexDescriptor* descriptor() { return _descriptor; }
        BtreeInMemoryState* state() { return _state; }
        IndexAccessMethod* accessMethod() { return _accessMethod; }

        const IndexDescriptor* descriptor() const { return _descriptor; }
        const BtreeInMemoryState* state() const { return _state; }
        const IndexAccessMethod* accessMethod() const { return _accessMethod; }

        IndexAccessMethod* forcedBtreeIndex() { return _forcedBtreeIndex; }
        void setForcedBtreeIndex( IndexAccessMethod* iam ) { _forcedBtreeIndex = iam; }

    private:
        IndexDescriptor* _descriptor; // owned here
        BtreeInMemoryState* _state; // owned here
        IndexAccessMethod* _accessMethod; // owned here
        IndexAccessMethod* _forcedBtreeIndex; // owned here
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

        bool remove( const IndexDescriptor* desc );

        // pass ownership to EntryContainer
        void add( IndexCatalogEntry* entry ) { _entries.mutableVector().push_back( entry ); }

        // TODO: should the findIndexBy* methods be done here
        // and proxied in IndexCatatalog
        //IndexCatalogEntry* findIndexByName();

    private:
        OwnedPointerVector<IndexCatalogEntry> _entries;
    };

}
