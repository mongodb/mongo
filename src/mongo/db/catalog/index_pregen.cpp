// index_pregen.cpp

/**
*    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_pregen.h"
#include "mongo/db/d_concurrency.h"
#include "mongo/db/index/index_access_method.h"

namespace mongo {

    const PregeneratedKeysOnIndex* PregeneratedKeys::get( IndexCatalogEntry* entry ) const {
        Map::const_iterator i = _indexes.find( entry );
        if ( i == _indexes.end() )
            return NULL;
        return &i->second;
    }

    void PregeneratedKeys::gen( const BSONObj& obj,
                                IndexCatalogEntry* entry,
                                const boost::shared_ptr<KeyGenerator>& generator ) {
        PregeneratedKeysOnIndex& onIndex = _indexes[entry];
        onIndex.generator = generator;
        generator->getKeys( obj, &onIndex.keys );
    }

    GeneratorHolder::GeneratorHolder()
        : _collectionsLock( "GeneratorHolder" ) {
    }

    bool GeneratorHolder::prepare( const StringData& ns,
                                   const BSONObj& obj,
                                   PregeneratedKeys* out ) {
        invariant( out );

        if ( Lock::isLocked() ) {
            // ewww, we do nothing
            return true;
        }

        shared_ptr<MyCollection> collection;
        {
            string temp = ns.toString();
            SimpleMutex::scoped_lock lk( _collectionsLock );
            Collections::const_iterator i = _collections.find( temp );
            if ( i == _collections.end() )
                return false;
            collection = i->second;
        }

        try {
            for ( size_t i = 0; i < collection->indexes.size(); i++ ) {
                out->gen( obj, collection->indexes[i].entry, collection->indexes[i].generator );
            }
        }
        catch ( DBException& e ) {
            log() << "GeneratorHolder::prepare failed: " << e;
            out->clear();
            return false;
        }
        return true;
    }

    void GeneratorHolder::reset( const Collection* aCollection ) {
        shared_ptr<MyCollection> myCollection( new MyCollection() );
        myCollection->ns = aCollection->ns().ns();

        IndexCatalog::IndexIterator ii = aCollection->getIndexCatalog()->getIndexIterator( true );
        while ( ii.more() ) {
            IndexDescriptor* desc = ii.next();
            IndexCatalogEntry* entry = ii.entry( desc );

            MyIndex myIndex;
            myIndex.entry = entry;
            myIndex.generator = entry->accessMethod()->getKeyGenerator();

            myCollection->indexes.push_back( myIndex );
        }

        SimpleMutex::scoped_lock lk( _collectionsLock );
        _collections[aCollection->ns().ns()] = myCollection;
    }

    void GeneratorHolder::dropped( const std::string& ns ) {
        SimpleMutex::scoped_lock lk( _collectionsLock );
        _collections.erase( ns );
    }

    void GeneratorHolder::droppedDatabase( const std::string& db ) {
        SimpleMutex::scoped_lock lk( _collectionsLock );
        vector<string> toDrop;
        for ( Collections::const_iterator i = _collections.begin(); i != _collections.end(); ++i ) {
            StringData temp = nsToDatabaseSubstring( i->first );
            if ( temp == db )
                toDrop.push_back( i->first );
        }

        for ( size_t i = 0; i < toDrop.size(); i++ ) {
            _collections.erase( toDrop[i] );
        }
    }

    namespace {
        // this is ok because we don't access this via the construction of any globals
        GeneratorHolder theHolder;
    }
    GeneratorHolder* GeneratorHolder::getInstance() {
        return &theHolder;
    }
}
