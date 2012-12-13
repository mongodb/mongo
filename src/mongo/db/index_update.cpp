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

#include "mongo/db/index_update.h"

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/background.h"
#include "mongo/db/btreebuilder.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/extsort.h"
#include "mongo/db/index.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/namespace_details.h"
#include "mongo/db/pdfile_private.h"
#include "mongo/db/replutil.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/sort_phase_one.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/startup_test.h"

namespace mongo {
    
    /* unindex all keys in index for this record. */
    static void _unindexRecord(IndexDetails& id, BSONObj& obj, const DiskLoc& dl, bool logMissing = true) {
        BSONObjSet keys;
        id.getKeysFromObject(obj, keys);
        IndexInterface& ii = id.idxInterface();
        for ( BSONObjSet::iterator i=keys.begin(); i != keys.end(); i++ ) {
            BSONObj j = *i;

            bool ok = false;
            try {
                ok = ii.unindex(id.head, id, j, dl);
            }
            catch (AssertionException& e) {
                problem() << "Assertion failure: _unindex failed " << id.indexNamespace() << endl;
                out() << "Assertion failure: _unindex failed: " << e.what() << '\n';
                out() << "  obj:" << obj.toString() << '\n';
                out() << "  key:" << j.toString() << '\n';
                out() << "  dl:" << dl.toString() << endl;
                logContext();
            }

            if ( !ok && logMissing ) {
                log() << "unindex failed (key too big?) " << id.indexNamespace() << " key: " << j << " " << obj["_id"] << endl;
            }
        }
    }

//zzz
    /* unindex all keys in all indexes for this record. */
    void unindexRecord(NamespaceDetails *d, 
                       Record *todelete, 
                       const DiskLoc& dl, 
                       bool noWarn /* = false */) {
        BSONObj obj = BSONObj::make(todelete);
        int n = d->nIndexes;
        for ( int i = 0; i < n; i++ )
            _unindexRecord(d->idx(i), obj, dl, !noWarn);

        for (int i = 0; i < d->indexBuildsInProgress; i++) { // background index
            // Always pass nowarn here, as this one may be missing for valid reasons as we are
            // concurrently building it
            _unindexRecord(d->idx(n+i), obj, dl, false);
        }
    }

    /* step one of adding keys to index idxNo for a new record
    */
    void fetchIndexInserters(BSONObjSet & /*out*/keys,
                             IndexInterface::IndexInserter &inserter,
                             NamespaceDetails *d,
                             int idxNo,
                             const BSONObj& obj,
                             DiskLoc recordLoc,
                             const bool allowDups) {
        IndexDetails &idx = d->idx(idxNo);
        idx.getKeysFromObject(obj, keys);
        if( keys.empty() )
            return;
        bool dupsAllowed = !idx.unique() || allowDups;
        Ordering ordering = Ordering::make(idx.keyPattern());
        
        try {
            // we can't do the two step method with multi keys as insertion of one key changes the indexes 
            // structure.  however we can do the first key of the set so we go ahead and do that FWIW
            inserter.addInsertionContinuation(
                    idx.idxInterface().beginInsertIntoIndex(
                            idxNo, idx, recordLoc, *keys.begin(), ordering, dupsAllowed));
        }
        catch (AssertionException& e) {
            if( e.getCode() == 10287 && idxNo >= d->nIndexes ) {
                DEV log() << "info: caught key already in index on bg indexing (ok)" << endl;
            }
            else {
                throw;
            }
        }
    }

