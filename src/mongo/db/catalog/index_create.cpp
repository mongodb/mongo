// index_create.cpp

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

#include "mongo/db/catalog/index_create.h"

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/audit.h"
#include "mongo/db/background.h"
#include "mongo/db/btreebuilder.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/extsort.h"
#include "mongo/db/storage/index_details.h"
#include "mongo/db/index/btree_based_builder.h"
#include "mongo/db/index/catalog_hack.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/namespace_details.h"
#include "mongo/db/pdfile_private.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/runner_yield_policy.h"
#include "mongo/db/repl/is_master.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/structure/collection.h"
#include "mongo/util/processinfo.h"

namespace mongo {

    /**
     * Add the provided (obj, dl) pair to the provided index.
     */
    static void addKeysToIndex( Collection* collection, int idxNo,
                                const BSONObj& obj, const DiskLoc &recordLoc ) {

        IndexDetails& id = collection->details()->idx(idxNo);

        IndexDescriptor* desc = collection->getIndexCatalog()->getDescriptor( idxNo );
        verify( desc );

        IndexAccessMethod* iam = collection->getIndexCatalog()->getIndex( desc );
        verify( iam );

        InsertDeleteOptions options;
        options.logIfError = false;
        options.dupsAllowed = (!KeyPattern::isIdKeyPattern(id.keyPattern()) && !id.unique())
            || ignoreUniqueIndex(id);

        int64_t inserted;
        Status ret = iam->insert(obj, recordLoc, options, &inserted);
        uassertStatusOK( ret );
    }

    //
    // Bulk index building
    //

    class BackgroundIndexBuildJob : public BackgroundOperation {

    public:
        BackgroundIndexBuildJob(const StringData& ns)
            : BackgroundOperation(ns) {
        }

        unsigned long long go( Collection* collection, IndexDetails& idx );

    private:
        unsigned long long addExistingToIndex( Collection* collection,
                                               IndexDetails& idx );

        void prep(const StringData& ns ) {
            Lock::assertWriteLocked(ns);
            uassert( 13130 , "can't start bg index b/c in recursive lock (db.eval?)" , !Lock::nested() );
        }
        void done(const StringData& ns) {
            Lock::assertWriteLocked(ns);

            // clear query optimizer cache
            Collection* collection = cc().database()->getCollection( ns );
            if ( collection )
                collection->infoCache()->addedIndex();
        }

    };

    unsigned long long BackgroundIndexBuildJob::go( Collection* collection, IndexDetails& idx) {

        string ns = collection->ns().ns();

        // clear cached things since we are changing state
        // namely what fields are indexed
        collection->infoCache()->addedIndex();

        prep( ns );

        try {
            idx.head.writing() = BtreeBasedBuilder::makeEmptyIndex( idx );
            unsigned long long n = addExistingToIndex( collection, idx );
            // idx may point at an invalid index entry at this point
            done( ns );
            return n;
        }
        catch (...) {
            done( ns );
            throw;
        }
    }

    unsigned long long BackgroundIndexBuildJob::addExistingToIndex( Collection* collection,
                                                                    IndexDetails& idx ) {

        string ns = collection->ns().ns(); // our copy for sanity

        bool dupsAllowed = !idx.unique();
        bool dropDups = idx.dropDups();

        ProgressMeter& progress = cc().curop()->setMessage("bg index build",
                                                           "Background Index Build Progress",
                                                           collection->numRecords());

        unsigned long long n = 0;
        unsigned long long numDropped = 0;

        auto_ptr<Runner> runner(InternalPlanner::collectionScan(ns));

        // We're not delegating yielding to the runner because we need to know when a yield
        // happens.
        RunnerYieldPolicy yieldPolicy;

        std::string idxName = idx.indexName();
        int idxNo = collection->details()->findIndexByName( idxName, true );
        verify( idxNo >= 0 );

        // After this yields in the loop, idx may point at a different index (if indexes get
        // flipped, see insert_makeIndex) or even an empty IndexDetails, so nothing below should
        // depend on idx. idxNo should be recalculated after each yield.

        BSONObj js;
        DiskLoc loc;
        while (Runner::RUNNER_ADVANCED == runner->getNext(&js, &loc)) {
            try {
                if ( !dupsAllowed && dropDups ) {
                    LastError::Disabled led( lastError.get() );
                    addKeysToIndex(collection, idxNo, js, loc);
                }
                else {
                    addKeysToIndex(collection, idxNo, js, loc);
                }
            }
            catch( AssertionException& e ) {
                if( e.interrupted() ) {
                    killCurrentOp.checkForInterrupt();
                }

                // TODO: Does exception really imply dropDups exception?
                if (dropDups) {
                    bool runnerEOF = runner->isEOF();
                    runner->saveState();
                    collection->deleteDocument( loc, false, true );
                    if (!runner->restoreState()) {
                        // Runner got killed somehow.  This probably shouldn't happen.
                        if (runnerEOF) {
                            // Quote: "We were already at the end.  Normal.
                            // TODO: Why is this normal?
                        }
                        else {
                            uasserted(12585, "cursor gone during bg index; dropDups");
                        }
                        break;
                    }
                    // We deleted a record, but we didn't actually yield the dblock.
                    // TODO: Why did the old code assume we yielded the lock?
                    numDropped++;
                }
                else {
                    log() << "background addExistingToIndex exception " << e.what() << endl;
                    throw;
                }
            }

            n++;
            progress.hit();

            getDur().commitIfNeeded();
            if (yieldPolicy.shouldYield()) {
                if (!yieldPolicy.yieldAndCheckIfOK(runner.get())) {
                    uasserted(12584, "cursor gone during bg index");
                    break;
                }

                progress.setTotalWhileRunning( collection->numRecords() );
                // Recalculate idxNo if we yielded
                idxNo = collection->details()->findIndexByName( idxName, true );
                verify( idxNo >= 0 );
            }
        }

        progress.finished();
        if ( dropDups )
            log() << "\t backgroundIndexBuild dupsToDrop: " << numDropped << endl;
        return n;
    }

    // ---------------------------

    // throws DBException
    void buildAnIndex( Collection* collection,
                       IndexDetails& idx,
                       bool mayInterrupt ) {

        string ns = collection->ns().ns(); // our copy

        BSONObj idxInfo = idx.info.obj();

        MONGO_TLOG(0) << "build index on: " << ns
                      << " properties: " << idxInfo.jsonString() << endl;

        audit::logCreateIndex( currentClient.get(), &idxInfo, idx.indexName(), ns );

        Timer t;
        unsigned long long n;

        verify( Lock::isWriteLocked( ns ) );

        if( inDBRepair || !idxInfo["background"].trueValue() ) {
            int idxNo = collection->details()->findIndexByName( idx.info.obj()["name"].valuestr(),
                                                                true );
            verify( idxNo >= 0 );
            n = BtreeBasedBuilder::fastBuildIndex( ns.c_str(), collection->details(),
                                                   idx, mayInterrupt, idxNo );
            verify( !idx.head.isNull() );
        }
        else {
            BackgroundIndexBuildJob j( ns );
            n = j.go( collection, idx );
        }
        MONGO_TLOG(0) << "build index done.  scanned " << n << " total records. " << t.millis() / 1000.0 << " secs" << endl;
    }

    extern BSONObj id_obj;  // { _id : 1 }

}  // namespace mongo

