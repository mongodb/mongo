/**
*    Copyright (C) 2008-2014 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/prefetch.h"

#include "mongo/db/dbhelpers.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/stats/timer_stats.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/util/log.h"

namespace mongo {

    // todo / idea: the prefetcher, when it fetches _id, on an upsert, will see if the record exists. if it does not, 
    //              at write time, we can just do an insert, which will be faster.

    //The count (of batches) and time spent fetching pages before application
    //    -- meaning depends on the prefetch behavior: all, _id index, none, etc.)
    static TimerStats prefetchIndexStats;
    static ServerStatusMetricField<TimerStats> displayPrefetchIndexPages(
                                                    "repl.preload.indexes",
                                                    &prefetchIndexStats );
    static TimerStats prefetchDocStats;
    static ServerStatusMetricField<TimerStats> displayPrefetchDocPages(
                                                    "repl.preload.docs",
                                                    &prefetchDocStats );

    void prefetchIndexPages(OperationContext* txn,
                            Collection* collection,
                            const repl::ReplSetImpl::IndexPrefetchConfig& prefetchConfig,
                            const BSONObj& obj);

    void prefetchRecordPages(OperationContext* txn, const char* ns, const BSONObj& obj);


    // prefetch for an oplog operation
    void prefetchPagesForReplicatedOp(OperationContext* txn,
                                      Database* db,
                                      const repl::ReplSetImpl::IndexPrefetchConfig& prefetchConfig,
                                      const BSONObj& op) {
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

        Collection* collection = db->getCollection( txn, ns );
        if ( !collection )
            return;

        LOG(4) << "index prefetch for op " << *opType << endl;

        DEV txn->lockState()->assertAtLeastReadLocked(ns);

        // should we prefetch index pages on updates? if the update is in-place and doesn't change 
        // indexed values, it is actually slower - a lot slower if there are a dozen indexes or 
        // lots of multikeys.  possible variations (not all mutually exclusive):
        //  1) current behavior: full prefetch
        //  2) don't do it for updates
        //  3) don't do multikey indexes for updates
        //  4) don't prefetchIndexPages on some heuristic; e.g., if it's an $inc.
        //  5) if not prefetching index pages (#2), we should do it if we are upsertings and it 
        //     will be an insert. to do that we could do the prefetchRecordPage first and if DNE
        //     then we do #1.
        // 
        // note that on deletes 'obj' does not have all the keys we would want to prefetch on.
        // a way to achieve that would be to prefetch the record first, and then afterwards do 
        // this part.
        //
        prefetchIndexPages(txn, collection, prefetchConfig, obj);

        // do not prefetch the data for inserts; it doesn't exist yet
        // 
        // we should consider doing the record prefetch for the delete op case as we hit the record
        // when we delete.  note if done we only want to touch the first page.
        // 
        // update: do record prefetch. 
        if ((*opType == 'u') &&
            // do not prefetch the data for capped collections because
            // they typically do not have an _id index for findById() to use.
            !collection->isCapped()) {
            prefetchRecordPages(txn, ns, obj);
        }
    }

    // page in pages needed for all index lookups on a given object
    void prefetchIndexPages(OperationContext* txn,
                            Collection* collection,
                            const repl::ReplSetImpl::IndexPrefetchConfig& prefetchConfig,
                            const BSONObj& obj) {
        DiskLoc unusedDl; // unused
        BSONObjSet unusedKeys;

        // do we want prefetchConfig to be (1) as-is, (2) for update ops only, or (3) configured per op type?  
        // One might want PREFETCH_NONE for updates, but it's more rare that it is a bad idea for inserts.  
        // #3 (per op), a big issue would be "too many knobs".
        switch (prefetchConfig) {
        case repl::ReplSetImpl::PREFETCH_NONE:
            return;
        case repl::ReplSetImpl::PREFETCH_ID_ONLY:
        {
            TimerHolder timer( &prefetchIndexStats);
            // on the update op case, the call to prefetchRecordPages will touch the _id index.
            // thus perhaps this option isn't very useful?
            try {
                IndexDescriptor* desc = collection->getIndexCatalog()->findIdIndex(txn);
                if ( !desc )
                    return;
                IndexAccessMethod* iam = collection->getIndexCatalog()->getIndex( desc );
                invariant( iam );
                iam->touch(txn, obj);
            }
            catch (const DBException& e) {
                LOG(2) << "ignoring exception in prefetchIndexPages(): " << e.what() << endl;
            }
            break;
        }
        case repl::ReplSetImpl::PREFETCH_ALL:
        {
            // indexCount includes all indexes, including ones
            // in the process of being built
            IndexCatalog::IndexIterator ii =
                collection->getIndexCatalog()->getIndexIterator( txn, true );
            while ( ii.more() ) {
                TimerHolder timer( &prefetchIndexStats);
                // This will page in all index pages for the given object.
                try {
                    IndexDescriptor* desc = ii.next();
                    IndexAccessMethod* iam = collection->getIndexCatalog()->getIndex( desc );
                    verify( iam );
                    iam->touch(txn, obj);
                }
                catch (const DBException& e) {
                    LOG(2) << "ignoring exception in prefetchIndexPages(): " << e.what() << endl;
                }
                unusedKeys.clear();
            }
            break;
        }
        default:
            fassertFailed(16427);
        }
    }

    // page in the data pages for a record associated with an object
    void prefetchRecordPages(OperationContext* txn, const char* ns, const BSONObj& obj) {
        BSONElement _id;
        if( obj.getObjectID(_id) ) {
            TimerHolder timer(&prefetchDocStats);
            BSONObjBuilder builder;
            builder.append(_id);
            BSONObj result;
            try {
                // we can probably use Client::Context here instead of ReadContext as we
                // have locked higher up the call stack already
                Client::ReadContext ctx(txn, ns);
                if( Helpers::findById(txn, ctx.ctx().db(), ns, builder.done(), result) ) {
                    // do we want to use Record::touch() here?  it's pretty similar.
                    volatile char _dummy_char = '\0';
                    // Touch the first word on every page in order to fault it into memory
                    for (int i = 0; i < result.objsize(); i += g_minOSPageSizeBytes) {
                        _dummy_char += *(result.objdata() + i);
                    }
                    // hit the last page, in case we missed it above
                    _dummy_char += *(result.objdata() + result.objsize() - 1);
                }
            }
            catch(const DBException& e) {
                LOG(2) << "ignoring exception in prefetchRecordPages(): " << e.what() << endl;
            }
        }
    }
}
