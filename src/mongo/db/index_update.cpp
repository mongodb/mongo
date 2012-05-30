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
#include "mongo/db/compact.h"
#include "mongo/db/extsort.h"
#include "mongo/db/index.h"
#include "mongo/db/namespace_details.h"
#include "mongo/db/pdfile_private.h"
#include "mongo/db/replutil.h"
#include "mongo/util/processinfo.h"

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
        if( d->indexBuildInProgress ) { // background index
            // always pass nowarn here, as this one may be missing for valid reasons as we are concurrently building it
            _unindexRecord(d->idx(n), obj, dl, false);
        }
    }

    /* step one of adding keys to index idxNo for a new record
       @return true means done.  false means multikey involved and more work to do
    */
    void fetchIndexInserters(BSONObjSet & /*out*/keys,
                             IndexInterface::IndexInserter &inserter,
                             NamespaceDetails *d,
                             int idxNo,
                             const BSONObj& obj,
                             DiskLoc recordLoc) {
        IndexDetails &idx = d->idx(idxNo);
        idx.getKeysFromObject(obj, keys);
        if( keys.empty() )
            return;
        bool dupsAllowed = !idx.unique();
        Ordering ordering = Ordering::make(idx.keyPattern());
        
        verify( !recordLoc.isNull() );

        try {
            // we can't do the two step method with multi keys as insertion of one key changes the indexes 
            // structure.  however we can do the first key of the set so we go ahead and do that FWIW
            inserter.addInsertionContinuation(
                    idx.idxInterface().beginInsertIntoIndex(
                            idxNo, idx, recordLoc, *keys.begin(), ordering, dupsAllowed));
        }
        catch (AssertionException& e) {
            if( e.getCode() == 10287 && idxNo == d->nIndexes ) {
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
        int n = d->nIndexesBeingBuilt();
        {
            BSONObjSet keys;
            for ( int i = 0; i < n; i++ ) {
                // this call throws on unique constraint violation.  we haven't done any writes yet so that is fine.
                fetchIndexInserters(/*out*/keys, inserter, d, i, obj, loc);
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
            IndexInterface& ii = idx.idxInterface();
            Ordering ordering = Ordering::make(idx.keyPattern());
            d->setIndexIsMultikey(ns, i);
            for( BSONObjSet::iterator k = ++keys.begin()/*skip 1*/; k != keys.end(); k++ ) {
                try {
                    ii.bt_insert(idx.head, loc, *k, ordering, !idx.unique(), idx);
                } catch (AssertionException& e) {
                    if( e.getCode() == 10287 && (int) i == d->nIndexes ) {
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
                                log(3) << "unindex fails on rollback after unique key constraint prevented insert\n";
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
                if( e.getCode() == 10287 && idxNo == d->nIndexes ) {
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

    SortPhaseOne *precalced = 0;

    template< class V >
    void buildBottomUpPhases2And3(bool dupsAllowed, IndexDetails& idx, BSONObjExternalSorter& sorter, 
        bool dropDups, set<DiskLoc> &dupsToDrop, CurOp * op, SortPhaseOne *phase1, ProgressMeterHolder &pm,
        Timer& t
        )
    {
        BtreeBuilder<V> btBuilder(dupsAllowed, idx);
        BSONObj keyLast;
        auto_ptr<BSONObjExternalSorter::Iterator> i = sorter.iterator();
        verify( pm == op->setMessage( "index: (2/3) btree bottom up" , phase1->nkeys , 10 ) );
        while( i->more() ) {
            RARELY killCurrentOp.checkForInterrupt();
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
        log(t.seconds() > 10 ? 0 : 1 ) << "\t done building bottom layer, going to commit" << endl;
        btBuilder.commit();
        if ( btBuilder.getn() != phase1->nkeys && ! dropDups ) {
            warning() << "not all entries were added to the index, probably some keys were too large" << endl;
        }
    }

    // throws DBException
    unsigned long long fastBuildIndex(const char *ns, NamespaceDetails *d, IndexDetails& idx, int idxNo) {
        CurOp * op = cc().curop();

        Timer t;

        tlog(1) << "fastBuildIndex " << ns << " idxNo:" << idxNo << ' ' << idx.info.obj().toString() << endl;

        bool dupsAllowed = !idx.unique();
        bool dropDups = idx.dropDups() || inDBRepair;
        BSONObj order = idx.keyPattern();

        getDur().writingDiskLoc(idx.head).Null();

        if ( logLevel > 1 ) printMemInfo( "before index start" );

        /* get and sort all the keys ----- */
        ProgressMeterHolder pm( op->setMessage( "index: (1/3) external sort" , d->stats.nrecords , 10 ) );
        SortPhaseOne _ours;
        SortPhaseOne *phase1 = precalced;
        if( phase1 == 0 ) {
            phase1 = &_ours;
            SortPhaseOne& p1 = *phase1;
            shared_ptr<Cursor> c = theDataFileMgr.findAll(ns);
            p1.sorter.reset( new BSONObjExternalSorter(idx.idxInterface(), order) );
            p1.sorter->hintNumObjects( d->stats.nrecords );
            const IndexSpec& spec = idx.getSpec();
            while ( c->ok() ) {
                BSONObj o = c->current();
                DiskLoc loc = c->currLoc();
                p1.addKeys(spec, o, loc);
                c->advance();
                pm.hit();
                if ( logLevel > 1 && p1.n % 10000 == 0 ) {
                    printMemInfo( "\t iterating objects" );
                }
            };
        }
        pm.finished();

        BSONObjExternalSorter& sorter = *(phase1->sorter);

        if( phase1->multi )
            d->setIndexIsMultikey(ns, idxNo);

        if ( logLevel > 1 ) printMemInfo( "before final sort" );
        phase1->sorter->sort();
        if ( logLevel > 1 ) printMemInfo( "after final sort" );

        log(t.seconds() > 5 ? 0 : 1) << "\t external sort used : " << sorter.numFiles() << " files " << " in " << t.seconds() << " secs" << endl;

        set<DiskLoc> dupsToDrop;

        /* build index --- */
        if( idx.version() == 0 )
            buildBottomUpPhases2And3<V0>(dupsAllowed, idx, sorter, dropDups, dupsToDrop, op, phase1, pm, t);
        else if( idx.version() == 1 ) 
            buildBottomUpPhases2And3<V1>(dupsAllowed, idx, sorter, dropDups, dupsToDrop, op, phase1, pm, t);
        else
            verify(false);

        if( dropDups ) 
            log() << "\t fastBuildIndex dupsToDrop:" << dupsToDrop.size() << endl;

        for( set<DiskLoc>::iterator i = dupsToDrop.begin(); i != dupsToDrop.end(); i++ ){
            theDataFileMgr.deleteRecord( ns, i->rec(), *i, false /* cappedOk */ , true /* noWarn */ , isMaster( ns ) /* logOp */ );
            getDur().commitIfNeeded();
        }

        return phase1->n;
    }

    class BackgroundIndexBuildJob : public BackgroundOperation {

        unsigned long long addExistingToIndex(const char *ns, NamespaceDetails *d, IndexDetails& idx, int idxNo) {
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
                        theDataFileMgr.deleteRecord( ns, toDelete.rec(), toDelete, false, true , true );
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
                }
                else {
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
        void done(const char *ns, NamespaceDetails *d) {
            NamespaceDetailsTransient::get(ns).addedIndex(); // clear query optimizer cache
            Lock::assertWriteLocked(ns);
        }

    public:
        BackgroundIndexBuildJob(const char *ns) : BackgroundOperation(ns) { }

        unsigned long long go(string ns, NamespaceDetails *d, IndexDetails& idx, int idxNo) {
            unsigned long long n = 0;

            prep(ns.c_str(), d);
            verify( idxNo == d->nIndexes );
            try {
                idx.head.writing() = idx.idxInterface().addBucket(idx);
                n = addExistingToIndex(ns.c_str(), d, idx, idxNo);
            }
            catch(...) {
                if( cc().database() && nsdetails(ns.c_str()) == d ) {
                    verify( idxNo == d->nIndexes );
                    done(ns.c_str(), d);
                }
                else {
                    log() << "ERROR: db gone during bg index?" << endl;
                }
                throw;
            }
            verify( idxNo == d->nIndexes );
            done(ns.c_str(), d);
            return n;
        }
    };

    /**
     * For the lifetime of this object, an index build is indicated on the specified
     * namespace and the newest index is marked as absent.  This simplifies
     * the cleanup required on recovery.
     */
    class RecoverableIndexState {
    public:
        RecoverableIndexState( NamespaceDetails *d ) : _d( d ) {
            indexBuildInProgress() = 1;
            nIndexes()--;
        }
        ~RecoverableIndexState() {
            DESTRUCTOR_GUARD (
                nIndexes()++;
                indexBuildInProgress() = 0;
            )
        }
    private:
        int &nIndexes() { return getDur().writingInt( _d->nIndexes ); }
        int &indexBuildInProgress() { return getDur().writingInt( _d->indexBuildInProgress ); }
        NamespaceDetails *_d;
    };

    // throws DBException
    void buildAnIndex(string ns, NamespaceDetails *d, IndexDetails& idx, int idxNo, bool background) {
        tlog() << "build index " << ns << ' ' << idx.keyPattern() << ( background ? " background" : "" ) << endl;
        Timer t;
        unsigned long long n;

        verify( !BackgroundOperation::inProgForNs(ns.c_str()) ); // should have been checked earlier, better not be...
        verify( d->indexBuildInProgress == 0 );
        verify( Lock::isWriteLocked(ns) );
        RecoverableIndexState recoverable( d );

        // Build index spec here in case the collection is empty and the index details are invalid
        idx.getSpec();

        if( inDBRepair || !background ) {
            n = fastBuildIndex(ns.c_str(), d, idx, idxNo);
            verify( !idx.head.isNull() );
        }
        else {
            BackgroundIndexBuildJob j(ns.c_str());
            n = j.go(ns, d, idx, idxNo);
        }
        tlog() << "build index done.  scanned " << n << " total records. " << t.millis() / 1000.0 << " secs" << endl;
    }

    /* add keys to indexes for a new record */
#if 0
    static void oldIndexRecord__notused(NamespaceDetails *d, BSONObj obj, DiskLoc loc) {
        int n = d->nIndexesBeingBuilt();
        for ( int i = 0; i < n; i++ ) {
            try {
                bool unique = d->idx(i).unique();
                addKeysToIndex(d, i, obj, loc, /*dupsAllowed*/!unique);
            }
            catch( DBException& ) {
                /* try to roll back previously added index entries
                   note <= i (not < i) is important here as the index we were just attempted
                   may be multikey and require some cleanup.
                */
                for( int j = 0; j <= i; j++ ) {
                    try {
                        _unindexRecord(d->idx(j), obj, loc, false);
                    }
                    catch(...) {
                        log(3) << "unindex fails on rollback after unique failure\n";
                    }
                }
                throw;
            }
        }
    }
#endif

    extern BSONObj id_obj; // { _id : 1 }

    void ensureHaveIdIndex(const char *ns) {
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
        theDataFileMgr.insert(system_indexes.c_str(), o.objdata(), o.objsize(), true);
    }


}
