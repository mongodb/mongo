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
*/

#include "mongo/pch.h"

#include "mongo/db/prefetch.h"

#include "mongo/db/dbhelpers.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/index.h"
#include "mongo/db/index_update.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_details.h"

namespace mongo {

    // prefetch for an oplog operation
    void prefetchPagesForReplicatedOp(const BSONObj& op) {
        const char *opField;
        const char *opType = op.getStringField("op");
        if ( *opType == 'i' )
            opField = "o";
        else if( *opType == 'u' )
            opField = "o2";
        else
            // prefetch ignores other ops
            return;
         
        BSONObj obj = op.getObjectField(opField);
        const char *ns = op.getStringField("ns");

        prefetchIndexPages(ns, obj);

        // do not prefetch the data for inserts; it doesn't exist yet
        if (*opType == 'u') {
            prefetchRecordPages(ns, obj);
        }
    }

    void prefetchIndexPages(const char *ns, const BSONObj& obj) {
        DiskLoc unusedDl; // unused
        IndexInterface::IndexInserter inserter;
        NamespaceDetails *nsd = nsdetails(ns);

        // includes all indexes, including ones
        // in the process of being built
        int indexCount = nsd->nIndexesBeingBuilt(); 
        BSONObjSet unusedKeys;
        //vector<int> multi;
        //vector<BSONObjSet> multiKeys;
        for ( int indexNo = 0; indexNo < indexCount; indexNo++ ) {
            // This will page in all index pages for the given object.
            fetchIndexInserters(/*out*/unusedKeys, inserter, nsd, indexNo, obj, unusedDl);
            // do something with multikeys later?
            // if( keys.size() > 1 ) {
            //     multi.push_back(i);
            //     multiKeys.push_back(BSONObjSet());
            //     multiKeys[multiKeys.size()-1].swap(keys);
            // }
            unusedKeys.clear();
        }
    }

    void prefetchRecordPages(const char* ns, const BSONObj& obj) {
        BSONElement _id;
        if( obj.getObjectID(_id) ) {
            BSONObjBuilder builder;
            builder.append(_id);
            BSONObj result;
            Client::ReadContext ctx( ns );
            try {
                if( Helpers::findById(cc(), ns, builder.done(), result) ) {
                    volatile char _dummy_char;       
                    // Touch the first word on every page in order to fault it into memory
                    for (int i = 0; i < result.objsize(); i += g_minOSPageSizeBytes) {                        
                        _dummy_char += *(result.objdata() + i); 
                    }
                    // hit the last page, in case we missed it above
                    _dummy_char += *(result.objdata() + result.objsize());
                }
            }
            catch( AssertionException& ) {
                log() << "ignoring assertion in prefetchRecord()" << endl;
            }
        }
    }
}
