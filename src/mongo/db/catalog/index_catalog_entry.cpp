// index_catalog_entry.cpp

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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndexing

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/index_catalog_entry.h"

#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/head_manager.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/file_allocator.h"
#include "mongo/util/log.h"

namespace mongo {

    class HeadManagerImpl : public HeadManager {
    public:
        HeadManagerImpl(IndexCatalogEntry* ice) : _catalogEntry(ice) { }
        virtual ~HeadManagerImpl() { }

        const DiskLoc getHead(OperationContext* txn) const {
            return _catalogEntry->head(txn);
        }

        void setHead(OperationContext* txn, const DiskLoc newHead) {
            _catalogEntry->setHead(txn, newHead);
        }

    private:
        // Not owned here.
        IndexCatalogEntry* _catalogEntry;
    };

    IndexCatalogEntry::IndexCatalogEntry( const StringData& ns,
                                          CollectionCatalogEntry* collection,
                                          IndexDescriptor* descriptor,
                                          CollectionInfoCache* infoCache )
        : _ns( ns.toString() ),
          _collection( collection ),
          _descriptor( descriptor ),
          _infoCache( infoCache ),
          _accessMethod( NULL ),
          _headManager(new HeadManagerImpl(this)),
          _ordering( Ordering::make( descriptor->keyPattern() ) ),
          _isReady( false ) {
        _descriptor->_cachedEntry = this;
    }

    IndexCatalogEntry::~IndexCatalogEntry() {
        _descriptor->_cachedEntry = NULL; // defensive

        delete _headManager;
        delete _accessMethod;
        delete _descriptor;
    }

    void IndexCatalogEntry::init( OperationContext* txn,
                                  IndexAccessMethod* accessMethod ) {
        verify( _accessMethod == NULL );
        _accessMethod = accessMethod;

        _isReady = _catalogIsReady( txn );
        _head = _catalogHead( txn );
        _isMultikey = _catalogIsMultikey( txn );
    }

    const DiskLoc& IndexCatalogEntry::head( OperationContext* txn ) const {
        DEV invariant( _head == _catalogHead( txn ) );
        return _head;
    }

    bool IndexCatalogEntry::isReady( OperationContext* txn ) const {
        DEV invariant( _isReady == _catalogIsReady( txn ) );
        return _isReady;
    }

    bool IndexCatalogEntry::isMultikey( OperationContext* txn ) const {
        DEV invariant( _isMultikey == _catalogIsMultikey( txn ) );
        return _isMultikey;
    }

    // ---

    void IndexCatalogEntry::setIsReady( bool newIsReady ) {
        _isReady = newIsReady;
    }

    void IndexCatalogEntry::setHead( OperationContext* txn, DiskLoc newHead ) {
        _collection->setIndexHead( txn,
                                   _descriptor->indexName(),
                                   newHead );
        _head = newHead;
    }

    void IndexCatalogEntry::setMultikey( OperationContext* txn ) {
        if ( isMultikey( txn ) )
            return;
        if ( _collection->setIndexIsMultikey( txn,
                                              _descriptor->indexName(),
                                              true ) ) {
            if ( _infoCache ) {
                LOG(1) << _ns << ": clearing plan cache - index "
                       << _descriptor->keyPattern() << " set to multi key.";
                _infoCache->clearQueryCache();
            }
        }
        _isMultikey = true;
    }

    // ----

    bool IndexCatalogEntry::_catalogIsReady( OperationContext* txn ) const {
        return _collection->isIndexReady( txn, _descriptor->indexName() );
    }

    DiskLoc IndexCatalogEntry::_catalogHead( OperationContext* txn ) const {
        return _collection->getIndexHead( txn, _descriptor->indexName() );
    }

    bool IndexCatalogEntry::_catalogIsMultikey( OperationContext* txn ) const {
        return _collection->isIndexMultikey( txn, _descriptor->indexName() );
    }

    // ------------------

    const IndexCatalogEntry* IndexCatalogEntryContainer::find( const IndexDescriptor* desc ) const {
        if ( desc->_cachedEntry )
            return desc->_cachedEntry;

        for ( const_iterator i = begin(); i != end(); ++i ) {
            const IndexCatalogEntry* e = *i;
            if ( e->descriptor() == desc )
                    return e;
        }
        return NULL;
    }

    IndexCatalogEntry* IndexCatalogEntryContainer::find( const IndexDescriptor* desc ) {
        if ( desc->_cachedEntry )
            return desc->_cachedEntry;

        for ( iterator i = begin(); i != end(); ++i ) {
            IndexCatalogEntry* e = *i;
            if ( e->descriptor() == desc )
                return e;
        }
        return NULL;
    }

    IndexCatalogEntry* IndexCatalogEntryContainer::find( const string& name ) {
        for ( iterator i = begin(); i != end(); ++i ) {
            IndexCatalogEntry* e = *i;
            if ( e->descriptor()->indexName() == name )
                return e;
        }
        return NULL;
    }

    bool IndexCatalogEntryContainer::remove( const IndexDescriptor* desc ) {
        for ( std::vector<IndexCatalogEntry*>::iterator i = _entries.mutableVector().begin();
              i != _entries.mutableVector().end();
              ++i ) {
            IndexCatalogEntry* e = *i;
            if ( e->descriptor() != desc )
                continue;
            _entries.mutableVector().erase( i );
            delete e;
            return true;
        }
        return false;
    }

}  // namespace mongo
