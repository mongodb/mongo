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

#include "mongo/db/index/btree_based_builder.h"

#include "mongo/db/btreebuilder.h"
#include "mongo/db/index/catalog_hack.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/pdfile_private.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/is_master.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/sort_phase_one.h"
#include "mongo/util/processinfo.h"

namespace mongo {

    int oldCompare(const BSONObj& l,const BSONObj& r, const Ordering &o); // key.cpp

    class ExternalSortComparisonV0 : public ExternalSortComparison {
    public:
        ExternalSortComparisonV0(const BSONObj& ordering) : _ordering(Ordering::make(ordering)) { }
        virtual ~ExternalSortComparisonV0() { }

        virtual int compare(const ExternalSortDatum& l, const ExternalSortDatum& r) const {
            int x = oldCompare(l.first, r.first, _ordering);
            if (x) { return x; }
            return l.second.compare(r.second);
        }
    private:
        const Ordering _ordering;
    };

    class ExternalSortComparisonV1 : public ExternalSortComparison {
    public:
        ExternalSortComparisonV1(const BSONObj& ordering) : _ordering(Ordering::make(ordering)) { }
        virtual ~ExternalSortComparisonV1() { }

        virtual int compare(const ExternalSortDatum& l, const ExternalSortDatum& r) const {
            int x = l.first.woCompare(r.first, _ordering, /*considerfieldname*/false);
            if (x) { return x; }
            return l.second.compare(r.second);
        }
    private:
        const Ordering _ordering;
    };

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
        // verifies that pm and op refer to the same ProgressMeter
        verify(pm == op->setMessage("index: (2/3) btree bottom up",
                                    "Index: (2/3) BTree Bottom Up Progress",
                                    phase1->nkeys,
                                    10));
        while( i->more() ) {
            RARELY killCurrentOp.checkForInterrupt( !mayInterrupt );
            ExternalSortDatum d = i->next();

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
        op->setMessage("index: (3/3) btree-middle", "Index: (3/3) BTree Middle Progress");
        LOG(t.seconds() > 10 ? 0 : 1 ) << "\t done building bottom layer, going to commit" << endl;
        btBuilder.commit( mayInterrupt );
        if ( btBuilder.getn() != phase1->nkeys && ! dropDups ) {
            warning() << "not all entries were added to the index, probably some "
                         "keys were too large" << endl;
        }
    }

    DiskLoc BtreeBasedBuilder::makeEmptyIndex(const IndexDetails& idx) {
        if (0 == idx.version()) {
            return BtreeBucket<V0>::addBucket(idx);
        } else {
            return BtreeBucket<V1>::addBucket(idx);
        }
    }

    ExternalSortComparison* BtreeBasedBuilder::getComparison(int version,
                                                             const BSONObj& keyPattern) {
        if (0 == version) {
            return new ExternalSortComparisonV0(keyPattern);
        } else {
            verify(1 == version);
            return new ExternalSortComparisonV1(keyPattern);
        }
    }

    void BtreeBasedBuilder::addKeysToPhaseOne(NamespaceDetails* d, const char* ns,
                           const IndexDetails& idx,
                           const BSONObj& order,
                           SortPhaseOne* phaseOne,
                           int64_t nrecords,
                           ProgressMeter* progressMeter,
                           bool mayInterrupt, int idxNo) {
        auto_ptr<Runner> runner(InternalPlanner::collectionScan(ns));
        phaseOne->sortCmp.reset(getComparison(idx.version(), idx.keyPattern()));
        phaseOne->sorter.reset(new BSONObjExternalSorter(phaseOne->sortCmp.get()));
        phaseOne->sorter->hintNumObjects( nrecords );
        auto_ptr<IndexDescriptor> desc(CatalogHack::getDescriptor(d, idxNo));
        auto_ptr<BtreeBasedAccessMethod> iam(CatalogHack::getBtreeBasedIndex(desc.get()));
        BSONObj o;
        DiskLoc loc;
        Runner::RunnerState state;
        while (Runner::RUNNER_ADVANCED == (state = runner->getNext(&o, &loc))) {
            RARELY killCurrentOp.checkForInterrupt( !mayInterrupt );
            BSONObjSet keys;
            iam->getKeys(o, &keys);
            phaseOne->addKeys(keys, loc, mayInterrupt);
            progressMeter->hit();
            if (logger::globalLogDomain()->shouldLog(logger::LogSeverity::Debug(2))
                && phaseOne->n % 10000 == 0 ) {
                printMemInfo( "\t iterating objects" );
            }
        }

        uassert(17050, "Internal error reading docs from collection", Runner::RUNNER_EOF == state);

    }

    uint64_t BtreeBasedBuilder::fastBuildIndex(const char* ns, NamespaceDetails* d,
                                               IndexDetails& idx, bool mayInterrupt,
                                               int idxNo) {
        CurOp * op = cc().curop();

        Timer t;

        MONGO_TLOG(1) << "fastBuildIndex " << ns << ' ' << idx.info.obj().toString() << endl;

        bool dupsAllowed = !idx.unique() || ignoreUniqueIndex(idx);
        bool dropDups = idx.dropDups() || inDBRepair;
        BSONObj order = idx.keyPattern();

        getDur().writingDiskLoc(idx.head).Null();

        if ( logger::globalLogDomain()->shouldLog(logger::LogSeverity::Debug(2) ) )
            printMemInfo( "before index start" );

        /* get and sort all the keys ----- */
        ProgressMeterHolder pm(op->setMessage("index: (1/3) external sort",
                                              "Index: (1/3) External Sort Progress",
                                              d->numRecords(),
                                              10));
        SortPhaseOne phase1;
        addKeysToPhaseOne(d, ns, idx, order, &phase1, d->numRecords(), pm.get(),
                          mayInterrupt, idxNo );
        pm.finished();

        BSONObjExternalSorter& sorter = *(phase1.sorter);

        if( phase1.multi ) {
            d->setIndexIsMultikey(ns, idxNo);
        }

        if ( logger::globalLogDomain()->shouldLog(logger::LogSeverity::Debug(2) ) )
            printMemInfo( "before final sort" );
        phase1.sorter->sort( mayInterrupt );
        if ( logger::globalLogDomain()->shouldLog(logger::LogSeverity::Debug(2) ) )
            printMemInfo( "after final sort" );

        LOG(t.seconds() > 5 ? 0 : 1) << "\t external sort used : " << sorter.numFiles()
                                     << " files " << " in " << t.seconds() << " secs" << endl;

        set<DiskLoc> dupsToDrop;

        /* build index --- */
        if( idx.version() == 0 )
            buildBottomUpPhases2And3<V0>(dupsAllowed,
                                         idx,
                                         sorter,
                                         dropDups,
                                         dupsToDrop,
                                         op,
                                         &phase1,
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
                                         &phase1,
                                         pm,
                                         t,
                                         mayInterrupt);
        else
            verify(false);

        if( dropDups ) 
            log() << "\t fastBuildIndex dupsToDrop:" << dupsToDrop.size() << endl;

        BtreeBasedBuilder::doDropDups(ns, d, dupsToDrop, mayInterrupt);

        return phase1.n;
    }

    void BtreeBasedBuilder::doDropDups(const char* ns, NamespaceDetails* d,
                                       const set<DiskLoc>& dupsToDrop, bool mayInterrupt) {

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

}  // namespace mongo
