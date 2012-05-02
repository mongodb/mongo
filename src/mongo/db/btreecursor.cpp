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
#include "curop-inl.h"
#include "queryutil.h"

namespace mongo {

    template< class V >
    class BtreeCursorImpl : public BtreeCursor { 
    public:
        typedef typename BucketBasics<V>::KeyNode KeyNode;
        typedef typename V::Key Key;
        typedef typename V::_KeyNode _KeyNode;

        BtreeCursorImpl(NamespaceDetails *a, int b, const IndexDetails& c, const BSONObj &d, const BSONObj &e, bool f, int g) : 
          BtreeCursor(a,b,c,d,e,f,g) { }
        BtreeCursorImpl(NamespaceDetails *_d, int _idxNo, const IndexDetails& _id,
                        const shared_ptr< FieldRangeVector > &_bounds, int singleIntervalLimit,
                        int _direction ) :
          BtreeCursor(_d,_idxNo,_id,_bounds,singleIntervalLimit,_direction )
        { 
            pair< DiskLoc, int > noBestParent;
            indexDetails.head.btree<V>()->customLocate( bucket, keyOfs, startKey, 0, false, _boundsIterator->cmp(), _boundsIterator->inc(), _ordering, _direction, noBestParent );
            skipAndCheck();
            dassert( _dups.size() == 0 );
        }

        virtual DiskLoc currLoc() { 
            if( bucket.isNull() ) return DiskLoc();
            return currKeyNode().recordLoc;
        }

        virtual BSONObj keyAt(int ofs) const { 
            verify( !bucket.isNull() );
            const BtreeBucket<V> *b = bucket.btree<V>();
            int n = b->getN();
            if( n == b->INVALID_N_SENTINEL ) {
                throw UserException(15850, "keyAt bucket deleted");
            }
            dassert( n >= 0 && n < 10000 );
            return ofs >= n ? BSONObj() : b->keyNode(ofs).key.toBson();
        }

        virtual BSONObj currKey() const { 
            verify( !bucket.isNull() );
            return bucket.btree<V>()->keyNode(keyOfs).key.toBson();
        }

        virtual bool curKeyHasChild() { 
            return !currKeyNode().prevChildBucket.isNull();
        }

        bool skipUnusedKeys() {
            int u = 0;
            while ( 1 ) {
                if ( !ok() )
                    break;
                const _KeyNode& kn = keyNode(keyOfs);
                if ( kn.isUsed() )
                    break;
                bucket = _advance(bucket, keyOfs, _direction, "skipUnusedKeys");
                u++;
                //don't include unused keys in nscanned
                //++_nscanned;
            }
            if ( u > 10 )
                OCCASIONALLY log() << "btree unused skipped:" << u << '\n';
            return u;
        }

        /* Since the last noteLocation(), our key may have moved around, and that old cached
           information may thus be stale and wrong (although often it is right).  We check
           that here; if we have moved, we have to search back for where we were at.

           i.e., after operations on the index, the BtreeCursor's cached location info may
           be invalid.  This function ensures validity, so you should call it before using
           the cursor if other writers have used the database since the last noteLocation
           call.
        */
        void checkLocation() {
            if ( eof() )
                return;

            _multikey = d->isMultikey(idxNo);

            if ( keyOfs >= 0 ) {
                verify( !keyAtKeyOfs.isEmpty() );

                try {
                    // Note keyAt() returns an empty BSONObj if keyOfs is now out of range,
                    // which is possible as keys may have been deleted.
                    int x = 0;
                    while( 1 ) {
                        //  if ( b->keyAt(keyOfs).woEqual(keyAtKeyOfs) &&
                        //       b->k(keyOfs).recordLoc == locAtKeyOfs ) {
                        if ( keyAt(keyOfs).binaryEqual(keyAtKeyOfs) ) {
                            const _KeyNode& kn = keyNode(keyOfs);
                            if( kn.recordLoc == locAtKeyOfs ) {
                                if ( !kn.isUsed() ) {
                                    // we were deleted but still exist as an unused
                                    // marker key. advance.
                                    skipUnusedKeys();
                                }
                                return;
                            }
                        }

                        // we check one key earlier too, in case a key was just deleted.  this is
                        // important so that multi updates are reasonably fast.
                        if( keyOfs == 0 || x++ )
                            break;
                        keyOfs--;
                    }
                }
                catch(UserException& e) { 
                    if( e.getCode() != 15850 )
                        throw;
                    // hack: fall through if bucket was just deleted. should only happen under deleteObjects()
                    DEV log() << "debug info: bucket was deleted" << endl;
                }
            }

            /* normally we don't get to here.  when we do, old position is no longer
                valid and we must refind where we left off (which is expensive)
            */

            /* TODO: Switch to keep indexdetails and do idx.head! */
            bucket = _locate(keyAtKeyOfs, locAtKeyOfs);
            RARELY log() << "key seems to have moved in the index, refinding. " << bucket.toString() << endl;
            if ( ! bucket.isNull() )
                skipUnusedKeys();

        }
    
