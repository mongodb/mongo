// btreebuilder.cpp

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

#include "pch.h"
#include "db.h"
#include "btree.h"
#include "pdfile.h"
#include "json.h"
#include "clientcursor.h"
#include "client.h"
#include "dbhelpers.h"
#include "curop-inl.h"
#include "stats/counters.h"
#include "dur_commitjob.h"
#include "btreebuilder.h"

namespace mongo {

    /* --- BtreeBuilder --- */

    template<class V>
    BtreeBuilder<V>::BtreeBuilder(bool _dupsAllowed, IndexDetails& _idx) :
        dupsAllowed(_dupsAllowed),
        idx(_idx),
        n(0),
        order( idx.keyPattern() ),
        ordering( Ordering::make(idx.keyPattern()) ) {
        first = cur = BtreeBucket<V>::addBucket(idx);
        b = cur.btreemod<V>();
        committed = false;
    }

    template<class V>
    void BtreeBuilder<V>::newBucket() {
        DiskLoc L = BtreeBucket<V>::addBucket(idx);
        b->setTempNext(L);
        cur = L;
        b = cur.btreemod<V>();
    }

    template<class V>
    void BtreeBuilder<V>::mayCommitProgressDurably() {
        if ( getDur().commitIfNeeded() ) {
            b = cur.btreemod<V>();
        }
    }

    template<class V>
    void BtreeBuilder<V>::addKey(BSONObj& _key, DiskLoc loc) {

        auto_ptr< KeyOwned > key( new KeyOwned(_key) );
        if ( key->dataSize() > BtreeBucket<V>::KeyMax ) {
            problem() << "Btree::insert: key too large to index, skipping " << idx.indexNamespace() 
                      << ' ' << key->dataSize() << ' ' << key->toString() << endl;
            return;
        }

        if( !dupsAllowed ) {
            if( n > 0 ) {
                int cmp = keyLast->woCompare(*key, ordering);
                massert( 10288 ,  "bad key order in BtreeBuilder - server internal error", cmp <= 0 );
                if( cmp == 0 ) {
                    //if( !dupsAllowed )
                    uasserted( ASSERT_ID_DUPKEY , BtreeBucket<V>::dupKeyError( idx , *keyLast ) );
                }
            }
        }

        if ( ! b->_pushBack(loc, *key, ordering, DiskLoc()) ) {
            // bucket was full
            newBucket();
            b->pushBack(loc, *key, ordering, DiskLoc());
        }
        keyLast = key;
        n++;
        mayCommitProgressDurably();
    }

    template<class V>
    void BtreeBuilder<V>::buildNextLevel(DiskLoc loc) {
        int levels = 1;
        while( 1 ) {
            if( loc.btree<V>()->tempNext().isNull() ) {
                // only 1 bucket at this level. we are done.
                getDur().writingDiskLoc(idx.head) = loc;
                break;
            }
            levels++;

            DiskLoc upLoc = BtreeBucket<V>::addBucket(idx);
            DiskLoc upStart = upLoc;
            BtreeBucket<V> *up = upLoc.btreemod<V>();

            DiskLoc xloc = loc;
            while( !xloc.isNull() ) {
                if ( getDur().commitIfNeeded() ) {
                    b = cur.btreemod<V>();
                    up = upLoc.btreemod<V>();
                }

                BtreeBucket<V> *x = xloc.btreemod<V>();
                Key k;
                DiskLoc r;
                x->popBack(r,k);
                bool keepX = ( x->n != 0 );
                DiskLoc keepLoc = keepX ? xloc : x->nextChild;

                if ( ! up->_pushBack(r, k, ordering, keepLoc) ) {
                    // current bucket full
                    DiskLoc n = BtreeBucket<V>::addBucket(idx);
                    up->setTempNext(n);
                    upLoc = n;
                    up = upLoc.btreemod<V>();
                    up->pushBack(r, k, ordering, keepLoc);
                }

                DiskLoc nextLoc = x->tempNext(); // get next in chain at current level
                if ( keepX ) {
                    x->parent = upLoc;
                }
                else {
		  if ( !x->nextChild.isNull() ) {
		    DiskLoc ll = x->nextChild;
		    ll.btreemod<V>()->parent = upLoc;
		    //(x->nextChild.btreemod<V>())->parent = upLoc;
		  }
		  x->deallocBucket( xloc, idx );
                }
                xloc = nextLoc;
            }

            loc = upStart;
            mayCommitProgressDurably();
        }

        if( levels > 1 )
            log(2) << "btree levels: " << levels << endl;
    }

    /** when all addKeys are done, we then build the higher levels of the tree */
    template<class V>
    void BtreeBuilder<V>::commit() {
        buildNextLevel(first);
        committed = true;
    }

    template<class V>
    BtreeBuilder<V>::~BtreeBuilder() {
        DESTRUCTOR_GUARD(
            if( !committed ) {
                log(2) << "Rolling back partially built index space" << endl;
                DiskLoc x = first;
                while( !x.isNull() ) {
                    DiskLoc next = x.btree<V>()->tempNext();
                    string ns = idx.indexNamespace();
                    theDataFileMgr._deleteRecord(nsdetails(ns.c_str()), ns.c_str(), x.rec(), x);
                    x = next;
                    getDur().commitIfNeeded();
                }
                verify( idx.head.isNull() );
                log(2) << "done rollback" << endl;
            }
        )
    }

    template class BtreeBuilder<V0>;
    template class BtreeBuilder<V1>;

}
