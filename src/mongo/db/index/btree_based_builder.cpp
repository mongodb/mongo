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
*/

#include "mongo/db/index/btree_based_builder.h"

#include "mongo/db/btreebuilder.h"
#include "mongo/db/index/catalog_hack.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/repl/is_master.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/sort_phase_one.h"
#include "mongo/util/processinfo.h"
#include "mongo/db/pdfile_private.h"

namespace mongo {

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
        op->setMessage("index: (3/3) btree-middle", "Index: (3/3) BTree Middle Progress");
        LOG(t.seconds() > 10 ? 0 : 1 ) << "\t done building bottom layer, going to commit" << endl;
        btBuilder.commit( mayInterrupt );
        if ( btBuilder.getn() != phase1->nkeys && ! dropDups ) {
            warning() << "not all entries were added to the index, probably some keys were too large" << endl;
        }
    }

    void BtreeBasedBuilder::addKeysToPhaseOne(NamespaceDetails* d, const char* ns,
                           const IndexDetails& idx,
                           const BSONObj& order,
                           SortPhaseOne* phaseOne,
                           int64_t nrecords,
                           ProgressMeter* progressMeter,
                           bool mayInterrupt, int idxNo) {
        shared_ptr<Cursor> cursor = theDataFileMgr.findAll( ns );
        phaseOne->sorter.reset( new BSONObjExternalSorter( idx.idxInterface(), order ) );
        phaseOne->sorter->hintNumObjects( nrecords );
        auto_ptr<IndexDescriptor> desc(CatalogHack::getDescriptor(d, idxNo));
        auto_ptr<BtreeBasedAccessMethod> iam(CatalogHack::getBtreeBasedIndex(desc.get()));
        while ( cursor->ok() ) {
            RARELY killCurrentOp.checkForInterrupt( !mayInterrupt );
            BSONObj o = cursor->current();
            DiskLoc loc = cursor->currLoc();
            BSONObjSet keys;
            iam->getKeys(o, &keys);
            phaseOne->addKeys(keys, loc, mayInterrupt);
            cursor->advance();
            progressMeter->hit();
            if ( logLevel > 1 && phaseOne->n % 10000 == 0 ) {
                printMemInfo( "\t iterating objects" );
            }
        }
    }

    uint64_t BtreeBasedBuilder::fastBuildIndex(const char* ns, NamespaceDetails* d,
                                               IndexDetails& idx, bool mayInterrupt,
                                               int idxNo) {
        CurOp * op = cc().curop();

        Timer t;

        tlog(1) << "fastBuildIndex " << ns << ' ' << idx.info.obj().toString() << endl;

        bool dupsAllowed = !idx.unique() || ignoreUniqueIndex(idx);
        bool dropDups = idx.dropDups() || inDBRepair;
        BSONObj order = idx.keyPattern();

        getDur().writingDiskLoc(idx.head).Null();

        if ( logLevel > 1 ) printMemInfo( "before index start" );

        /* get and sort all the keys ----- */
        ProgressMeterHolder pm(op->setMessage("index: (1/3) external sort",
                                              "Index: (1/3) External Sort Progress",
                                              d->stats.nrecords,
                                              10));
        SortPhaseOne phase1;
        addKeysToPhaseOne(d, ns, idx, order, &phase1, d->stats.nrecords, pm.get(),
                          mayInterrupt, idxNo );
        pm.finished();

        BSONObjExternalSorter& sorter = *(phase1.sorter);
        // Ensure the index and external sorter have a consistent index interface (and sort order).
        fassert( 16408, &idx.idxInterface() == &sorter.getIndexInterface() );

        if( phase1.multi ) {
            d->setIndexIsMultikey(ns, idxNo);
        }

        if ( logLevel > 1 ) printMemInfo( "before final sort" );
        phase1.sorter->sort( mayInterrupt );
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