    protected:
        virtual void _advanceTo(DiskLoc &thisLoc, int &keyOfs, const BSONObj &keyBegin, int keyBeginLen, bool afterKey, const vector< const BSONElement * > &keyEnd, const vector< bool > &keyEndInclusive, const Ordering &order, int direction ) {
            thisLoc.btree<V>()->advanceTo(thisLoc, keyOfs, keyBegin, keyBeginLen, afterKey, keyEnd, keyEndInclusive, order, direction);
        }
        virtual DiskLoc _advance(const DiskLoc& thisLoc, int& keyOfs, int direction, const char *caller) {
            return thisLoc.btree<V>()->advance(thisLoc, keyOfs, direction, caller);
        }
        virtual void _audit() {
            out() << "BtreeCursor(). dumping head bucket" << endl;
            indexDetails.head.btree<V>()->dump();
        }
        virtual DiskLoc _locate(const BSONObj& key, const DiskLoc& loc) {
            bool found;
            return indexDetails.head.btree<V>()->
                     locate(indexDetails, indexDetails.head, key, _ordering, keyOfs, found, loc, _direction);
        }

        const _KeyNode& keyNode(int keyOfs) const { 
            return bucket.btree<V>()->k(keyOfs);
        }

    private:
        const KeyNode currKeyNode() const {
            verify( !bucket.isNull() );
            const BtreeBucket<V> *b = bucket.btree<V>();
            return b->keyNode(keyOfs);
        }
    };

    template class BtreeCursorImpl<V0>;
    template class BtreeCursorImpl<V1>;

    /*
    class BtreeCursorV1 : public BtreeCursor { 
    public:
        typedef BucketBasics<V1>::KeyNode KeyNode;
        typedef V1::Key Key;

        BtreeCursorV1(NamespaceDetails *a, int b, const IndexDetails& c, const BSONObj &d, const BSONObj &e, bool f, int g) : 
          BtreeCursor(a,b,c,d,e,f,g) { }
        BtreeCursorV1(NamespaceDetails *_d, int _idxNo, const IndexDetails& _id, const shared_ptr< FieldRangeVector > &_bounds, int _direction) : 
          BtreeCursor(_d,_idxNo,_id,_bounds,_direction) 
        { 
            pair< DiskLoc, int > noBestParent;
            indexDetails.head.btree<V1>()->customLocate( bucket, keyOfs, startKey, 0, false, _boundsIterator->cmp(), _boundsIterator->inc(), _ordering, _direction, noBestParent );
            skipAndCheck();
            dassert( _dups.size() == 0 );
        }

        virtual DiskLoc currLoc() { 
            if( bucket.isNull() ) return DiskLoc();
            return currKeyNode().recordLoc;
        }

        virtual BSONObj currKey() const { 
            verify( !bucket.isNull() );
            return bucket.btree<V1>()->keyNode(keyOfs).key.toBson();
        }

    protected:
        virtual void _advanceTo(DiskLoc &thisLoc, int &keyOfs, const BSONObj &keyBegin, int keyBeginLen, bool afterKey, const vector< const BSONElement * > &keyEnd, const vector< bool > &keyEndInclusive, const Ordering &order, int direction ) {
            thisLoc.btree<V1>()->advanceTo(thisLoc, keyOfs, keyBegin, keyBeginLen, afterKey, keyEnd, keyEndInclusive, order, direction);
        }
        virtual DiskLoc _advance(const DiskLoc& thisLoc, int& keyOfs, int direction, const char *caller) {
            return thisLoc.btree<V1>()->advance(thisLoc, keyOfs, direction, caller);
        }
        virtual void _audit() {
            out() << "BtreeCursor(). dumping head bucket" << endl;
            indexDetails.head.btree<V1>()->dump();
        }
        virtual DiskLoc _locate(const BSONObj& key, const DiskLoc& loc);
        virtual const _KeyNode& keyNode(int keyOfs) { 
            return bucket.btree<V1>()->k(keyOfs);
        }

    private:
        const KeyNode currKeyNode() const {
            verify( !bucket.isNull() );
            const BtreeBucket<V1> *b = bucket.btree<V1>();
            return b->keyNode(keyOfs);
        }
    };*/

