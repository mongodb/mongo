// record_store_v1_capped.cpp

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

#include "mongo/db/structure/record_store_v1_capped.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/dur.h"
#include "mongo/db/storage/extent.h"
#include "mongo/db/storage/extent_manager.h"
#include "mongo/db/storage/record.h"
#include "mongo/db/structure/catalog/namespace_details.h"

namespace mongo {

    CappedRecordStoreV1::CappedRecordStoreV1( Collection* collection,
                                              const StringData& ns,
                                              NamespaceDetails* details,
                                              ExtentManager* em,
                                              bool isSystemIndexes )
        : RecordStoreV1Base( ns, details, em, isSystemIndexes ),
          _collection( collection ) {

        DiskLoc extentLoc = details->firstExtent();
        while ( !extentLoc.isNull() ) {
            Extent* extent = em->getExtent( extentLoc );
            extentLoc = extent->xnext;
            MAdvise* m( new MAdvise( reinterpret_cast<void*>( extent ),
                                     extent->length,
                                     MAdvise::Sequential ) );
            _extentAdvice.mutableVector().push_back( m );
        }
    }

    CappedRecordStoreV1::~CappedRecordStoreV1() {
    }

    StatusWith<DiskLoc> CappedRecordStoreV1::allocRecord( int lengthWithHeaders, int quotaMax ) {
        DiskLoc loc = _details->alloc( _collection, _ns, lengthWithHeaders );
        if ( !loc.isNull() )
            return StatusWith<DiskLoc>( loc );

        return StatusWith<DiskLoc>( ErrorCodes::InternalError,
                                    "no space in capped collection" );
    }

    Status CappedRecordStoreV1::truncate() {
        // Get a writeable reference to 'this' and reset all pertinent
        // attributes.
        NamespaceDetails* t = _details->writingWithoutExtra();

        t->cappedLastDelRecLastExtent() = DiskLoc();
        t->cappedListOfAllDeletedRecords() = DiskLoc();

        // preserve firstExtent/lastExtent
        t->_capExtent = t->_firstExtent;
        t->_stats.datasize = t->_stats.nrecords = 0;
        // lastExtentSize preserve
        // nIndexes preserve 0
        // capped preserve true
        // max preserve
        t->_paddingFactor = 1.0;
        t->_systemFlags = 0;
        t->_capFirstNewRecord = DiskLoc();
        t->_capFirstNewRecord.setInvalid();
        t->cappedLastDelRecLastExtent().setInvalid();
        // dataFileVersion preserve
        // indexFileVersion preserve
        t->_multiKeyIndexBits = 0;
        t->_reservedA = 0;
        t->_extraOffset = 0;
        // indexBuildInProgress preserve 0
        memset(t->_reserved, 0, sizeof(t->_reserved));

        // Reset all existing extents and recreate the deleted list.
        for( DiskLoc ext = t->_firstExtent; !ext.isNull(); ext = ext.ext()->xnext ) {
            DiskLoc prev = ext.ext()->xprev;
            DiskLoc next = ext.ext()->xnext;
            DiskLoc empty = ext.ext()->reuse( _ns, true );
            ext.ext()->xprev.writing() = prev;
            ext.ext()->xnext.writing() = next;
            _details->addDeletedRec( empty.drec(), empty );
        }

        return Status::OK();
    }

    void CappedRecordStoreV1::temp_cappedTruncateAfter( DiskLoc end, bool inclusive ) {
        _details->cappedTruncateAfter( _ns.c_str(), end, inclusive );
    }
}