    /** add index keys for a newly inserted record 
        done in two steps/phases to allow potential deferal of write lock portion in the future
    */
    void indexRecordUsingTwoSteps(const char *ns, NamespaceDetails *d, BSONObj obj,
                                         DiskLoc loc, bool shouldBeUnlocked) {
        vector<int> multi;
        vector<BSONObjSet> multiKeys;

        IndexInterface::IndexInserter inserter;

        // Step 1, read phase.
        int n = d->getTotalIndexCount();
        {
            BSONObjSet keys;
            for ( int i = 0; i < n; i++ ) {
                // this call throws on unique constraint violation.  we haven't done any writes yet so that is fine.
                fetchIndexInserters(/*out*/keys, 
                                    inserter, 
                                    d, 
                                    i, 
                                    obj, 
                                    loc, 
                                    ignoreUniqueIndex(d->idx(i)));
                if( keys.size() > 1 ) {
                    multi.push_back(i);
                    multiKeys.push_back(BSONObjSet());
                    multiKeys[multiKeys.size()-1].swap(keys);
                }
                keys.clear();
            }
        }

        inserter.finishAllInsertions();  // Step 2, write phase.

        // now finish adding multikeys
        for( unsigned j = 0; j < multi.size(); j++ ) {
            unsigned i = multi[j];
            BSONObjSet& keys = multiKeys[j];
            IndexDetails& idx = d->idx(i);
            bool dupsAllowed = !idx.unique() || ignoreUniqueIndex(idx);
            IndexInterface& ii = idx.idxInterface();
            Ordering ordering = Ordering::make(idx.keyPattern());
            d->setIndexIsMultikey(ns, i);
            for( BSONObjSet::iterator k = ++keys.begin()/*skip 1*/; k != keys.end(); k++ ) {
                try {
                    ii.bt_insert(idx.head, loc, *k, ordering, dupsAllowed, idx);
                } catch (AssertionException& e) {
                    if( e.getCode() == 10287 && (int) i >= d->nIndexes ) {
                        DEV log() << "info: caught key already in index on bg indexing (ok)" << endl;
                    }
                    else {
                        /* roll back previously added index entries
                           note must do self index as it is multikey and could require some cleanup itself
                        */
                        for( int j = 0; j < n; j++ ) {
                            try {
                                _unindexRecord(d->idx(j), obj, loc, false);
                            }
                            catch(...) {
                                LOG(3) << "unindex fails on rollback after unique key constraint prevented insert\n";
                            }
                        }
                        throw;
                    }
                }
            }
        }
    }

    /* add keys to index idxNo for a new record */
    static void addKeysToIndex(const char *ns, NamespaceDetails *d, int idxNo, BSONObj& obj,
                               DiskLoc recordLoc, bool dupsAllowed) {
        IndexDetails& idx = d->idx(idxNo);
        BSONObjSet keys;
        idx.getKeysFromObject(obj, keys);
        if( keys.empty() ) 
            return;
        BSONObj order = idx.keyPattern();
        IndexInterface& ii = idx.idxInterface();
        Ordering ordering = Ordering::make(order);
        int n = 0;
        for ( BSONObjSet::iterator i=keys.begin(); i != keys.end(); i++ ) {
            if( ++n == 2 ) {
                d->setIndexIsMultikey(ns, idxNo);
            }
            verify( !recordLoc.isNull() );
            try {
                ii.bt_insert(idx.head, recordLoc, *i, ordering, dupsAllowed, idx);
            }
            catch (AssertionException& e) {
                if( e.getCode() == 10287 && idxNo >= d->nIndexes ) {
                    DEV log() << "info: caught key already in index on bg indexing (ok)" << endl;
                    continue;
                }
                if( !dupsAllowed ) {
                    // dup key exception, presumably.
                    throw;
                }
                problem() << " caught assertion addKeysToIndex " << idx.indexNamespace() << " " << obj["_id"] << endl;
            }
        }
    }

    void addKeysToPhaseOne( const char* ns,
                            const IndexDetails& idx,
                            const BSONObj& order,
                            SortPhaseOne* phaseOne,
                            int64_t nrecords,
                            ProgressMeter* progressMeter,
                            bool mayInterrupt ) {
        shared_ptr<Cursor> cursor = theDataFileMgr.findAll( ns );
        phaseOne->sorter.reset( new BSONObjExternalSorter( idx.idxInterface(), order ) );
        phaseOne->sorter->hintNumObjects( nrecords );
        const IndexSpec& spec = idx.getSpec();
        while ( cursor->ok() ) {
            RARELY killCurrentOp.checkForInterrupt( !mayInterrupt );
            BSONObj o = cursor->current();
            DiskLoc loc = cursor->currLoc();
            phaseOne->addKeys( spec, o, loc, mayInterrupt );
            cursor->advance();
            progressMeter->hit();
            if ( logLevel > 1 && phaseOne->n % 10000 == 0 ) {
                printMemInfo( "\t iterating objects" );
            }
        }
    }

