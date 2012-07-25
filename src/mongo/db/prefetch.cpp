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
        switch (*opType) {
        case 'i': // insert
        case 'd': // delete
            opField = "o";
            break;
        case 'u': // update
            opField = "o2";
            break;
        default:
            // prefetch ignores other ops
            return;
        }
         
        BSONObj obj = op.getObjectField(opField);
        const char *ns = op.getStringField("ns");
        NamespaceDetails *nsd = nsdetails(ns);
        if (!nsd) return; // maybe not opened yet

        log(4) << "index prefetch for op " << *opType << endl;
        prefetchIndexPages(nsd, obj);

        // do not prefetch the data for inserts; it doesn't exist yet
        if ((*opType == 'u') &&
            // do not prefetch the data for capped collections because
            // they typically do not have an _id index for findById() to use.
            !nsd->isCapped()) {
            prefetchRecordPages(ns, obj);
        }
    }

    void prefetchIndexPages(NamespaceDetails *nsd, const BSONObj& obj) {
        DiskLoc unusedDl; // unused
        IndexInterface::IndexInserter inserter;

        // includes all indexes, including ones
        // in the process of being built
        int indexCount = nsd->nIndexesBeingBuilt(); 
        BSONObjSet unusedKeys;
        for ( int indexNo = 0; indexNo < indexCount; indexNo++ ) {
            // This will page in all index pages for the given object.
            try {
                fetchIndexInserters(/*out*/unusedKeys, inserter, nsd, indexNo, obj, unusedDl, /*allowDups*/true);
            }
            catch (const DBException& e) {
                LOG(2) << "ignoring exception in prefetchIndexPages(): " << e.what() << endl;
            }
            unusedKeys.clear();
        }
    }

    void prefetchRecordPages(const char* ns, const BSONObj& obj) {
        BSONElement _id;
        if( obj.getObjectID(_id) ) {
            BSONObjBuilder builder;
            builder.append(_id);
            BSONObj result;
            try {
                Client::ReadContext ctx( ns );
                if( Helpers::findById(cc(), ns, builder.done(), result) ) {
                    volatile char _dummy_char = '\0';
                    // Touch the first word on every page in order to fault it into memory
                    for (int i = 0; i < result.objsize(); i += g_minOSPageSizeBytes) {                        
                        _dummy_char += *(result.objdata() + i); 
                    }
                    // hit the last page, in case we missed it above
                    _dummy_char += *(result.objdata() + result.objsize());
                }
            }
            catch(const DBException& e) {
                LOG(2) << "ignoring exception in prefetchRecordPages(): " << e.what() << endl;
            }
        }
    }
}
