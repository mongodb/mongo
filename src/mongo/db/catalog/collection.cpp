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

#include "mongo/db/catalog/collection.h"

#include "mongo/base/counter.h"
#include "mongo/base/owned_pointer_map.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/curop.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/structure/catalog/namespace_details.h"
#include "mongo/db/structure/catalog/namespace_details_rsv1_metadata.h"
#include "mongo/db/structure/record_store_v1_capped.h"
#include "mongo/db/structure/record_store_v1_simple.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/storage/extent.h"
#include "mongo/db/storage/extent_manager.h"

#include "mongo/db/auth/user_document_parser.h" // XXX-ANDY

namespace mongo {

    std::string CompactOptions::toString() const {
        std::stringstream ss;
        ss << "paddingMode: ";
        switch ( paddingMode ) {
        case NONE:
            ss << "NONE";
            break;
        case PRESERVE:
            ss << "PRESERVE";
            break;
        case MANUAL:
            ss << "MANUAL (" << paddingBytes << " + ( doc * " << paddingFactor <<") )";
        }

        ss << " validateDocuments: " << validateDocuments;

        return ss.str();
    }

    // ----

    Collection::Collection( const StringData& fullNS,
                            NamespaceDetails* details,
                            Database* database )
        : _ns( fullNS ),
          _infoCache( this ),
          _indexCatalog( this, details ),
          _cursorCache( fullNS ) {
        _details = details;
        _database = database;

        if ( details->isCapped() ) {
            _recordStore.reset( new CappedRecordStoreV1( this,
                                                         _ns.ns(),
                                                         new NamespaceDetailsRSV1MetaData( details ),
                                                         &database->getExtentManager(),
                                                         _ns.coll() == "system.indexes" ) );
        }
        else {
            _recordStore.reset( new SimpleRecordStoreV1( _ns.ns(),
                                                         new NamespaceDetailsRSV1MetaData( details ),
                                                         &database->getExtentManager(),
                                                         _ns.coll() == "system.indexes" ) );
        }
        _magic = 1357924;
        _indexCatalog.init();
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

    RecordIterator* Collection::getIterator( const DiskLoc& start, bool tailable,
                                                     const CollectionScanParams::Direction& dir) const {
        invariant( ok() );
        return _recordStore->getIterator( start, tailable, dir );
    }

    int64_t Collection::countTableScan( const MatchExpression* expression ) {
        scoped_ptr<RecordIterator> iterator( getIterator( DiskLoc(),
                                                              false,
                                                              CollectionScanParams::FORWARD ) );
        int64_t count = 0;
        while ( !iterator->isEOF() ) {
            DiskLoc loc = iterator->getNext();
            BSONObj obj = docFor( loc );
            if ( expression->matchesBSON( obj ) )
                count++;
        }

        return count;
    }

    BSONObj Collection::docFor( const DiskLoc& loc ) {
        Record* rec = _recordStore->recordFor( loc );
        return BSONObj( rec->accessed()->data() );
    }

    StatusWith<DiskLoc> Collection::insertDocument( const DocWriter* doc, bool enforceQuota ) {
        verify( _indexCatalog.numIndexesTotal() == 0 ); // eventually can implement, just not done

        StatusWith<DiskLoc> loc = _recordStore->insertRecord( doc,
                                                             enforceQuota ? largestFileNumberInQuota() : 0 );
        if ( !loc.isOK() )
            return loc;

        return StatusWith<DiskLoc>( loc );
    }

    StatusWith<DiskLoc> Collection::insertDocument( const BSONObj& docToInsert, bool enforceQuota ) {
        if ( _indexCatalog.findIdIndex() ) {
            if ( docToInsert["_id"].eoo() ) {
                return StatusWith<DiskLoc>( ErrorCodes::InternalError,
                                            str::stream() << "Collection::insertDocument got "
                                            "document without _id for ns:" << _ns.ns() );
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

    StatusWith<DiskLoc> Collection::insertDocument( const BSONObj& doc,
                                                    MultiIndexBlock& indexBlock ) {
        StatusWith<DiskLoc> loc = _recordStore->insertRecord( doc.objdata(),
                                                              doc.objsize(),
                                                              0 );

        if ( !loc.isOK() )
            return loc;

        InsertDeleteOptions indexOptions;
        indexOptions.logIfError = false;
        indexOptions.dupsAllowed = true; // in repair we should be doing no checking

        Status status = indexBlock.insert( doc, loc.getValue(), indexOptions );
        if ( !status.isOK() )
            return StatusWith<DiskLoc>( status );

        return loc;
    }


    StatusWith<DiskLoc> Collection::_insertDocument( const BSONObj& docToInsert,
                                                     bool enforceQuota ) {

        // TODO: for now, capped logic lives inside NamespaceDetails, which is hidden
        //       under the RecordStore, this feels broken since that should be a
        //       collection access method probably

        StatusWith<DiskLoc> loc = _recordStore->insertRecord( docToInsert.objdata(),
                                                              docToInsert.objsize(),
                                                              enforceQuota ? largestFileNumberInQuota() : 0 );
        if ( !loc.isOK() )
            return loc;

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
            _recordStore->deleteRecord( loc.getValue() );
            return StatusWith<DiskLoc>( e.toStatus( "insertDocument" ) );
        }

        return loc;
    }

    void Collection::deleteDocument( const DiskLoc& loc, bool cappedOK, bool noWarn,
                                     BSONObj* deletedId ) {
        if ( _details->isCapped() && !cappedOK ) {
            log() << "failing remove on a capped ns " << _ns << endl;
            uasserted( 10089,  "cannot remove from a capped collection" );
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
        _cursorCache.invalidateDocument(loc, INVALIDATION_DELETION);

        _indexCatalog.unindexRecord( doc, loc, noWarn);

        _recordStore->deleteRecord( loc );

        _infoCache.notifyOfWriteOp();
    }

    Counter64 moveCounter;
    ServerStatusMetricField<Counter64> moveCounterDisplay( "record.moves", &moveCounter );

    StatusWith<DiskLoc> Collection::updateDocument( const DiskLoc& oldLocation,
                                                    const BSONObj& objNew,
                                                    bool enforceQuota,
                                                    OpDebug* debug ) {

        Record* oldRecord = _recordStore->recordFor( oldLocation );
        BSONObj objOld( oldRecord->accessed()->data() );

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
        OwnedPointerMap<IndexDescriptor*,UpdateTicket> updateTickets;
        IndexCatalog::IndexIterator ii = _indexCatalog.getIndexIterator( true );
        while ( ii.more() ) {
            IndexDescriptor* descriptor = ii.next();
            IndexAccessMethod* iam = _indexCatalog.getIndex( descriptor );

            InsertDeleteOptions options;
            options.logIfError = false;
            options.dupsAllowed =
                !(KeyPattern::isIdKeyPattern(descriptor->keyPattern()) || descriptor->unique())
                || ignoreUniqueIndex(descriptor);
            UpdateTicket* updateTicket = new UpdateTicket();
            updateTickets.mutableMap()[descriptor] = updateTicket;
            Status ret = iam->validateUpdate(objOld, objNew, oldLocation, options, updateTicket );
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
            _cursorCache.invalidateDocument(oldLocation, INVALIDATION_DELETION);
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
                _recordStore->deleteRecord( oldLocation );
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

        ii = _indexCatalog.getIndexIterator( true );
        while ( ii.more() ) {
            IndexDescriptor* descriptor = ii.next();
            IndexAccessMethod* iam = _indexCatalog.getIndex( descriptor );

            int64_t updatedKeys;
            Status ret = iam->update(*updateTickets.mutableMap()[descriptor], &updatedKeys);
            if ( !ret.isOK() )
                return StatusWith<DiskLoc>( ret );
            if ( debug )
                debug->keyUpdates += updatedKeys;
        }

        // Broadcast the mutation so that query results stay correct.
        _cursorCache.invalidateDocument(oldLocation, INVALIDATION_MUTATION);

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

    void Collection::increaseStorageSize( int size, bool enforceQuota ) {
        _recordStore->increaseStorageSize( size, enforceQuota ? largestFileNumberInQuota() : 0 );
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

    /**
     * order will be:
     * 1) store index specs
     * 2) drop indexes
     * 3) truncate record store
     * 4) re-write indexes
     */
    Status Collection::truncate() {
        massert( 17445, "index build in progress", _indexCatalog.numIndexesInProgress() == 0 );

        // 1) store index specs
        vector<BSONObj> indexSpecs;
        {
            IndexCatalog::IndexIterator ii = _indexCatalog.getIndexIterator( false );
            while ( ii.more() ) {
                const IndexDescriptor* idx = ii.next();
                indexSpecs.push_back( idx->infoObj().getOwned() );
            }
        }

        // 2) drop indexes
        Status status = _indexCatalog.dropAllIndexes( true );
        if ( !status.isOK() )
            return status;
        _cursorCache.invalidateAll( false );
        _infoCache.reset();

        // 3) truncate record store
        status = _recordStore->truncate();
        if ( !status.isOK() )
            return status;

        // 4) re-create indexes
        for ( size_t i = 0; i < indexSpecs.size(); i++ ) {
            status = _indexCatalog.createIndex( indexSpecs[i], false );
            if ( !status.isOK() )
                return status;
        }

        return Status::OK();
    }

    void Collection::temp_cappedTruncateAfter( DiskLoc end, bool inclusive) {
        invariant( isCapped() );
        reinterpret_cast<CappedRecordStoreV1*>(_recordStore.get())->temp_cappedTruncateAfter( end, inclusive );
    }

    namespace {
        class MyValidateAdaptor : public ValidateAdaptor {
        public:
            virtual ~MyValidateAdaptor(){}

            virtual Status validate( Record* record, size_t* dataSize ) {
                BSONObj obj = BSONObj( record->data() );
                const Status status = validateBSON(obj.objdata(), obj.objsize());
                if ( status.isOK() )
                    *dataSize = obj.objsize();
                return Status::OK();
            }

        };
    }

    Status Collection::validate( bool full, bool scanData,
                                 ValidateResults* results, BSONObjBuilder* output ){

        MyValidateAdaptor adaptor;
        Status status = _recordStore->validate( full, scanData, &adaptor, results, output );
        if ( !status.isOK() )
            return status;

        { // indexes
            output->append("nIndexes", _indexCatalog.numIndexesReady() );
            int idxn = 0;
            try  {
                BSONObjBuilder indexes; // not using subObjStart to be exception safe
                IndexCatalog::IndexIterator i = _indexCatalog.getIndexIterator(false);
                while( i.more() ) {
                    const IndexDescriptor* descriptor = i.next();
                    log() << "validating index " << descriptor->indexNamespace() << endl;
                    IndexAccessMethod* iam = _indexCatalog.getIndex( descriptor );
                    invariant( iam );

                    int64_t keys;
                    iam->validate(&keys);
                    indexes.appendNumber(descriptor->indexNamespace(),
                                         static_cast<long long>(keys));
                    idxn++;
                }
                output->append("keysPerIndex", indexes.done());
            }
            catch ( DBException& exc ) {
                string err = str::stream() <<
                    "exception during index validate idxn "<<
                    BSONObjBuilder::numStr(idxn) <<
                    ": " << exc.toString();
                results->errors.push_back( err );
                results->valid = false;
            }
        }

        return Status::OK();
    }

}