    template< class V >
    void buildBottomUpPhases2And3( bool dupsAllowed,
                                   IndexDetails& idx,
                                   BSONObjExternalSorter& sorter,
                                   bool dropDups,
                                   set<DiskLoc>& dupsToDrop,
                                   CurOp* op,
                                   SortPhaseOne* phase1,
                                   ProgressMeterHolder& pm,
                                   Timer& t,
                                   bool mayInterrupt ) {
        BtreeBuilder<V> btBuilder(dupsAllowed, idx);
        BSONObj keyLast;
        auto_ptr<BSONObjExternalSorter::Iterator> i = sorter.iterator();
        verify( pm == op->setMessage( "index: (2/3) btree bottom up" , phase1->nkeys , 10 ) );
        while( i->more() ) {
            RARELY killCurrentOp.checkForInterrupt( !mayInterrupt );
            BSONObjExternalSorter::Data d = i->next();

            try {
                if ( !dupsAllowed && dropDups ) {
                    LastError::Disabled led( lastError.get() );
                    btBuilder.addKey(d.first, d.second);
                }
                else {
                    btBuilder.addKey(d.first, d.second);                    
                }
            }
            catch( AssertionException& e ) {
                if ( dupsAllowed ) {
                    // unknown exception??
                    throw;
                }

                if( e.interrupted() ) {
                    killCurrentOp.checkForInterrupt();
                }

                if ( ! dropDups )
                    throw;

                /* we could queue these on disk, but normally there are very few dups, so instead we
                    keep in ram and have a limit.
                */
                dupsToDrop.insert(d.second);
                uassert( 10092 , "too may dups on index build with dropDups=true", dupsToDrop.size() < 1000000 );
            }
            pm.hit();
        }
        pm.finished();
        op->setMessage( "index: (3/3) btree-middle" );
        LOG(t.seconds() > 10 ? 0 : 1 ) << "\t done building bottom layer, going to commit" << endl;
        btBuilder.commit( mayInterrupt );
        if ( btBuilder.getn() != phase1->nkeys && ! dropDups ) {
            warning() << "not all entries were added to the index, probably some keys were too large" << endl;
        }
    }

    void doDropDups( const char* ns,
                     NamespaceDetails* d,
                     const set<DiskLoc>& dupsToDrop,
                     bool mayInterrupt ) {
        for( set<DiskLoc>::const_iterator i = dupsToDrop.begin(); i != dupsToDrop.end(); ++i ) {
            RARELY killCurrentOp.checkForInterrupt( !mayInterrupt );
            theDataFileMgr.deleteRecord( d,
                                         ns,
                                         i->rec(),
                                         *i,
                                         false /* cappedOk */,
                                         true /* noWarn */,
                                         isMaster( ns ) /* logOp */ );
            getDur().commitIfNeeded();
        }
    }

    // throws DBException
    uint64_t fastBuildIndex(const char* ns,
                            NamespaceDetails* d,
                            IndexDetails& idx,
                            bool mayInterrupt) {
        CurOp * op = cc().curop();

        Timer t;

        tlog(1) << "fastBuildIndex " << ns << ' ' << idx.info.obj().toString() << endl;

        bool dupsAllowed = !idx.unique() || ignoreUniqueIndex(idx);
        bool dropDups = idx.dropDups() || inDBRepair;
        BSONObj order = idx.keyPattern();

        getDur().writingDiskLoc(idx.head).Null();

        if ( logLevel > 1 ) printMemInfo( "before index start" );

        /* get and sort all the keys ----- */
        ProgressMeterHolder pm( op->setMessage( "index: (1/3) external sort" , d->stats.nrecords , 10 ) );
        SortPhaseOne _ours;
        SortPhaseOne *phase1 = theDataFileMgr.getPrecalced();
        if( phase1 == 0 ) {
            phase1 = &_ours;
            addKeysToPhaseOne( ns, idx, order, phase1, d->stats.nrecords, pm.get(), mayInterrupt );
        }
        pm.finished();

        BSONObjExternalSorter& sorter = *(phase1->sorter);
        // Ensure the index and external sorter have a consistent index interface (and sort order).
        fassert( 16408, &idx.idxInterface() == &sorter.getIndexInterface() );

        if( phase1->multi ) {
            int idxNo = IndexBuildsInProgress::get(ns, idx.info.obj()["name"].valuestr());
            d->setIndexIsMultikey(ns, idxNo);
        }

        if ( logLevel > 1 ) printMemInfo( "before final sort" );
        phase1->sorter->sort( mayInterrupt );
        if ( logLevel > 1 ) printMemInfo( "after final sort" );

        LOG(t.seconds() > 5 ? 0 : 1) << "\t external sort used : " << sorter.numFiles() << " files " << " in " << t.seconds() << " secs" << endl;

        set<DiskLoc> dupsToDrop;

        /* build index --- */
        if( idx.version() == 0 )
            buildBottomUpPhases2And3<V0>(dupsAllowed,
                                         idx,
                                         sorter,
                                         dropDups,
                                         dupsToDrop,
                                         op,
                                         phase1,
                                         pm,
                                         t,
                                         mayInterrupt);
        else if( idx.version() == 1 ) 
            buildBottomUpPhases2And3<V1>(dupsAllowed,
                                         idx,
                                         sorter,
                                         dropDups,
                                         dupsToDrop,
                                         op,
                                         phase1,
                                         pm,
                                         t,
                                         mayInterrupt);
        else
            verify(false);

        if( dropDups ) 
            log() << "\t fastBuildIndex dupsToDrop:" << dupsToDrop.size() << endl;

        doDropDups(ns, d, dupsToDrop, mayInterrupt);

        return phase1->n;
    }

