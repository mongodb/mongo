// btreecursor.cpp

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
#include "btree.h"
#include "pdfile.h"
#include "jsobj.h"
#include "curop.h"

namespace mongo {

    extern int otherTraceLevel;

    BtreeCursor::BtreeCursor( NamespaceDetails *_d, int _idxNo, const IndexDetails &_id, 
                              const BSONObj &_startKey, const BSONObj &_endKey, bool endKeyInclusive, int _direction, bool independentFieldRanges ) :
            d(_d), idxNo(_idxNo), 
            startKey( _startKey ),
            endKey( _endKey ),
            endKeyInclusive_( endKeyInclusive ),
            _nEqKeyElts( 0 ),
            multikey( d->isMultikey( idxNo ) ),
            indexDetails( _id ),
            order( _id.keyPattern() ),
            _ordering( Ordering::make( order ) ),
            _superlativeKey( makeSuperlativeKey( order, _direction ) ),
            direction( _direction ),
            boundIndex_(),
            _spec( _id.getSpec() ),
            _independentFieldRanges( _spec.getType() ? false : independentFieldRanges )
    {
        audit();
        init();
        DEV assert( dups.size() == 0 );
    }

    BtreeCursor::BtreeCursor( NamespaceDetails *_d, int _idxNo, const IndexDetails& _id, const vector< pair< BSONObj, BSONObj > > &_bounds, int _direction )
        :
            d(_d), idxNo(_idxNo), 
            endKeyInclusive_( true ),
            _nEqKeyElts( 0 ),
            multikey( d->isMultikey( idxNo ) ),
            indexDetails( _id ),
            order( _id.keyPattern() ),
            _ordering( Ordering::make( order ) ),
            _superlativeKey( makeSuperlativeKey( order, _direction ) ),
            direction( _direction ),
            bounds_( _bounds ),
            boundIndex_(),
            _spec( _id.getSpec() ),
            _independentFieldRanges( !_spec.getType() )
    {
        assert( !bounds_.empty() );
        audit();
        initInterval();
        DEV assert( dups.size() == 0 );
    }

    BSONObj BtreeCursor::makeSuperlativeKey( const BSONObj &order, int direction ) {
        BSONObjBuilder b;
        BSONObjIterator i( order );
        while( i.more() ) {
            BSONElement e = i.next();
            if ( ( e.number() < 0 ) ^ ( direction < 0 ) ) {
                b.appendMinKey( "" );
            } else {
                b.appendMaxKey( "" );                
            }
        }
        return b.obj();
    }
    
    void BtreeCursor::audit() {
        dassert( d->idxNo((IndexDetails&) indexDetails) == idxNo );

        if ( otherTraceLevel >= 12 ) {
            if ( otherTraceLevel >= 200 ) {
                out() << "::BtreeCursor() qtl>200.  validating entire index." << endl;
                indexDetails.head.btree()->fullValidate(indexDetails.head, order);
            }
            else {
                out() << "BTreeCursor(). dumping head bucket" << endl;
                indexDetails.head.btree()->dump();
            }
        }
    }

    void BtreeCursor::init() {
        if ( _spec.getType() ){
            startKey = _spec.getType()->fixKey( startKey );
            endKey = _spec.getType()->fixKey( endKey );
        } else if ( _independentFieldRanges ) {
            _nEqKeyElts = 0;
            BSONObjIterator i( startKey );
            BSONObjIterator j( endKey );
            while( i.more() && j.more() ) {
                if ( i.next().valuesEqual( j.next() ) ) {
                    ++_nEqKeyElts;
                } else {
                    break;
                }
            }
        }
        bool found;
        bucket = indexDetails.head.btree()->
            locate(indexDetails, indexDetails.head, startKey, _ordering, keyOfs, found, direction > 0 ? minDiskLoc : maxDiskLoc, direction);
        skipAndCheck();
    }
    
    void BtreeCursor::initInterval() {
        do {
            startKey = bounds_[ boundIndex_ ].first;
            endKey = bounds_[ boundIndex_ ].second;
            init();
        } while ( !ok() && ++boundIndex_ < bounds_.size() );
    }

    void BtreeCursor::skipAndCheck() {
        skipUnusedKeys();
        if ( !_independentFieldRanges ) {
            checkEnd();
            return;
        }
        while( 1 ) {
            if ( !skipOutOfRangeKeysAndCheckEnd() ) {
                break;
            }
            while( skipOutOfRangeKeysAndCheckEnd() );
            if ( !skipUnusedKeys() ) {
                break;
            }
        }
    }
    
    /* skip unused keys. */
    bool BtreeCursor::skipUnusedKeys() {
        int u = 0;
        while ( 1 ) {
            if ( !ok() )
                break;
            BtreeBucket *b = bucket.btree();
            _KeyNode& kn = b->k(keyOfs);
            if ( kn.isUsed() )
                break;
            bucket = b->advance(bucket, keyOfs, direction, "skipUnusedKeys");
            u++;
            if ( u % 10 == 0 ) {
                skipOutOfRangeKeysAndCheckEnd();
            }
        }
        if ( u > 10 )
            OCCASIONALLY log() << "btree unused skipped:" << u << '\n';
        return u;
    }

    // Return a value in the set {-1, 0, 1} to represent the sign of parameter i.
    int sgn( int i ) {
        if ( i == 0 )
            return 0;
        return i > 0 ? 1 : -1;
    }

    // Check if the current key is beyond endKey.
    void BtreeCursor::checkEnd() {
        if ( bucket.isNull() )
            return;
        if ( !endKey.isEmpty() ) {
            int cmp = sgn( endKey.woCompare( currKey(), order ) );
            if ( ( cmp != 0 && cmp != direction ) ||
                ( cmp == 0 && !endKeyInclusive_ ) )
                bucket = DiskLoc();
        }
    }
    
