// collection.cpp

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

#include "mongo/db/structure/collection.h"

#include "mongo/base/counter.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/database.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/namespace_details.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/storage/extent.h"
#include "mongo/db/storage/extent_manager.h"
#include "mongo/db/structure/collection_iterator.h"

#include "mongo/db/pdfile.h" // XXX-ERH
#include "mongo/db/auth/user_document_parser.h" // XXX-ANDY

namespace mongo {

    Collection::Collection( const StringData& fullNS,
                            NamespaceDetails* details,
                            Database* database )
        : _ns( fullNS ),
          _recordStore( _ns.ns() ),
          _infoCache( this ),
          _indexCatalog( this, details ) {
        _details = details;
        _database = database;
        _recordStore.init( _details,
                           &database->getExtentManager(),
                           _ns.coll() == "system.indexes" );
        _magic = 1357924;
    }

    Collection::~Collection() {
        verify( ok() );
        _magic = 0;
    }

    bool Collection::requiresIdIndex() const {

        if ( _ns.ns().find( '$' ) != string::npos ) {
            // no indexes on indexes
            return false;
        }

        if ( _ns == _database->_namespacesName ||
             _ns == _database->_indexesName ||
             _ns == _database->_extentFreelistName ||
             _ns == _database->_profileName ) {
            return false;
        }

        if ( _ns.db() == "local" ) {
            if ( _ns.coll().startsWith( "oplog." ) )
                return false;
        }

        if ( !_ns.isSystem() ) {
            // non system collections definitely have an _id index
            return true;
        }


        return true;
    }

    CollectionIterator* Collection::getIterator( const DiskLoc& start, bool tailable,
                                                     const CollectionScanParams::Direction& dir) const {
        verify( ok() );
        if ( _details->isCapped() )
            return new CappedIterator( this, start, tailable, dir );
        return new FlatIterator( this, start, dir );
    }

    BSONObj Collection::docFor( const DiskLoc& loc ) {
        Record* rec = getExtentManager()->recordFor( loc );
        return BSONObj::make( rec->accessed() );
    }

    StatusWith<DiskLoc> Collection::insertDocument( const DocWriter* doc, bool enforceQuota ) {
        verify( _indexCatalog.numIndexesTotal() == 0 ); // eventually can implement, just not done

        int lenWHdr = _details->getRecordAllocationSize( doc->documentSize() + Record::HeaderSize );

        // TODO: for now, capped logic lives inside NamespaceDetails, which is hidden
        //       under the RecordStore, this feels broken since that should be a
        //       collection access method probably
        StatusWith<DiskLoc> loc = _recordStore.allocRecord( lenWHdr,
                                                            enforceQuota ? largestFileNumberInQuota() : 0 );
        if ( !loc.isOK() )
            return loc;

        Record *r = loc.getValue().rec();
        fassert( 17319, r->lengthWithHeaders() >= lenWHdr );

        // copy the data
        r = reinterpret_cast<Record*>( getDur().writingPtr(r, lenWHdr) );
        doc->writeDocument( r->data() );

        addRecordToRecListInExtent(r, loc.getValue()); // XXX move down into record store

        _details->incrementStats( r->netLength(), 1 );

        return StatusWith<DiskLoc>( loc );
    }

    StatusWith<DiskLoc> Collection::insertDocument( const BSONObj& docToInsert, bool enforceQuota ) {
        if ( _indexCatalog.findIdIndex() ) {
            if ( docToInsert["_id"].eoo() ) {
                return StatusWith<DiskLoc>( ErrorCodes::InternalError,
                                            "Collection::insertDocument got document without _id" );
            }
        }

        if ( _details->isCapped() ) {
            // TOOD: old god not done
            Status ret = _indexCatalog.checkNoIndexConflicts( docToInsert );
            if ( !ret.isOK() )
                return StatusWith<DiskLoc>( ret );
        }

        StatusWith<DiskLoc> status = _insertDocument( docToInsert, enforceQuota );
        if ( status.isOK() ) {
            _details->paddingFits();
        }

        return status;
    }