    class BackgroundIndexBuildJob : public BackgroundOperation {

        unsigned long long addExistingToIndex(const char *ns, NamespaceDetails *d,
                                              IndexDetails& idx) {
            bool dupsAllowed = !idx.unique();
            bool dropDups = idx.dropDups();

            ProgressMeter& progress = cc().curop()->setMessage( "bg index build" , d->stats.nrecords );

            unsigned long long n = 0;
            unsigned long long numDropped = 0;
            auto_ptr<ClientCursor> cc;
            {
                shared_ptr<Cursor> c = theDataFileMgr.findAll(ns);
                cc.reset( new ClientCursor(QueryOption_NoCursorTimeout, c, ns) );
            }

            std::string idxName = idx.indexName();
            int idxNo = IndexBuildsInProgress::get(ns, idxName);
            massert(16574, "Couldn't find index being built", idxNo != -1);

            // After this yields in the loop, idx may point at a different index (if indexes get
            // flipped, see insert_makeIndex) or even an empty IndexDetails, so nothing below should
            // depend on idx. idxNo should be recalculated after each yield.

            while ( cc->ok() ) {
                BSONObj js = cc->current();
                try {
                    {
                        if ( !dupsAllowed && dropDups ) {
                            LastError::Disabled led( lastError.get() );
                            addKeysToIndex(ns, d, idxNo, js, cc->currLoc(), dupsAllowed);
                        }
                        else {
                            addKeysToIndex(ns, d, idxNo, js, cc->currLoc(), dupsAllowed);
                        }
                    }
                    cc->advance();
                }
                catch( AssertionException& e ) {
                    if( e.interrupted() ) {
                        killCurrentOp.checkForInterrupt();
                    }

                    if ( dropDups ) {
                        DiskLoc toDelete = cc->currLoc();
                        bool ok = cc->advance();
                        ClientCursor::YieldData yieldData;
                        massert( 16093, "after yield cursor deleted" , cc->prepareToYield( yieldData ) );
                        theDataFileMgr.deleteRecord( d, ns, toDelete.rec(), toDelete, false, true , true );
                        if( !cc->recoverFromYield( yieldData ) ) {
                            cc.release();
                            if( !ok ) {
                                /* we were already at the end. normal. */
                            }
                            else {
                                uasserted(12585, "cursor gone during bg index; dropDups");
                            }
                            break;
                        }

                        // Recalculate idxNo if we yielded
                        idxNo = IndexBuildsInProgress::get(ns, idxName);
                        // This index must still be around, because this is thread that would clean
                        // it up
                        massert(16575, "cannot find index build anymore", idxNo != -1);

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

                if ( cc->yieldSometimes( ClientCursor::WillNeed ) ) {
                    progress.setTotalWhileRunning( d->stats.nrecords );

                    // Recalculate idxNo if we yielded
                    idxNo = IndexBuildsInProgress::get(ns, idxName);
                    // Someone may have interrupted the index build
                    massert(16576, "cannot find index build anymore", idxNo != -1);
                }
                else {
                    idxNo = -1;
                    cc.release();
                    uasserted(12584, "cursor gone during bg index");
                    break;
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
            unsigned long long n = 0;

            prep(ns.c_str(), d);
            try {
                idx.head.writing() = idx.idxInterface().addBucket(idx);
                n = addExistingToIndex(ns.c_str(), d, idx);
                // idx may point at an invalid index entry at this point
            }
            catch(...) {
                if( cc().database() && nsdetails(ns.c_str()) == d ) {
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
                      bool background,
                      bool mayInterrupt) {
        tlog() << "build index " << ns << ' ' << idx.keyPattern() << ( background ? " background" : "" ) << endl;
        Timer t;
        unsigned long long n;

        verify( Lock::isWriteLocked(ns) );

        // Build index spec here in case the collection is empty and the index details are invalid
        idx.getSpec();

        if( inDBRepair || !background ) {
            n = fastBuildIndex(ns.c_str(), d, idx, mayInterrupt);
            verify( !idx.head.isNull() );
        }
        else {
            BackgroundIndexBuildJob j(ns.c_str());
            n = j.go(ns, d, idx);
        }
        tlog() << "build index done.  scanned " << n << " total records. " << t.millis() / 1000.0 << " secs" << endl;
    }

    extern BSONObj id_obj; // { _id : 1 }

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

        string system_indexes = cc().database()->name + ".system.indexes";

        BSONObjBuilder b;
        b.append("name", "_id_");
        b.append("ns", ns);
        b.append("key", id_obj);
        BSONObj o = b.done();

        /* edge case: note the insert could fail if we have hit maxindexes already */
        theDataFileMgr.insert(system_indexes.c_str(), o.objdata(), o.objsize(), mayInterrupt, true);
    }

    /* remove bit from a bit array - actually remove its slot, not a clear
       note: this function does not work with x == 63 -- that is ok
             but keep in mind in the future if max indexes were extended to
             exactly 64 it would be a problem
    */
    unsigned long long removeBit(unsigned long long b, int x) {
        unsigned long long tmp = b;
        return
            (tmp & ((((unsigned long long) 1) << x)-1)) |
            ((tmp >> (x+1)) << x);
    }

    class IndexUpdateTest : public StartupTest {
    public:
        void run() {
            verify( removeBit(1, 0) == 0 );
            verify( removeBit(2, 0) == 1 );
            verify( removeBit(2, 1) == 0 );
            verify( removeBit(255, 1) == 127 );
            verify( removeBit(21, 2) == 9 );
            verify( removeBit(0x4000000000000001ULL, 62) == 1 );
        }
    } iu_unittest;

    bool dropIndexes( NamespaceDetails *d, const char *ns, const char *name, string &errmsg, BSONObjBuilder &anObjBuilder, bool mayDeleteIdIndex ) {

        BackgroundOperation::assertNoBgOpInProgForNs(ns);

        d = d->writingWithExtra();
        d->aboutToDeleteAnIndex();

        /* there may be pointers pointing at keys in the btree(s).  kill them. */
        ClientCursor::invalidate(ns);

        // delete a specific index or all?
        if ( *name == '*' && name[1] == 0 ) {
            LOG(4) << "  d->nIndexes was " << d->nIndexes << '\n';
            anObjBuilder.append("nIndexesWas", (double)d->nIndexes);
            IndexDetails *idIndex = 0;
            if( d->nIndexes ) {
                for ( int i = 0; i < d->nIndexes; i++ ) {
                    if ( !mayDeleteIdIndex && d->idx(i).isIdIndex() ) {
                        idIndex = &d->idx(i);
                    }
                    else {
                        d->idx(i).kill_idx();
                    }
                }
                d->nIndexes = 0;
            }
            if ( idIndex ) {
                d->getNextIndexDetails(ns) = *idIndex;
                d->addIndex(ns);
                wassert( d->nIndexes == 1 );
            }
            /* assuming here that id index is not multikey: */
            d->multiKeyIndexBits = 0;
            assureSysIndexesEmptied(ns, idIndex);
            anObjBuilder.append("msg", mayDeleteIdIndex ?
                                "indexes dropped for collection" :
                                "non-_id indexes dropped for collection");
        }
        else {
            // delete just one index
            int x = d->findIndexByName(name);
            if ( x >= 0 ) {
                LOG(4) << "  d->nIndexes was " << d->nIndexes << endl;
                anObjBuilder.append("nIndexesWas", (double)d->nIndexes);

                /* note it is  important we remove the IndexDetails with this
                 call, otherwise, on recreate, the old one would be reused, and its
                 IndexDetails::info ptr would be bad info.
                 */
                IndexDetails *id = &d->idx(x);
                if ( !mayDeleteIdIndex && id->isIdIndex() ) {
                    errmsg = "may not delete _id index";
                    return false;
                }
                id->kill_idx();
                d->multiKeyIndexBits = removeBit(d->multiKeyIndexBits, x);
                d->nIndexes--;
                for ( int i = x; i < d->nIndexes; i++ )
                    d->idx(i) = d->idx(i+1);
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

}
