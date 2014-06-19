// namespace_details_rsv1_metadata.cpp

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

#include "mongo/db/ops/update.h"
#include "mongo/db/structure/catalog/namespace_details_rsv1_metadata.h"

namespace mongo {
    NamespaceDetailsRSV1MetaData::NamespaceDetailsRSV1MetaData( const StringData& ns,
                                                                NamespaceDetails* details,
                                                                RecordStore* namespaceRecordStore )
        : _ns( ns.toString() ),
          _details( details ),
          _namespaceRecordStore( namespaceRecordStore ) {
    }

    const DiskLoc& NamespaceDetailsRSV1MetaData::capExtent() const {
        return _details->capExtent;
    }

    void NamespaceDetailsRSV1MetaData::setCapExtent( OperationContext* txn, const DiskLoc& loc ) {
        *txn->recoveryUnit()->writing( &_details->capExtent ) = loc;
    }

    const DiskLoc& NamespaceDetailsRSV1MetaData::capFirstNewRecord() const {
        return _details->capFirstNewRecord;
    }

    void NamespaceDetailsRSV1MetaData::setCapFirstNewRecord( OperationContext* txn,
                                                             const DiskLoc& loc ) {
        *txn->recoveryUnit()->writing( &_details->capFirstNewRecord ) = loc;
    }

    bool NamespaceDetailsRSV1MetaData::capLooped() const {
        return _details->capFirstNewRecord.isValid();
    }

    long long NamespaceDetailsRSV1MetaData::dataSize() const {
        return _details->stats.datasize;
    }
    long long NamespaceDetailsRSV1MetaData::numRecords() const {
        return _details->stats.nrecords;
    }

    void NamespaceDetailsRSV1MetaData::incrementStats( OperationContext* txn,
                                                       long long dataSizeIncrement,
                                                       long long numRecordsIncrement ) {
        // durability todo : this could be a bit annoying / slow to record constantly
        NamespaceDetails::Stats* s = txn->recoveryUnit()->writing( &_details->stats );
        s->datasize += dataSizeIncrement;
        s->nrecords += numRecordsIncrement;
    }

    void NamespaceDetailsRSV1MetaData::setStats( OperationContext* txn,
                                                 long long dataSize,
                                                 long long numRecords ) {
        NamespaceDetails::Stats* s = txn->recoveryUnit()->writing( &_details->stats );
        s->datasize = dataSize;
        s->nrecords = numRecords;
    }

    const DiskLoc& NamespaceDetailsRSV1MetaData::deletedListEntry( int bucket ) const {
        return _details->deletedList[ bucket ];
    }

    void NamespaceDetailsRSV1MetaData::setDeletedListEntry( OperationContext* txn,
                                                            int bucket,
                                                            const DiskLoc& loc ) {
        *txn->recoveryUnit()->writing( &_details->deletedList[bucket] ) = loc;
    }

    void NamespaceDetailsRSV1MetaData::orphanDeletedList( OperationContext* txn ) {
        for( int i = 0; i < Buckets; i++ ) {
            setDeletedListEntry( txn, i, DiskLoc() );
        }
    }

    const DiskLoc& NamespaceDetailsRSV1MetaData::firstExtent() const {
        return _details->firstExtent;
    }

    void NamespaceDetailsRSV1MetaData::setFirstExtent( OperationContext* txn, const DiskLoc& loc ) {
        *txn->recoveryUnit()->writing( &_details->firstExtent ) = loc;
    }

    const DiskLoc& NamespaceDetailsRSV1MetaData::lastExtent() const {
        return _details->lastExtent;
    }

    void NamespaceDetailsRSV1MetaData::setLastExtent( OperationContext* txn, const DiskLoc& loc ) {
        *txn->recoveryUnit()->writing( &_details->lastExtent ) = loc;
    }

    bool NamespaceDetailsRSV1MetaData::isCapped() const {
        return _details->isCapped;
    }

    bool NamespaceDetailsRSV1MetaData::isUserFlagSet( int flag ) const {
        return _details->userFlags & flag;
    }

    int NamespaceDetailsRSV1MetaData::userFlags() const {
        return _details->userFlags;
    }

    bool NamespaceDetailsRSV1MetaData::setUserFlag( OperationContext* txn, int flag ) {
        if ( ( _details->userFlags & flag ) == flag )
            return false;

        txn->recoveryUnit()->writingInt( _details->userFlags) |= flag;
        _syncUserFlags( txn );
        return true;
    }

    bool NamespaceDetailsRSV1MetaData::clearUserFlag( OperationContext* txn, int flag ) {
        if ( ( _details->userFlags & flag ) == 0 )
            return false;

        txn->recoveryUnit()->writingInt(_details->userFlags) &= ~flag;
        _syncUserFlags( txn );
        return true;
    }

    bool NamespaceDetailsRSV1MetaData::replaceUserFlags( OperationContext* txn, int flags ) {
        if ( _details->userFlags == flags )
            return false;

        txn->recoveryUnit()->writingInt(_details->userFlags) = flags;
        _syncUserFlags( txn );
        return true;
    }

    int NamespaceDetailsRSV1MetaData::lastExtentSize() const {
        return _details->lastExtentSize;
    }

    void NamespaceDetailsRSV1MetaData::setLastExtentSize( OperationContext* txn, int newMax ) {
        if ( _details->lastExtentSize == newMax )
            return;
        txn->recoveryUnit()->writingInt(_details->lastExtentSize) = newMax;
    }

    long long NamespaceDetailsRSV1MetaData::maxCappedDocs() const {
        invariant( _details->isCapped );
        if ( _details->maxDocsInCapped == 0x7fffffff )
            return numeric_limits<long long>::max();
        return _details->maxDocsInCapped;
    }

    double NamespaceDetailsRSV1MetaData::paddingFactor() const {
        return _details->paddingFactor;
    }

    void NamespaceDetailsRSV1MetaData::setPaddingFactor( OperationContext* txn, double paddingFactor ) {
        if ( paddingFactor == _details->paddingFactor )
            return;

        if ( _details->isCapped )
            return;

        *txn->recoveryUnit()->writing(&_details->paddingFactor) = paddingFactor;
    }

    void NamespaceDetailsRSV1MetaData::_syncUserFlags( OperationContext* txn ) {
        if ( !_namespaceRecordStore )
            return;

        scoped_ptr<RecordIterator> iterator( _namespaceRecordStore->getIterator( DiskLoc(),
                                                                                 false,
                                                                                 CollectionScanParams::FORWARD ) );
        while ( !iterator->isEOF() ) {
            DiskLoc loc = iterator->getNext();

            BSONObj oldEntry = iterator->dataFor( loc ).toBson();
            BSONElement e = oldEntry["name"];
            if ( e.type() != String )
                continue;

            if ( e.String() != _ns )
                continue;

            BSONObj newEntry = applyUpdateOperators( oldEntry,
                                                     BSON( "$set" << BSON( "options.flags" << userFlags() ) ) );

            StatusWith<DiskLoc> result = _namespaceRecordStore->updateRecord( txn,
                                                                              loc,
                                                                              newEntry.objdata(),
                                                                              newEntry.objsize(),
                                                                              -1,
                                                                              NULL );
            fassert( 17486, result.isOK() );
            return;
        }

        fassertFailed( 17488 );
    }

}