    StatusWith<DiskLoc> Collection::_insertDocument( const BSONObj& docToInsert, bool enforceQuota ) {

        int lenWHdr = _details->getRecordAllocationSize( docToInsert.objsize() + Record::HeaderSize );
        fassert( 17208, lenWHdr >= ( docToInsert.objsize() + Record::HeaderSize ) );

        // TODO: for now, capped logic lives inside NamespaceDetails, which is hidden
        //       under the RecordStore, this feels broken since that should be a
        //       collection access method probably
        StatusWith<DiskLoc> loc = _recordStore.allocRecord( lenWHdr,
                                                            enforceQuota ? largestFileNumberInQuota() : 0 );
        if ( !loc.isOK() )
            return loc;

        Record *r = loc.getValue().rec();
        fassert( 17210, r->lengthWithHeaders() >= lenWHdr );

        // copy the data
        r = reinterpret_cast<Record*>( getDur().writingPtr(r, lenWHdr) );
        memcpy( r->data(), docToInsert.objdata(), docToInsert.objsize() );

        addRecordToRecListInExtent(r, loc.getValue()); // XXX move down into record store

        _details->incrementStats( r->netLength(), 1 );

        _infoCache.notifyOfWriteOp();

        try {
            _indexCatalog.indexRecord( docToInsert, loc.getValue() );
        }
        catch ( AssertionException& e ) {
            if ( _details->isCapped() ) {
                return StatusWith<DiskLoc>( ErrorCodes::InternalError,
                                            str::stream() << "unexpected index insertion failure on"
                                            << " capped collection" << e.toString()
                                            << " - collection and its index will not match" );
            }

            // indexRecord takes care of rolling back indexes
            // so we just have to delete the main storage
            _recordStore.deallocRecord( loc.getValue(), r );
            return StatusWith<DiskLoc>( e.toStatus( "insertDocument" ) );
        }

        return loc;
    }

    void Collection::deleteDocument( const DiskLoc& loc, bool cappedOK, bool noWarn,
                                     BSONObj* deletedId ) {
        if ( _details->isCapped() && !cappedOK ) {
            log() << "failing remove on a capped ns " << _ns << endl;
            uasserted( 17115,  "cannot remove from a capped collection" ); // XXX 10089
            return;
        }

        BSONObj doc = docFor( loc );

        if ( deletedId ) {
            BSONElement e = doc["_id"];
            if ( e.type() ) {
                *deletedId = e.wrap();
            }
        }

        /* check if any cursors point to us.  if so, advance them. */
        ClientCursor::aboutToDelete(_ns.ns(), _details, loc);

        Record* rec = getExtentManager()->recordFor( loc );

        _indexCatalog.unindexRecord( doc, loc, noWarn);

        _recordStore.deallocRecord( loc, rec );

        _infoCache.notifyOfWriteOp();
    }

    Counter64 moveCounter;
    ServerStatusMetricField<Counter64> moveCounterDisplay( "record.moves", &moveCounter );

