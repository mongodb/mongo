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

#include "mongo/db/index_update.h"

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
    static void addKeysToIndex(const char *ns, NamespaceDetails *d, int idxNo, const BSONObj& obj,
                               const DiskLoc &recordLoc, bool dupsAllowed) {
        IndexDetails& id = d->idx(idxNo);
        auto_ptr<IndexDescriptor> desc(CatalogHack::getDescriptor(d, idxNo));
        auto_ptr<IndexAccessMethod> iam(CatalogHack::getIndex(desc.get()));
        InsertDeleteOptions options;
        options.logIfError = false;
        options.dupsAllowed = (!KeyPattern::isIdKeyPattern(id.keyPattern()) && !id.unique())
            || ignoreUniqueIndex(id);

        int64_t inserted;
        Status ret = iam->insert(obj, recordLoc, options, &inserted);
        if (Status::OK() != ret) {
            uasserted(ret.location(), ret.reason());
        }
    }

    //
    // Bulk index building
    //

    class BackgroundIndexBuildJob : public BackgroundOperation {

        unsigned long long addExistingToIndex(const char *ns, NamespaceDetails *d,
                                              IndexDetails& idx) {
            bool dupsAllowed = !idx.unique();
            bool dropDups = idx.dropDups();

            ProgressMeter& progress = cc().curop()->setMessage("bg index build",
                                                               "Background Index Build Progress",
                                                               d->numRecords());

            unsigned long long n = 0;
            unsigned long long numDropped = 0;

            auto_ptr<Runner> runner(InternalPlanner::collectionScan(ns));
            // We're not delegating yielding to the runner because we need to know when a yield
            // happens.
            RunnerYieldPolicy yieldPolicy;

            std::string idxName = idx.indexName();
            int idxNo = d->findIndexByName( idxName, true );
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
                        addKeysToIndex(ns, d, idxNo, js, loc, dupsAllowed);
                    }
                    else {
                        addKeysToIndex(ns, d, idxNo, js, loc, dupsAllowed);
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
                        theDataFileMgr.deleteRecord(d, ns, loc.rec(), loc, false, true, true);
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

                    progress.setTotalWhileRunning( d->numRecords() );
                    // Recalculate idxNo if we yielded
                    idxNo = d->findIndexByName( idxName, true );
                    verify( idxNo >= 0 );
                }
            }

            progress.finished();
            if ( dropDups )
                log() << "\t backgroundIndexBuild dupsToDrop: " << numDropped << endl;
            return n;
        }

        /* we do set a flag in the namespace for quick checking, but this is our authoritative info -
           that way on a crash/restart, we don't think we are still building one. */
        set<NamespaceDetails*> bgJobsInProgress;

        void prep(const char *ns, NamespaceDetails *d) {
            Lock::assertWriteLocked(ns);
            uassert( 13130 , "can't start bg index b/c in recursive lock (db.eval?)" , !Lock::nested() );
            bgJobsInProgress.insert(d);
        }
        void done(const char *ns) {
            Lock::assertWriteLocked(ns);

            // clear query optimizer cache
            Collection* collection = cc().database()->getCollection( ns );
            if ( collection )
                collection->infoCache()->addedIndex();
        }

    public:
        BackgroundIndexBuildJob(const char *ns) : BackgroundOperation(ns) { }

        unsigned long long go(string ns, NamespaceDetails *d, IndexDetails& idx) {

            // clear cached things since we are changing state
            // namely what fields are indexed
            Collection* collection = cc().database()->getCollection( ns );
            if ( collection )
                collection->infoCache()->addedIndex();

            unsigned long long n = 0;

            prep(ns.c_str(), d);
            try {
                idx.head.writing() = BtreeBasedBuilder::makeEmptyIndex(idx);
                n = addExistingToIndex(ns.c_str(), d, idx);
                // idx may point at an invalid index entry at this point
            }
            catch(...) {
                if( cc().database() && nsdetails(ns) == d ) {
                    done(ns.c_str());
                }
                else {
                    log() << "ERROR: db gone during bg index?" << endl;
                }
                throw;
            }
            done(ns.c_str());
            return n;
        }
    };

    // throws DBException
    void buildAnIndex(const std::string& ns,
                      NamespaceDetails* d,
                      IndexDetails& idx,
                      bool mayInterrupt) {

        BSONObj idxInfo = idx.info.obj();

        MONGO_TLOG(0) << "build index on: " << ns << " properties: " << idxInfo.jsonString() << endl;

        audit::logCreateIndex( currentClient.get(), &idxInfo, idx.indexName(), ns );

        Timer t;
        unsigned long long n;

        verify( Lock::isWriteLocked(ns) );

        if( inDBRepair || !idxInfo["background"].trueValue() ) {
            int idxNo = d->findIndexByName( idx.info.obj()["name"].valuestr(), true );
            verify( idxNo >= 0 );
            n = BtreeBasedBuilder::fastBuildIndex(ns.c_str(), d, idx, mayInterrupt, idxNo);
            verify( !idx.head.isNull() );
        }
        else {
            BackgroundIndexBuildJob j(ns.c_str());
            n = j.go(ns, d, idx);
        }
        MONGO_TLOG(0) << "build index done.  scanned " << n << " total records. " << t.millis() / 1000.0 << " secs" << endl;
    }

    extern BSONObj id_obj;  // { _id : 1 }

}  // namespace mongo

