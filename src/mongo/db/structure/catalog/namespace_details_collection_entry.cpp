// namespace_details_collection_entry.h

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

#include "mongo/db/structure/catalog/namespace_details_collection_entry.h"

#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/storage/mmap_v1/mmap_v1_database_catalog_entry.h"
#include "mongo/db/structure/catalog/namespace_details.h"
#include "mongo/db/structure/record_store.h"
#include "mongo/util/startup_test.h"

namespace mongo {
    NamespaceDetailsCollectionCatalogEntry::NamespaceDetailsCollectionCatalogEntry( const StringData& ns,
                                                                                    NamespaceDetails* details,
                                                                                    RecordStore* indexRecordStore,
                                                                                    MMAPV1DatabaseCatalogEntry* db )
        : CollectionCatalogEntry( ns ),
          _details( details ),
          _indexRecordStore( indexRecordStore ),
          _db( db ) {
    }

    CollectionOptions NamespaceDetailsCollectionCatalogEntry::getCollectionOptions(OperationContext* txn) const {
        return _db->getCollectionOptions( txn, ns().ns() );
    }

    int NamespaceDetailsCollectionCatalogEntry::getTotalIndexCount() const {
        return _details->nIndexes + _details->indexBuildsInProgress;
    }

    int NamespaceDetailsCollectionCatalogEntry::getCompletedIndexCount() const {
        return _details->nIndexes;
    }

    int NamespaceDetailsCollectionCatalogEntry::getMaxAllowedIndexes() const {
        return NamespaceDetails::NIndexesMax;
    }

    void NamespaceDetailsCollectionCatalogEntry::getAllIndexes( std::vector<std::string>* names ) const {
        NamespaceDetails::IndexIterator i = _details->ii( true );
        while ( i.more() ) {
            const IndexDetails& id = i.next();
            const BSONObj obj = _indexRecordStore->dataFor( id.info ).toBson();
            names->push_back( obj.getStringField("name") );
        }
    }

    bool NamespaceDetailsCollectionCatalogEntry::isIndexMultikey(const StringData& idxName) const {
        int idxNo = _findIndexNumber( idxName );
        invariant( idxNo >= 0 );
        return isIndexMultikey( idxNo );
    }

    bool NamespaceDetailsCollectionCatalogEntry::isIndexMultikey(int idxNo) const {
        return (_details->multiKeyIndexBits & (((unsigned long long) 1) << idxNo)) != 0;
    }

    bool NamespaceDetailsCollectionCatalogEntry::setIndexIsMultikey(OperationContext* txn,
                                                                    const StringData& indexName,
                                                                    bool multikey ) {

        int idxNo = _findIndexNumber( indexName );
        invariant( idxNo >= 0 );
        return setIndexIsMultikey( txn, idxNo, multikey );
    }

    bool NamespaceDetailsCollectionCatalogEntry::setIndexIsMultikey(OperationContext* txn,
                                                                    int idxNo,
                                                                    bool multikey ) {
        unsigned long long mask = 1ULL << idxNo;

        if (multikey) {
            // Shortcut if the bit is already set correctly
            if (_details->multiKeyIndexBits & mask) {
                return false;
            }

            *txn->recoveryUnit()->writing(&_details->multiKeyIndexBits) |= mask;
        }
        else {
            // Shortcut if the bit is already set correctly
            if (!(_details->multiKeyIndexBits & mask)) {
                return false;
            }

            // Invert mask: all 1's except a 0 at the ith bit
            mask = ~mask;
            *txn->recoveryUnit()->writing(&_details->multiKeyIndexBits) &= mask;
        }

        return true;
    }

    DiskLoc NamespaceDetailsCollectionCatalogEntry::getIndexHead( const StringData& idxName ) const {
        int idxNo = _findIndexNumber( idxName );
        invariant( idxNo >= 0 );
        return _details->idx( idxNo ).head;
    }

    BSONObj NamespaceDetailsCollectionCatalogEntry::getIndexSpec( const StringData& idxName ) const {
        int idxNo = _findIndexNumber( idxName );
        invariant( idxNo >= 0 );
        const IndexDetails& id = _details->idx( idxNo );
        return _indexRecordStore->dataFor( id.info ).toBson();
    }

