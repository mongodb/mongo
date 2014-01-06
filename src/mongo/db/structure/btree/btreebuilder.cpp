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

#include "mongo/db/structure/btree/btreebuilder.h"

#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/curop-inl.h"
#include "mongo/db/db.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/dur_commitjob.h"
#include "mongo/db/json.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/repl/is_master.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/structure/btree/btree.h"

namespace mongo {

    /* --- BtreeBuilder --- */

    template<class V>
    BtreeBuilder<V>::BtreeBuilder(bool dupsAllowed, IndexCatalogEntry* btreeState ):
        _dupsAllowed(dupsAllowed),
        _btreeState(btreeState),
        _numAdded(0) {
        first = cur = BtreeBucket<V>::addBucket(btreeState);
        b = _getModifiableBucket( cur );
        committed = false;
    }

    template<class V>
    BtreeBucket<V>* BtreeBuilder<V>::_getModifiableBucket( DiskLoc loc ) {
        return BtreeBucket<V>::asVersionMod( _btreeState->recordStore()->recordFor( loc ) );
    }

    template<class V>
    const BtreeBucket<V>* BtreeBuilder<V>::_getBucket( DiskLoc loc ) {
        return BtreeBucket<V>::asVersion( _btreeState->recordStore()->recordFor( loc ) );
    }

    template<class V>
    void BtreeBuilder<V>::newBucket() {
        DiskLoc L = BtreeBucket<V>::addBucket(_btreeState);
        b->setTempNext(L);
        cur = L;
        b = _getModifiableBucket( cur );
    }

    template<class V>
    void BtreeBuilder<V>::mayCommitProgressDurably() {
        if ( getDur().commitIfNeeded() ) {
            b = _getModifiableBucket( cur );
        }
    }

    template<class V>
    void BtreeBuilder<V>::addKey(BSONObj& _key, DiskLoc loc) {
        auto_ptr< KeyOwned > key( new KeyOwned(_key) );
        if ( key->dataSize() > BtreeBucket<V>::KeyMax ) {
            string msg = str::stream() << "Btree::insert: key too large to index, failing "
                                       << _btreeState->descriptor()->indexNamespace()
                                       << ' ' << key->dataSize() << ' ' << key->toString();
            problem() << msg << endl;
            if ( isMaster( NULL ) ) {
                uasserted( 17282, msg );
            }
            return;
        }

        if( !_dupsAllowed ) {
            if( _numAdded > 0 ) {
                int cmp = keyLast->woCompare(*key, _btreeState->ordering());
                massert( 10288 ,  "bad key order in BtreeBuilder - server internal error", cmp <= 0 );
                if( cmp == 0 ) {
                    //if( !dupsAllowed )
                    uasserted( ASSERT_ID_DUPKEY, BtreeBucket<V>::dupKeyError( _btreeState->descriptor(),
                                                                              *keyLast ) );
                }
            }
        }

        if ( ! b->_pushBack(loc, *key, _btreeState->ordering(), DiskLoc()) ) {
            // bucket was full
            newBucket();
            b->pushBack(loc, *key, _btreeState->ordering(), DiskLoc());
        }
        keyLast = key;
        _numAdded++;
        mayCommitProgressDurably();
    }

    template<class V>
    void BtreeBuilder<V>::buildNextLevel(DiskLoc loc, bool mayInterrupt) {
        int levels = 1;
        while( 1 ) {
            if( _getBucket(loc)->tempNext().isNull() ) {
                // only 1 bucket at this level. we are done.
                _btreeState->setHead( loc );
                break;
            }
            levels++;

            DiskLoc upLoc = BtreeBucket<V>::addBucket(_btreeState);
            DiskLoc upStart = upLoc;
            BtreeBucket<V> *up = _getModifiableBucket( upLoc );

            DiskLoc xloc = loc;
            while( !xloc.isNull() ) {
                killCurrentOp.checkForInterrupt( !mayInterrupt );

                if ( getDur().commitIfNeeded() ) {
                    b = _getModifiableBucket( cur );
                    up = _getModifiableBucket( upLoc );
                }

                BtreeBucket<V> *x = _getModifiableBucket( xloc );
                Key k;
                DiskLoc r;
                x->popBack(r,k);
                bool keepX = ( x->n != 0 );
                DiskLoc keepLoc = keepX ? xloc : x->nextChild;

                if ( ! up->_pushBack(r, k, _btreeState->ordering(), keepLoc) ) {
                    // current bucket full
                    DiskLoc n = BtreeBucket<V>::addBucket(_btreeState);
                    up->setTempNext(n);
                    upLoc = n;
                    up = _getModifiableBucket( upLoc );
                    up->pushBack(r, k, _btreeState->ordering(), keepLoc);
                }

                DiskLoc nextLoc = x->tempNext(); // get next in chain at current level
                if ( keepX ) {
                    x->parent = upLoc;
                }
                else {
                    if ( !x->nextChild.isNull() ) {
                        DiskLoc ll = x->nextChild;
                        _getModifiableBucket(ll)->parent = upLoc;
                        //(x->nextChild.btreemod<V>())->parent = upLoc;
                    }
                    x->deallocBucket( _btreeState, xloc );
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