    bool BtreeCursor::skipOutOfRangeKeysAndCheckEnd() {
        if ( bucket.isNull() )
            return false;
        bool eq = true;
        if ( !endKey.isEmpty() ) {
            int i = 0;
            BSONObjIterator c( currKey() );
            BSONObjIterator l( startKey );
            BSONObjIterator r( endKey );
            BSONObjIterator o( order );
            for( ; i < _nEqKeyElts; ++i ) {
                BSONElement cc = c.next();
                BSONElement ll = l.next();
                BSONElement rr = r.next();
                BSONElement oo = o.next();
                int x = cc.woCompare( rr, false );
                if ( ( oo.number() < 0 ) ^ ( direction < 0 ) ) {
                    x = -x;
                }
                if ( x > 0 ) {
                    bucket = DiskLoc();
                    return false;
                }
                // can't have x < 0, since start and end are equal for these fields
                assert( x == 0 );
            }
            // first range (non equality) element
            if( c.more() ) {
                BSONElement cc = c.next();
                BSONElement ll = l.next();
                BSONElement rr = r.next();
                BSONElement oo = o.next();
                int x = cc.woCompare( rr, false );
                if ( ( oo.number() < 0 ) ^ ( direction < 0 ) ) {
                    x = -x;
                }
                if ( x > 0 ) {
                    bucket = DiskLoc();
                    return false;
                } else if ( x < 0 ) {
                    eq = false;
                }
                ++i;
            }
            // subsequent elements
            for( ; c.more(); ++i ) {
                BSONElement cc = c.next();
                BSONElement ll = l.next();
                BSONElement rr = r.next();
                BSONElement oo = o.next();
                int x = cc.woCompare( rr, false );
                if ( ( oo.number() < 0 ) ^ ( direction < 0 ) ) {
                    x = -x;
                }
                if ( x > 0 ) {
                    advanceTo( currKey(), i, _superlativeKey );
                    return true;
                } else if ( x < 0 ) {
                    eq = false;
                    int y = cc.woCompare( ll, false );
                    if ( ( oo.number() < 0 ) ^ ( direction < 0 ) ) {
                        y = -y;
                    }
                    if ( y < 0 ) {
                        advanceTo( currKey(), i, startKey );
                        return true;
                    }
                }
            }
                
            if ( eq && !endKeyInclusive_ ) {
                bucket = DiskLoc();
            }
        }
        return false;
    }

    void BtreeCursor::advanceTo( const BSONObj &keyBegin, int keyBeginLen, const BSONObj &keyEnd) {
        bucket.btree()->advanceTo( indexDetails, bucket, keyOfs, keyBegin, keyBeginLen, keyEnd, _ordering, direction );
    }
    
    bool BtreeCursor::advance() {
        killCurrentOp.checkForInterrupt();
        if ( bucket.isNull() )
            return false;
        bucket = bucket.btree()->advance(bucket, keyOfs, direction, "BtreeCursor::advance");
        skipAndCheck();
        if( !ok() && ++boundIndex_ < bounds_.size() )
            initInterval();
        return !bucket.isNull();
    }

    void BtreeCursor::noteLocation() {
        if ( !eof() ) {
            BSONObj o = bucket.btree()->keyAt(keyOfs).copy();
            keyAtKeyOfs = o;
            locAtKeyOfs = bucket.btree()->k(keyOfs).recordLoc;
        }
    }

    /* Since the last noteLocation(), our key may have moved around, and that old cached
       information may thus be stale and wrong (although often it is right).  We check
       that here; if we have moved, we have to search back for where we were at.

       i.e., after operations on the index, the BtreeCursor's cached location info may
       be invalid.  This function ensures validity, so you should call it before using
       the cursor if other writers have used the database since the last noteLocation
       call.
    */
    void BtreeCursor::checkLocation() {
        if ( eof() )
            return;

        multikey = d->isMultikey(idxNo);

        if ( keyOfs >= 0 ) {
            BtreeBucket *b = bucket.btree();

            assert( !keyAtKeyOfs.isEmpty() );

            // Note keyAt() returns an empty BSONObj if keyOfs is now out of range,
            // which is possible as keys may have been deleted.
            int x = 0;
            while( 1 ) {
                if ( b->keyAt(keyOfs).woEqual(keyAtKeyOfs) &&
                    b->k(keyOfs).recordLoc == locAtKeyOfs ) {
                        if ( !b->k(keyOfs).isUsed() ) {
                            /* we were deleted but still exist as an unused
                            marker key. advance.
                            */
                            skipUnusedKeys();
                        }
                        return;
                }

                /* we check one key earlier too, in case a key was just deleted.  this is 
                   important so that multi updates are reasonably fast.
                   */
                if( keyOfs == 0 || x++ )
                    break;
                keyOfs--;
            }
        }

        /* normally we don't get to here.  when we do, old position is no longer
            valid and we must refind where we left off (which is expensive)
        */

        bool found;

        /* TODO: Switch to keep indexdetails and do idx.head! */
        bucket = indexDetails.head.btree()->locate(indexDetails, indexDetails.head, keyAtKeyOfs, _ordering, keyOfs, found, locAtKeyOfs, direction);
        RARELY log() << "  key seems to have moved in the index, refinding. found:" << found << endl;
        if ( ! bucket.isNull() )
            skipUnusedKeys();

    }

    /* ----------------------------------------------------------------------------- */

    struct BtreeCursorUnitTest {
        BtreeCursorUnitTest() {
            assert( minDiskLoc.compare(maxDiskLoc) < 0 );
        }
    } btut;

} // namespace mongo