    void NamespaceDetailsCollectionCatalogEntry::setIndexHead( OperationContext* txn,
                                                               const StringData& idxName,
                                                               const DiskLoc& newHead ) {
        int idxNo = _findIndexNumber( idxName );
        invariant( idxNo >= 0 );
        *txn->recoveryUnit()->writing( &_details->idx( idxNo ).head) = newHead;
    }

    bool NamespaceDetailsCollectionCatalogEntry::isIndexReady( const StringData& idxName ) const {
        int idxNo = _findIndexNumber( idxName );
        invariant( idxNo >= 0 );
        return idxNo < getCompletedIndexCount();
    }

    int NamespaceDetailsCollectionCatalogEntry::_findIndexNumber( const StringData& idxName ) const {
        NamespaceDetails::IndexIterator i = _details->ii( true );
        while ( i.more() ) {
            const IndexDetails& id = i.next();
            int idxNo = i.pos() - 1;
            const BSONObj obj = _indexRecordStore->dataFor( id.info ).toBson();
            if ( idxName == obj.getStringField("name") )
                return idxNo;
        }
        return -1;
    }

    /* remove bit from a bit array - actually remove its slot, not a clear
       note: this function does not work with x == 63 -- that is ok
             but keep in mind in the future if max indexes were extended to
             exactly 64 it would be a problem
    */
    unsigned long long removeAndSlideBit(unsigned long long b, int x) {
        unsigned long long tmp = b;
        return
            (tmp & ((((unsigned long long) 1) << x)-1)) |
            ((tmp >> (x+1)) << x);
    }

    class IndexUpdateTest : public StartupTest {
    public:
        void run() {
            verify( removeAndSlideBit(1, 0) == 0 );
            verify( removeAndSlideBit(2, 0) == 1 );
            verify( removeAndSlideBit(2, 1) == 0 );
            verify( removeAndSlideBit(255, 1) == 127 );
            verify( removeAndSlideBit(21, 2) == 9 );
            verify( removeAndSlideBit(0x4000000000000001ULL, 62) == 1 );
        }
    } iu_unittest;

    Status NamespaceDetailsCollectionCatalogEntry::removeIndex( OperationContext* txn,
                                                                const StringData& indexName ) {
        int idxNo = _findIndexNumber( indexName );
        if ( idxNo < 0 )
            return Status( ErrorCodes::NamespaceNotFound, "index not found to remove" );

        DiskLoc infoLocation = _details->idx( idxNo ).info;

        { // sanity check
            BSONObj info = _indexRecordStore->dataFor( infoLocation ).toBson();
            invariant( info["name"].String() == indexName );
        }

        { // drop the namespace
            string indexNamespace = IndexDescriptor::makeIndexNamespace( ns().ns(), indexName );
            Status status = _db->dropCollection( txn, indexNamespace );
            if ( !status.isOK() ) {
                return status;
            }
        }

        { // all info in the .ns file
            NamespaceDetails* d = _details->writingWithExtra( txn );

            // fix the _multiKeyIndexBits, by moving all bits above me down one
            d->multiKeyIndexBits = removeAndSlideBit(d->multiKeyIndexBits, idxNo);

            if ( idxNo >= d->nIndexes )
                d->indexBuildsInProgress--;
            else
                d->nIndexes--;

            for ( int i = idxNo; i < getTotalIndexCount(); i++ )
                d->idx(i) = d->idx(i+1);

            d->idx( getTotalIndexCount() ) = IndexDetails();
        }

        // remove from system.indexes
        _indexRecordStore->deleteRecord( txn, infoLocation );

        return Status::OK();
    }

