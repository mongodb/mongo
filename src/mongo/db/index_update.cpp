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
#include "mongo/db/background.h"
#include "mongo/db/btreebuilder.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/extsort.h"
#include "mongo/db/index.h"
#include "mongo/db/index/btree_based_builder.h"
#include "mongo/db/index/catalog_hack.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/namespace_details.h"
#include "mongo/db/pdfile_private.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/runner_yield_policy.h"
#include "mongo/db/repl/is_master.h"
#include "mongo/db/repl/rs.h"
#include "mongo/util/processinfo.h"

namespace mongo {
    
    /**
     * Remove the provided (obj, dl) pair from the provided index.
     */
    static void _unindexRecord(NamespaceDetails *d, int idxNo, const BSONObj& obj,
                               const DiskLoc& dl, bool logIfError = true) {
        auto_ptr<IndexDescriptor> desc(CatalogHack::getDescriptor(d, idxNo));
        auto_ptr<IndexAccessMethod> iam(CatalogHack::getIndex(desc.get()));
        InsertDeleteOptions options;
        options.logIfError = logIfError;

        int64_t removed;
        Status ret = iam->remove(obj, dl, options, &removed);
        if (Status::OK() != ret) {
            problem() << "Couldn't unindex record " << obj.toString() << " status: "
                << ret.toString() << endl;
        }
    }

    /**
     * Remove the provided (obj, dl) pair from all indices.
     */
    void unindexRecord(NamespaceDetails* nsd, Record* todelete, const DiskLoc& dl,
                       bool noWarn /* = false */) {

        BSONObj obj = BSONObj::make(todelete);
        int numIndices = nsd->getTotalIndexCount();

        for (int i = 0; i < numIndices; i++) {
            // If i >= d->nIndexes, it's a background index, and we DO NOT want to log anything.
            bool logIfError = (i < nsd->getCompletedIndexCount()) ? !noWarn : false;
            _unindexRecord(nsd, i, obj, dl, logIfError);
        }
    }

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

    /**
     * Add the provided (obj, loc) pair to all indices.
     */
    void indexRecord(const char *ns, NamespaceDetails *d, const BSONObj &obj, const DiskLoc &loc) {
        int numIndices = d->getTotalIndexCount();

        for (int i = 0; i < numIndices; ++i) {
            IndexDetails &id = d->idx(i);

            try {
                addKeysToIndex(ns, d, i, obj, loc, !id.unique() || ignoreUniqueIndex(id));
            }
            catch (AssertionException&) {
                // TODO: the new index layer indexes either all or no keys, so j <= i can be j < i.
                for (int j = 0; j <= i; j++) {
                    try {
                        _unindexRecord(d, j, obj, loc, false);
                    }
                    catch(...) {
                        LOG(3) << "unindex fails on rollback after unique "
                                  "key constraint prevented insert" << std::endl;
                    }
                }
                throw;
            }
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
            int idxNo = IndexBuildsInProgress::get(ns, idxName);

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
                    idxNo = IndexBuildsInProgress::get(ns, idxName);
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
            NamespaceDetailsTransient::get(ns).addedIndex(); // clear query optimizer cache
            Lock::assertWriteLocked(ns);
        }

    public:
        BackgroundIndexBuildJob(const char *ns) : BackgroundOperation(ns) { }

        unsigned long long go(string ns, NamespaceDetails *d, IndexDetails& idx) {

            // clear cached things since we are changing state
            // namely what fields are indexed
            NamespaceDetailsTransient::get(ns.c_str()).addedIndex();

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

        Timer t;
        unsigned long long n;

        verify( Lock::isWriteLocked(ns) );

        if( inDBRepair || !idxInfo["background"].trueValue() ) {
            int idxNo = IndexBuildsInProgress::get(ns.c_str(), idx.info.obj()["name"].valuestr());
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

    void ensureHaveIdIndex(const char* ns, bool mayInterrupt) {
        NamespaceDetails *d = nsdetails(ns);
        if ( d == 0 || d->isSystemFlagSet(NamespaceDetails::Flag_HaveIdIndex) )
            return;

        d->setSystemFlag( NamespaceDetails::Flag_HaveIdIndex );

        {
            NamespaceDetails::IndexIterator i = d->ii();
            while( i.more() ) {
                if( i.next().isIdIndex() )
                    return;
            }
        }

        string system_indexes = cc().database()->name() + ".system.indexes";

        BSONObjBuilder b;
        b.append("name", "_id_");
        b.append("ns", ns);
        b.append("key", id_obj);
        BSONObj o = b.done();

        /* edge case: note the insert could fail if we have hit maxindexes already */
        theDataFileMgr.insert(system_indexes.c_str(), o.objdata(), o.objsize(), mayInterrupt, true);
    }

    bool dropIndexes(NamespaceDetails *d, const char *ns, const char *name, string &errmsg, BSONObjBuilder &anObjBuilder, bool mayDeleteIdIndex) {
        BackgroundOperation::assertNoBgOpInProgForNs(ns);

        /* there may be pointers pointing at keys in the btree(s).  kill them. */
        ClientCursor::invalidate(ns);

        // delete a specific index or all?
        if ( *name == '*' && name[1] == 0 ) {
            // this should be covered by assertNoBgOpInProgForNs above, but being paranoid
            verify( d->getCompletedIndexCount() == d->getTotalIndexCount() );

            LOG(4) << "  d->nIndexes was " << d->getCompletedIndexCount() << std::endl;
            anObjBuilder.appendNumber("nIndexesWas", d->getCompletedIndexCount() );
            IndexDetails *idIndex = 0;

            for ( int i = 0; i < d->getCompletedIndexCount(); i++ ) {

                if ( !mayDeleteIdIndex && d->idx(i).isIdIndex() ) {
                    idIndex = &d->idx(i);
                    continue;
                }

                d->removeIndex( i );
                i--;
            }

            if ( idIndex ) {
                verify( d->getCompletedIndexCount() == 1 );
            }

            //verify( 0 == assureSysIndexesEmptied(ns, idIndex) );// TODO(erh)
            assureSysIndexesEmptied(ns, idIndex);
            anObjBuilder.append("msg", mayDeleteIdIndex ?
                                "indexes dropped for collection" :
                                "non-_id indexes dropped for collection");
        }
        else {
            // delete just one index
            int x = d->findIndexByName(name);
            if ( x >= 0 ) {

                if ( !mayDeleteIdIndex && d->idx(x).isIdIndex() ) {
                    errmsg = "may not delete _id index";
                    return false;
                }

                LOG(4) << "  d->nIndexes was " << d->getCompletedIndexCount() << endl;
                anObjBuilder.appendNumber("nIndexesWas", d->getCompletedIndexCount() );

                d->removeIndex( x );
            }
            else {
                int n = removeFromSysIndexes(ns, name); // just in case an orphaned listing there - i.e. should have been repaired but wasn't
                if( n ) {
                    log() << "info: removeFromSysIndexes cleaned up " << n << " entries" << endl;
                }
                log() << "dropIndexes: " << name << " not found" << endl;
                errmsg = "index not found";
                return false;
            }
        }
        return true;
    }

}  // namespace mongo
