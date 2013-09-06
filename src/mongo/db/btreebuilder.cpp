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

#include "mongo/pch.h"

#include "mongo/db/btreebuilder.h"

#include "mongo/db/btree.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/curop-inl.h"
#include "mongo/db/db.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/dur_commitjob.h"
#include "mongo/db/json.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/stats/counters.h"

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
    void BtreeBuilder<V>::buildNextLevel(DiskLoc loc, bool mayInterrupt) {
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
                killCurrentOp.checkForInterrupt( !mayInterrupt );

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

        if( levels > 1 ) {
            LOG(2) << "btree levels: " << levels << endl;
        }
    }

    /** when all addKeys are done, we then build the higher levels of the tree */
    template<class V>
    void BtreeBuilder<V>::commit(bool mayInterrupt) {
        buildNextLevel(first, mayInterrupt);
        committed = true;
    }

    template class BtreeBuilder<V0>;
    template class BtreeBuilder<V1>;

}