    Status NamespaceDetailsCollectionCatalogEntry::prepareForIndexBuild( OperationContext* txn,
                                                                         const IndexDescriptor* desc ) {
        BSONObj spec = desc->infoObj();
        // 1) entry in system.indexs
        StatusWith<DiskLoc> systemIndexesEntry = _indexRecordStore->insertRecord( txn,
                                                                                  spec.objdata(),
                                                                                  spec.objsize(),
                                                                                  -1 );
        if ( !systemIndexesEntry.isOK() )
            return systemIndexesEntry.getStatus();

        // 2) NamespaceDetails mods
        IndexDetails *id;
        try {
            id = &_details->idx(getTotalIndexCount(), true);
        }
        catch( DBException& ) {
            _details->allocExtra(txn,
                                 ns().ns(),
                                 _db->_namespaceIndex,
                                 getTotalIndexCount());
            id = &_details->idx(getTotalIndexCount(), false);
        }

        *txn->recoveryUnit()->writing( &id->info ) = systemIndexesEntry.getValue();
        *txn->recoveryUnit()->writing( &id->head ) = DiskLoc();

        txn->recoveryUnit()->writingInt( _details->indexBuildsInProgress ) += 1;

        // 3) indexes entry in .ns file
        NamespaceIndex& nsi = _db->_namespaceIndex;
        invariant( nsi.details( desc->indexNamespace() ) == NULL );
        nsi.add_ns( txn, desc->indexNamespace(), DiskLoc(), false );

        // 4) system.namespaces entry index ns
        _db->_addNamespaceToNamespaceCollection( txn, desc->indexNamespace(), NULL);

        return Status::OK();
    }

    void NamespaceDetailsCollectionCatalogEntry::indexBuildSuccess( OperationContext* txn,
                                                                    const StringData& indexName ) {
        int idxNo = _findIndexNumber( indexName );
        fassert( 17202, idxNo >= 0 );

        // Make sure the newly created index is relocated to nIndexes, if it isn't already there
        if ( idxNo != getCompletedIndexCount() ) {
            int toIdxNo = getCompletedIndexCount();

            //_details->swapIndex( txn, idxNo, toIdxNo );

            // flip main meta data
            IndexDetails temp = _details->idx(idxNo);
            *txn->recoveryUnit()->writing(&_details->idx(idxNo)) = _details->idx(toIdxNo);
            *txn->recoveryUnit()->writing(&_details->idx(toIdxNo)) = temp;

            // flip multi key bits
            bool tempMultikey = isIndexMultikey(idxNo);
            setIndexIsMultikey( txn, idxNo, isIndexMultikey(toIdxNo) );
            setIndexIsMultikey( txn, toIdxNo, tempMultikey );

            idxNo = toIdxNo;
            invariant( idxNo = _findIndexNumber( indexName ) );
        }

        txn->recoveryUnit()->writingInt( _details->indexBuildsInProgress ) -= 1;
        txn->recoveryUnit()->writingInt( _details->nIndexes ) += 1;

        invariant( isIndexReady( indexName ) );
    }

    void NamespaceDetailsCollectionCatalogEntry::updateTTLSetting( OperationContext* txn,
                                                                   const StringData& idxName,
                                                                   long long newExpireSeconds ) {
        int idx = _findIndexNumber( idxName );
        invariant( idx >= 0 );

        IndexDetails& indexDetails = _details->idx( idx );

        BSONObj obj = _indexRecordStore->dataFor( indexDetails.info ).toBson();
        const BSONElement oldExpireSecs = obj.getField("expireAfterSeconds");

        // Important that we set the new value in-place.  We are writing directly to the
        // object here so must be careful not to overwrite with a longer numeric type.

        char* nonConstPtr = const_cast<char*>(oldExpireSecs.value());
        switch( oldExpireSecs.type() ) {
        case EOO:
            massert( 16631, "index does not have an 'expireAfterSeconds' field", false );
            break;
        case NumberInt:
            *txn->recoveryUnit()->writing(reinterpret_cast<int*>(nonConstPtr)) = newExpireSeconds;
            break;
        case NumberDouble:
            *txn->recoveryUnit()->writing(reinterpret_cast<double*>(nonConstPtr)) = newExpireSeconds;
            break;
        case NumberLong:
            *txn->recoveryUnit()->writing(reinterpret_cast<long long*>(nonConstPtr)) = newExpireSeconds;
            break;
        default:
            massert( 16632, "current 'expireAfterSeconds' is not a number", false );
        }
    }


}
