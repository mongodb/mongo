// DEPRECATED
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

#include "mongo/db/btreecursor.h"

#include "mongo/db/kill_current_op.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/queryutil.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/btree_access_method.h"
#include "mongo/db/index/catalog_hack.h"

namespace mongo {

    BtreeCursor* BtreeCursor::make( NamespaceDetails * nsd , int idxNo , const IndexDetails& indexDetails ) {
        return new BtreeCursor( nsd , idxNo , indexDetails );
    }
    
    BtreeCursor* BtreeCursor::make( NamespaceDetails* namespaceDetails,
                                    const IndexDetails& id,
                                    const BSONObj& startKey,
                                    const BSONObj& endKey,
                                    bool endKeyInclusive,
                                    int direction ) {
        auto_ptr<BtreeCursor> c( make( namespaceDetails, namespaceDetails->idxNo( id ), id ) );
        c->init(startKey,endKey,endKeyInclusive,direction);
        c->initWithoutIndependentFieldRanges();
        dassert( c->_dups.size() == 0 );
        return c.release();
    }

    BtreeCursor* BtreeCursor::make( NamespaceDetails* namespaceDetails,
                                    const IndexDetails& id,
                                    const shared_ptr<FieldRangeVector>& bounds,
                                    int singleIntervalLimit,
                                    int direction )
    {
        auto_ptr<BtreeCursor> c( make( namespaceDetails, namespaceDetails->idxNo( id ), id ) );
        c->init(bounds,singleIntervalLimit,direction);
        return c.release();
    }

    BtreeCursor::BtreeCursor( NamespaceDetails* nsd, int theIndexNo, const IndexDetails& id ) :
        d( nsd ),
        idxNo( theIndexNo ),
        indexDetails( id ),
        _boundsMustMatch( true ),
        _nscanned() {
    }

    void BtreeCursor::_finishConstructorInit() {
        _multikey = d->isMultikey( idxNo );
        _order = indexDetails.keyPattern();
    }
    
    void BtreeCursor::init(const BSONObj& sk, const BSONObj& ek, bool endKeyInclusive,
                           int direction ) {
        _finishConstructorInit();
        startKey = sk;
        endKey = ek;
        _endKeyInclusive =  endKeyInclusive;
        _direction = direction;
        _independentFieldRanges = false;
        dassert( d->idxNo((IndexDetails&) indexDetails) == idxNo );

        _indexDescriptor.reset(CatalogHack::getDescriptor(d, idxNo));
        _indexAM.reset(CatalogHack::getBtreeIndex(_indexDescriptor.get()));

        IndexCursor *cursor;
        _indexAM->newCursor(&cursor);
        _indexCursor.reset(static_cast<BtreeIndexCursor*>(cursor));

        CursorOptions opts;
        opts.direction = _direction == 1 ? CursorOptions::INCREASING : CursorOptions::DECREASING;
        cursor->setOptions(opts);

        _hitEnd = false;
    }

    void BtreeCursor::init(  const shared_ptr< FieldRangeVector > &bounds, int singleIntervalLimit, int direction ) {
        _finishConstructorInit();
        _bounds = bounds;
        verify( _bounds );
        _direction = direction;
        _endKeyInclusive = true;
        _boundsIterator.reset( new FieldRangeVectorIterator( *_bounds , singleIntervalLimit ) );
        _independentFieldRanges = true;
        dassert( d->idxNo((IndexDetails&) indexDetails) == idxNo );
        startKey = _bounds->startKey();
        _boundsIterator->advance( startKey ); // handles initialization
        _boundsIterator->prepDive();

        _indexDescriptor.reset(CatalogHack::getDescriptor(d, idxNo));
        _indexAM.reset(CatalogHack::getBtreeIndex(_indexDescriptor.get()));

        IndexCursor *cursor;
        _indexAM->newCursor(&cursor);
        _indexCursor.reset(static_cast<BtreeIndexCursor*>(cursor));

        CursorOptions opts;
        opts.direction = _direction == 1 ? CursorOptions::INCREASING : CursorOptions::DECREASING;
        _indexCursor->setOptions(opts);

        _indexCursor->seek(_boundsIterator->cmp(), _boundsIterator->inc());
        _hitEnd = false;
        skipAndCheck();
        dassert( _dups.size() == 0 );
    }

