// collection_compact.cpp

/**
*    Copyright (C) 2013 MongoDB Inc.
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
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/curop.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/structure/catalog/namespace_details.h"
#include "mongo/db/storage/extent.h"
#include "mongo/db/storage/extent_manager.h"
#include "mongo/db/storage/record.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/touch_pages.h"

namespace mongo {

    namespace {
        BSONObj _compactAdjustIndexSpec( const BSONObj& oldSpec ) {
            BSONObjBuilder b;
            BSONObj::iterator i( oldSpec );
            while( i.more() ) {
                BSONElement e = i.next();
                if ( str::equals( e.fieldName(), "v" ) ) {
                    // Drop any preexisting index version spec.  The default index version will
                    // be used instead for the new index.
                    continue;
                }
                if ( str::equals( e.fieldName(), "background" ) ) {
                    // Create the new index in the foreground.
                    continue;
                }
                // Pass the element through to the new index spec.
                b.append(e);
            }
            return b.obj();
        }

        class MyCompactAdaptor : public RecordStoreCompactAdaptor {
        public:
            MyCompactAdaptor(Collection* collection,
                             MultiIndexBlock* indexBlock)

                : _collection( collection ),
                  _multiIndexBlock(indexBlock) {
            }

            virtual bool isDataValid( Record* rec ) {
                return BSONObj( rec->data() ).valid();
            }

            virtual size_t dataSize( Record* rec ) {
                return BSONObj( rec->data() ).objsize();
            }

            virtual void inserted( Record* rec, const DiskLoc& newLocation ) {
                InsertDeleteOptions options;
                options.logIfError = false;
                options.dupsAllowed = true; // in compact we should be doing no checking

                _multiIndexBlock->insert( BSONObj( rec->data() ), newLocation, options );
            }

        private:
            Collection* _collection;

            MultiIndexBlock* _multiIndexBlock;
        };

    }


    StatusWith<CompactStats> Collection::compact( OperationContext* txn,
                                                  const CompactOptions* compactOptions ) {
        if ( !_recordStore->compactSupported() )
            return StatusWith<CompactStats>( ErrorCodes::BadValue,
                                             str::stream() <<
                                             "cannot compact collection with record store: " <<
                                             _recordStore->name() );

        if ( _indexCatalog.numIndexesInProgress() )
            return StatusWith<CompactStats>( ErrorCodes::BadValue,
                                             "cannot compact when indexes in progress" );


        // same data, but might perform a little different after compact?
        _infoCache.reset();

        vector<BSONObj> indexSpecs;
        {
            IndexCatalog::IndexIterator ii( _indexCatalog.getIndexIterator( false ) );
            while ( ii.more() ) {
                IndexDescriptor* descriptor = ii.next();

                const BSONObj spec = _compactAdjustIndexSpec(descriptor->infoObj());
                const BSONObj key = spec.getObjectField("key");
                const Status keyStatus = validateKeyPattern(key);
                if (!keyStatus.isOK()) {
                    return StatusWith<CompactStats>(
                        ErrorCodes::CannotCreateIndex,
                        str::stream() << "Cannot compact collection due to invalid index "
                                      << spec << ": " << keyStatus.reason() << " For more info see"
                                      << " http://dochub.mongodb.org/core/index-validation");
                }
                indexSpecs.push_back(spec);
            }
        }

        // note that the drop indexes call also invalidates all clientcursors for the namespace,
        // which is important and wanted here
        log() << "compact dropping indexes" << endl;
        Status status = _indexCatalog.dropAllIndexes(txn, true);
        if ( !status.isOK() ) {
            return StatusWith<CompactStats>( status );
        }

        txn->checkForInterrupt();

        CompactStats stats;

        MultiIndexBlock multiIndexBlock(txn, this);
        status = multiIndexBlock.init( indexSpecs );
        if ( !status.isOK() )
            return StatusWith<CompactStats>( status );

        MyCompactAdaptor adaptor(this, &multiIndexBlock);

        _recordStore->compact( txn, &adaptor, compactOptions, &stats );

        log() << "starting index commits";
        status = multiIndexBlock.commit();
        if ( !status.isOK() )
            return StatusWith<CompactStats>( status );

        return StatusWith<CompactStats>( stats );
    }

}  // namespace mongo