    BtreeCursor* BtreeCursor::make(
        NamespaceDetails *_d, const IndexDetails& _id,
        const shared_ptr< FieldRangeVector > &_bounds, int _direction )
    {
        return make( _d, _d->idxNo( (IndexDetails&) _id), _id, _bounds, 0, _direction );
    }

    BtreeCursor* BtreeCursor::make(
        NamespaceDetails *_d, const IndexDetails& _id,
        const BSONObj &startKey, const BSONObj &endKey, bool endKeyInclusive, int direction)
    {
        return make( _d, _d->idxNo( (IndexDetails&) _id), _id, startKey, endKey, endKeyInclusive, direction );
    }


    BtreeCursor* BtreeCursor::make(
        NamespaceDetails *_d, int _idxNo, const IndexDetails& _id, 
        const BSONObj &startKey, const BSONObj &endKey, bool endKeyInclusive, int direction) 
    { 
        int v = _id.version();
        BtreeCursor *c = 0;
        if( v == 1 ) {
            c = new BtreeCursorImpl<V1>(_d,_idxNo,_id,startKey,endKey,endKeyInclusive,direction);
        }
        else if( v == 0 ) {
            c = new BtreeCursorImpl<V0>(_d,_idxNo,_id,startKey,endKey,endKeyInclusive,direction);
        }
        else {
            uasserted(14800, str::stream() << "unsupported index version " << v);
        }
        c->initWithoutIndependentFieldRanges();
        dassert( c->_dups.size() == 0 );
        return c;
    }

    BtreeCursor* BtreeCursor::make(
        NamespaceDetails *_d, int _idxNo, const IndexDetails& _id, 
        const shared_ptr< FieldRangeVector > &_bounds, int singleIntervalLimit, int _direction )
    {
        int v = _id.version();
        if( v == 1 )
            return new BtreeCursorImpl<V1>(_d,_idxNo,_id,_bounds,singleIntervalLimit,_direction);
        if( v == 0 )
            return new BtreeCursorImpl<V0>(_d,_idxNo,_id,_bounds,singleIntervalLimit,_direction);
        uasserted(14801, str::stream() << "unsupported index version " << v);

        // just check we are in sync with this method
        dassert( IndexDetails::isASupportedIndexVersionNumber(v) );

        return 0;
    }

    BtreeCursor::BtreeCursor( NamespaceDetails *_d, int _idxNo, const IndexDetails &_id,
                              const BSONObj &_startKey, const BSONObj &_endKey, bool endKeyInclusive, int _direction ) :
        d(_d), idxNo(_idxNo),
        startKey( _startKey ),
        endKey( _endKey ),
        _endKeyInclusive( endKeyInclusive ),
        _multikey( d->isMultikey( idxNo ) ),
        indexDetails( _id ),
        _order( _id.keyPattern() ),
        _ordering( Ordering::make( _order ) ),
        _direction( _direction ),
        _independentFieldRanges( false ),
        _nscanned( 0 ) {
        audit();
    }

    BtreeCursor::BtreeCursor( NamespaceDetails *_d, int _idxNo, const IndexDetails& _id,
                             const shared_ptr< FieldRangeVector > &_bounds, int singleIntervalLimit,
                             int _direction )
        :
        d(_d), idxNo(_idxNo),
        _endKeyInclusive( true ),
        _multikey( d->isMultikey( idxNo ) ),
        indexDetails( _id ),
        _order( _id.keyPattern() ),
        _ordering( Ordering::make( _order ) ),
        _direction( _direction ),
        _bounds( ( verify( _bounds.get() ), _bounds ) ),
        _boundsIterator( new FieldRangeVectorIterator( *_bounds, singleIntervalLimit ) ),
        _independentFieldRanges( true ),
        _nscanned( 0 ) {
        audit();
        startKey = _bounds->startKey();
        _boundsIterator->advance( startKey ); // handles initialization
        _boundsIterator->prepDive();
        bucket = indexDetails.head;
        keyOfs = 0;
    }

