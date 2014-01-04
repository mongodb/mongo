// state.cpp

/**
*    Copyright (C) 2008 10gen Inc.
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

#include "mongo/db/structure/btree/state.h"

#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/storage/extent_manager.h"
#include "mongo/db/structure/record_store.h"

namespace mongo {

    BtreeInMemoryState::BtreeInMemoryState( Collection* collection,
                                            const IndexDescriptor* descriptor,
                                            RecordStore* recordStore )
        : _collection( collection ),
          _descriptor( descriptor ),
          _recordStore( recordStore ),
          _ordering( Ordering::make( descriptor->keyPattern() ) ) {
        _isReady = false;
        _isMultikeySet = false;
        _head.Null();
    }

    int BtreeInMemoryState::_indexNo() const {
        NamespaceDetails* nsd = _collection->details();
        int idxNo = nsd->_catalogFindIndexByName( _descriptor->indexName(), true );
        fassert( 17333, idxNo >= 0 );
        return idxNo;
    }

    const DiskLoc& BtreeInMemoryState::head() const {
        if ( _head.isNull() ) {
            _head = _catalogFindHeadFromDisk();
            return _head;
        }

        DEV {
            if ( _head != _catalogFindHeadFromDisk() ) {
                log() << "_head: " << _head
                      << " _catalogFindHeadFromDisk(): " << _catalogFindHeadFromDisk();
            }
            verify( _head == _catalogFindHeadFromDisk() );
        }

        return _head;
    }

    DiskLoc BtreeInMemoryState::_catalogFindHeadFromDisk() const {
        NamespaceDetails* nsd = _collection->details();
        int idxNo = _indexNo();
        return nsd->idx( idxNo ).head;
    }

    void BtreeInMemoryState::setHead( DiskLoc newHead ) {
        NamespaceDetails* nsd = _collection->details();
        int idxNo = _indexNo();
        IndexDetails& id = nsd->idx( idxNo );
        id.head.writing() = newHead;
        _head = newHead;
    }

    void BtreeInMemoryState::setMultikey() {
        NamespaceDetails* nsd = _collection->details();
        int idxNo = _indexNo();
        if ( nsd->setIndexIsMultikey( idxNo, true ) )
            _collection->infoCache()->clearQueryCache();

        _isMultikeySet = true;
        _isMultikey = true;
    }

    bool BtreeInMemoryState::isMultikey() const {
        if ( _isMultikeySet ) {
            DEV {
                NamespaceDetails* nsd = _collection->details();
                int idxNo = _indexNo();
                verify( _isMultikey == nsd->isMultikey( idxNo ) );
            }
            return _isMultikey;
        }

        NamespaceDetails* nsd = _collection->details();
        int idxNo = _indexNo();
        _isMultikey = nsd->isMultikey( idxNo );
        _isMultikeySet = true;
        return _isMultikey;
    }


    bool BtreeInMemoryState::isReady() const {
        DEV _debugCheckVerifyReady();
        return _isReady;
    }

    void BtreeInMemoryState::setIsReady( bool isReady ) {
        _isReady = isReady;
        DEV _debugCheckVerifyReady();
        if ( isReady ) {
            // get caches ready
            head();
            isMultikey();

            fassert( 17339, _head == _catalogFindHeadFromDisk() );
            fassert( 17340, _isMultikey == _collection->details()->isMultikey( _indexNo() ) );
        }
    }

    void BtreeInMemoryState::_debugCheckVerifyReady() const {
        bool real = _indexNo() < _collection->getIndexCatalog()->numIndexesReady();
        verify( real == _isReady );
    }
}