    /** Properly destroy forward declared class members. */
    BtreeCursor::~BtreeCursor() {}

    DiskLoc BtreeCursor::currLoc() { 
        if (!ok()) {
            return DiskLoc();
        } else {
            return _indexCursor->getValue();
        }
    }

    const DiskLoc BtreeCursor::getBucket() const {
        return _indexCursor->getBucket();
    }

    int BtreeCursor::getKeyOfs() const {
        return _indexCursor->getKeyOfs();
    }

    BSONObj BtreeCursor::currKey() const { 
        return _indexCursor->getKey();
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
        if (eof()) return;
        _indexCursor->restorePosition();
        _multikey = d->isMultikey(idxNo);
    }
    
    void BtreeCursor::initWithoutIndependentFieldRanges() {
        _indexCursor->seek(startKey);
        if (ok()) { _nscanned = 1; }
        checkEnd();
    }

    void BtreeCursor::skipAndCheck() {
        long long startNscanned = _nscanned;
        if ( !skipOutOfRangeKeysAndCheckEnd() ) {
            return;
        }
        do {
            // If nscanned is increased by more than 20 before a matching key is found, abort
            // skipping through the btree to find a matching key.  This iteration cutoff
            // prevents unbounded internal iteration within BtreeCursor::init() and
            // BtreeCursor::advance() (the callers of skipAndCheck()).  See SERVER-3448.
            if ( _nscanned > startNscanned + 20 ) {
                //skipUnusedKeys();
                // If iteration is aborted before a key matching _bounds is identified, the
                // cursor may be left pointing at a key that is not within bounds
                // (_bounds->matchesKey( currKey() ) may be false).  Set _boundsMustMatch to
                // false accordingly.
                _boundsMustMatch = false;
                return;
            }
        } while( skipOutOfRangeKeysAndCheckEnd() );
    }

    bool BtreeCursor::skipOutOfRangeKeysAndCheckEnd() {
        if ( !ok() ) {
            return false;
        }
        int ret = _boundsIterator->advance( currKey() );
        if ( ret == -2 ) {
            _hitEnd = true;
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
        if (!ok()) { return; }

        if ( !endKey.isEmpty() ) {
            int cmp = sgn( endKey.woCompare( currKey(), _order ) );
            if ( ( cmp != 0 && cmp != _direction ) || ( cmp == 0 && !_endKeyInclusive ) ) {
                _hitEnd = true;
            }
        }
    }

    bool BtreeCursor::ok() {
        return !_indexCursor->isEOF() && !_hitEnd;
    }

    void BtreeCursor::advanceTo( const BSONObj &keyBegin, int keyBeginLen, bool afterKey, const vector< const BSONElement * > &keyEnd, const vector< bool > &keyEndInclusive) {
        _indexCursor->skip(keyBegin, keyBeginLen, afterKey, keyEnd, keyEndInclusive);
    }

    bool BtreeCursor::advance() {
        // Reset this flag at the start of a new iteration.
        _boundsMustMatch = true;

        killCurrentOp.checkForInterrupt();
        if (!ok()) {
            return false;
        }
        
        _indexCursor->next();
        
        if ( !_independentFieldRanges ) {
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
        if (!eof()) { _indexCursor->savePosition(); }
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

    bool BtreeCursor::currentMatches( MatchDetails* details ) {
        // If currKey() might not match the specified _bounds, check whether or not it does.
        if ( !_boundsMustMatch && _bounds && !_bounds->matchesKey( currKey() ) ) {
            // If the key does not match _bounds, it does not match the query.
            return false;
        }
        // Forward to the base class implementation, which may utilize a Matcher.
        return Cursor::currentMatches( details );
    }

    /* -------------------------- tests below -------------------------------------- */
    /* ----------------------------------------------------------------------------- */

    struct BtreeCursorUnitTest {
        BtreeCursorUnitTest() {
            verify( minDiskLoc.compare(maxDiskLoc) < 0 );
        }
    } btut;

} // namespace mongo