    /** Properly destroy forward declared class members. */
    BtreeCursor::~BtreeCursor() {}
    
    void BtreeCursor::audit() {
        dassert( d->idxNo((IndexDetails&) indexDetails) == idxNo );
    }

    void BtreeCursor::initWithoutIndependentFieldRanges() {
        if ( indexDetails.getSpec().getType() ) {
            startKey = indexDetails.getSpec().getType()->fixKey( startKey );
            endKey = indexDetails.getSpec().getType()->fixKey( endKey );
        }
        bucket = _locate(startKey, _direction > 0 ? minDiskLoc : maxDiskLoc);
        if ( ok() ) {
            _nscanned = 1;
        }
        skipUnusedKeys();
        checkEnd();
    }

    void BtreeCursor::skipAndCheck() {
        long long startNscanned = _nscanned;
        skipUnusedKeys();
        while( 1 ) {
            if ( !skipOutOfRangeKeysAndCheckEnd() ) {
                break;
            }
            do {
                if ( _nscanned > startNscanned + 20 ) {
                    skipUnusedKeys();
                    return;
                }
            } while( skipOutOfRangeKeysAndCheckEnd() );
            if ( !skipUnusedKeys() ) {
                break;
            }
        }
    }

    bool BtreeCursor::skipOutOfRangeKeysAndCheckEnd() {
        if ( !ok() ) {
            return false;
        }
        int ret = _boundsIterator->advance( currKey() );
        if ( ret == -2 ) {
            bucket = DiskLoc();
            return false;
        }
        else if ( ret == -1 ) {
            ++_nscanned;
            return false;
        }
        ++_nscanned;
        advanceTo( currKey(), ret, _boundsIterator->after(), _boundsIterator->cmp(), _boundsIterator->inc() );
        return true;
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
            int cmp = sgn( endKey.woCompare( currKey(), _order ) );
            if ( ( cmp != 0 && cmp != _direction ) ||
                    ( cmp == 0 && !_endKeyInclusive ) )
                bucket = DiskLoc();
        }
    }

    void BtreeCursor::advanceTo( const BSONObj &keyBegin, int keyBeginLen, bool afterKey, const vector< const BSONElement * > &keyEnd, const vector< bool > &keyEndInclusive) {
        _advanceTo( bucket, keyOfs, keyBegin, keyBeginLen, afterKey, keyEnd, keyEndInclusive, _ordering, _direction );
    }

    bool BtreeCursor::advance() {
        killCurrentOp.checkForInterrupt();
        if ( bucket.isNull() )
            return false;

        bucket = _advance(bucket, keyOfs, _direction, "BtreeCursor::advance");

        if ( !_independentFieldRanges ) {
            skipUnusedKeys();
            checkEnd();
            if ( ok() ) {
                ++_nscanned;
            }
        }
        else {
            skipAndCheck();
        }
        return ok();
    }

    void BtreeCursor::noteLocation() {
        if ( !eof() ) {
            BSONObj o = currKey().getOwned();
            keyAtKeyOfs = o;
            locAtKeyOfs = currLoc();
        }
    }

    string BtreeCursor::toString() {
        string s = string("BtreeCursor ") + indexDetails.indexName();
        if ( _direction < 0 ) s += " reverse";
        if ( _bounds.get() && _bounds->size() > 1 ) s += " multi";
        return s;
    }
    
    BSONObj BtreeCursor::prettyIndexBounds() const {
        if ( !_independentFieldRanges ) {
            return BSON( "start" << prettyKey( startKey ) << "end" << prettyKey( endKey ) );
        }
        else {
            return _bounds->obj();
        }
    }    

    /* ----------------------------------------------------------------------------- */

    struct BtreeCursorUnitTest {
        BtreeCursorUnitTest() {
            verify( minDiskLoc.compare(maxDiskLoc) < 0 );
        }
    } btut;

} // namespace mongo