    StatusWith<DiskLoc> Collection::updateDocument( const DiskLoc& oldLocation,
                                                    const BSONObj& objNew,
                                                    bool enforceQuota,
                                                    OpDebug* debug ) {

        Record* oldRecord = getExtentManager()->recordFor( oldLocation );
        BSONObj objOld = BSONObj::make( oldRecord );

        if ( objOld.hasElement( "_id" ) ) {
            BSONElement oldId = objOld["_id"];
            BSONElement newId = objNew["_id"];
            if ( oldId != newId )
                return StatusWith<DiskLoc>( ErrorCodes::InternalError,
                                            "in Collection::updateDocument _id mismatch",
                                            13596 );
        }

        if ( ns().coll() == "system.users" ) {
            // XXX - andy and spencer think this should go away now
            V2UserDocumentParser parser;
            Status s = parser.checkValidUserDocument(objNew);
            if ( !s.isOK() )
                return StatusWith<DiskLoc>( s );
        }

        /* duplicate key check. we descend the btree twice - once for this check, and once for the actual inserts, further
           below.  that is suboptimal, but it's pretty complicated to do it the other way without rollbacks...
        */
        OwnedPointerVector<UpdateTicket> updateTickets;
        updateTickets.mutableVector().resize(_indexCatalog.numIndexesTotal());
        for (int i = 0; i < _indexCatalog.numIndexesTotal(); ++i) {
            IndexDescriptor* descriptor = _indexCatalog.getDescriptor( i );
            IndexAccessMethod* iam = _indexCatalog.getIndex( descriptor );

            InsertDeleteOptions options;
            options.logIfError = false;
            options.dupsAllowed =
                !(KeyPattern::isIdKeyPattern(descriptor->keyPattern()) || descriptor->unique())
                || ignoreUniqueIndex(descriptor);
            updateTickets.mutableVector()[i] = new UpdateTicket();
            Status ret = iam->validateUpdate(objOld, objNew, oldLocation, options,
                                             updateTickets.mutableVector()[i]);
            if ( !ret.isOK() ) {
                return StatusWith<DiskLoc>( ret );
            }
        }

        if ( oldRecord->netLength() < objNew.objsize() ) {
            // doesn't fit, have to move to new location

            if ( _details->isCapped() )
                return StatusWith<DiskLoc>( ErrorCodes::InternalError,
                                            "failing update: objects in a capped ns cannot grow",
                                            10003 );

            moveCounter.increment();
            _details->paddingTooSmall();

            // unindex old record, don't delete
            // this way, if inserting new doc fails, we can re-index this one
            ClientCursor::aboutToDelete(_ns.ns(), _details, oldLocation);
            _indexCatalog.unindexRecord( objOld, oldLocation, true );

            if ( debug ) {
                if (debug->nmoved == -1) // default of -1 rather than 0
                    debug->nmoved = 1;
                else
                    debug->nmoved += 1;
            }

            StatusWith<DiskLoc> loc = _insertDocument( objNew, enforceQuota );

            if ( loc.isOK() ) {
                // insert successful, now lets deallocate the old location
                // remember its already unindexed
                _recordStore.deallocRecord( oldLocation, oldRecord );
            }
            else {
                // new doc insert failed, so lets re-index the old document and location
                _indexCatalog.indexRecord( objOld, oldLocation );
            }

            return loc;
        }

        _infoCache.notifyOfWriteOp();
        _details->paddingFits();

        if ( debug )
            debug->keyUpdates = 0;

        for (int i = 0; i < _indexCatalog.numIndexesTotal(); ++i) {
            IndexDescriptor* descriptor = _indexCatalog.getDescriptor( i );
            IndexAccessMethod* iam = _indexCatalog.getIndex( descriptor );

            int64_t updatedKeys;
            Status ret = iam->update(*updateTickets.vector()[i], &updatedKeys);
            if ( !ret.isOK() )
                return StatusWith<DiskLoc>( ret );
            if ( debug )
                debug->keyUpdates += updatedKeys;
        }

        //  update in place
        int sz = objNew.objsize();
        memcpy(getDur().writingPtr(oldRecord->data(), sz), objNew.objdata(), sz);
        return StatusWith<DiskLoc>( oldLocation );
    }

    int64_t Collection::storageSize( int* numExtents, BSONArrayBuilder* extentInfo ) const {
        if ( _details->firstExtent().isNull() ) {
            if ( numExtents )
                *numExtents = 0;
            return 0;
        }

        Extent* e = getExtentManager()->getExtent( _details->firstExtent() );

        long long total = 0;
        int n = 0;
        while ( e ) {
            total += e->length;
            n++;

            if ( extentInfo ) {
                extentInfo->append( BSON( "len" << e->length << "loc: " << e->myLoc.toBSONObj() ) );
            }

            e = getExtentManager()->getNextExtent( e );
        }

        if ( numExtents )
            *numExtents = n;

        return total;
    }

    ExtentManager* Collection::getExtentManager() {
        verify( ok() );
        return &_database->getExtentManager();
    }

    const ExtentManager* Collection::getExtentManager() const {
        verify( ok() );
        return &_database->getExtentManager();
    }

    Extent* Collection::increaseStorageSize( int size, bool enforceQuota ) {
        return getExtentManager()->increaseStorageSize( _ns,
                                                        _details,
                                                        size,
                                                        enforceQuota ? largestFileNumberInQuota() : 0 );
    }

    int Collection::largestFileNumberInQuota() const {
        if ( !storageGlobalParams.quota )
            return 0;

        if ( _ns.db() == "local" )
            return 0;

        if ( _ns.isSpecial() )
            return 0;

        return storageGlobalParams.quotaFiles;
    }

    bool Collection::isCapped() const {
        return _details->isCapped();
    }

    uint64_t Collection::numRecords() const {
        return _details->numRecords();
    }

    uint64_t Collection::dataSize() const {
        return _details->dataSize();
    }

}
