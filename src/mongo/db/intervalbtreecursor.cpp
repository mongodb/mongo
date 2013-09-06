/**
 *    Copyright (C) 2012 10gen Inc.
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

#include "mongo/db/intervalbtreecursor.h"

#include "mongo/db/btree.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/namespace_details-inl.h"
#include "mongo/db/pdfile.h"

namespace mongo {

    unordered_set<IntervalBtreeCursor*> IntervalBtreeCursor::_activeCursors;
    SimpleMutex IntervalBtreeCursor::_activeCursorsMutex("active_interval_btree_cursors");

    /**
     * Advance 'loc' until it does not reference an unused key, or the end of the btree is reached.
     */
    static void skipUnused( BtreeKeyLocation* loc ) {

        // While loc points to an unused key ...
        while( !loc->bucket.isNull() &&
               loc->bucket.btree<V1>()->k( loc->pos ).isUnused() ) {

            // ... advance loc to the next key in the btree.
            loc->bucket = loc->bucket.btree<V1>()->advance( loc->bucket,
                                                            loc->pos,
                                                            1,
                                                            __FUNCTION__ );
        }
    }

    IntervalBtreeCursor* IntervalBtreeCursor::make( NamespaceDetails* namespaceDetails,
                                                    const IndexDetails& indexDetails,
                                                    const BSONObj& lowerBound,
                                                    bool lowerBoundInclusive,
                                                    const BSONObj& upperBound,
                                                    bool upperBoundInclusive ) {
        if ( indexDetails.version() != 1 ) {
            // Only v1 indexes are supported.
            return NULL;
        }
        auto_ptr<IntervalBtreeCursor> ret( new IntervalBtreeCursor( namespaceDetails,
                                                                    indexDetails,
                                                                    lowerBound,
                                                                    lowerBoundInclusive,
                                                                    upperBound,
                                                                    upperBoundInclusive ) );
        ret->init();
        return ret.release();
    }

    IntervalBtreeCursor::IntervalBtreeCursor( NamespaceDetails* namespaceDetails,
                                              const IndexDetails& indexDetails,
                                              const BSONObj& lowerBound,
                                              bool lowerBoundInclusive,
                                              const BSONObj& upperBound,
                                              bool upperBoundInclusive ) :
        _namespaceDetails( *namespaceDetails ),
        _indexNo( namespaceDetails->idxNo( indexDetails ) ),
        _indexDetails( indexDetails ),
        _ordering( Ordering::make( _indexDetails.keyPattern() ) ),
        _lowerBound( lowerBound ),
        _lowerBoundInclusive( lowerBoundInclusive ),
        _upperBound( upperBound ),
        _upperBoundInclusive( upperBoundInclusive ),
        _currRecoverable( _indexDetails, _ordering, _curr ),
        _nscanned(),
        _multikeyFlag() {

        SimpleMutex::scoped_lock lock(_activeCursorsMutex);
        _activeCursors.insert(this);
    }

    IntervalBtreeCursor::~IntervalBtreeCursor() {
        SimpleMutex::scoped_lock lock(_activeCursorsMutex);
        _activeCursors.erase(this);
    }

    void IntervalBtreeCursor::aboutToDeleteBucket(const DiskLoc& bucket) {
        SimpleMutex::scoped_lock lock(_activeCursorsMutex);
        for (unordered_set<IntervalBtreeCursor*>::iterator i = _activeCursors.begin();
             i != _activeCursors.end(); ++i) {

            IntervalBtreeCursor* ic = *i;
            if (bucket == ic->_curr.bucket) {
                ic->_currRecoverable.invalidateInitialLocation();
            }
        }
    }

    void IntervalBtreeCursor::init() {
        _multikeyFlag = _namespaceDetails.isMultikey( _indexNo );
        _curr = locateKey( _lowerBound, !_lowerBoundInclusive );
        skipUnused( &_curr );
        relocateEnd();
        if ( ok() ) {
            _nscanned = 1;
        }
    }

    bool IntervalBtreeCursor::ok() {
        return !_curr.bucket.isNull();
    }

    DiskLoc IntervalBtreeCursor::currLoc() {
        if ( eof() ) {
            return DiskLoc();
        }
        return _curr.bucket.btree<V1>()->keyNode( _curr.pos ).recordLoc;
    }

    bool IntervalBtreeCursor::advance() {
        RARELY killCurrentOp.checkForInterrupt();
        if ( eof() ) {
            return false;
        }
        // Advance _curr to the next key in the btree.
        _curr.bucket = _curr.bucket.btree<V1>()->advance( _curr.bucket,
                                                          _curr.pos,
                                                          1,
                                                          __FUNCTION__ );
        skipUnused( &_curr );
        if ( _curr == _end ) {
            // _curr has reached _end, so iteration is complete.
            _curr.bucket.Null();
        }
        else {
            ++_nscanned;
        }
        return ok();
    }

    BSONObj IntervalBtreeCursor::currKey() const {
        if ( _curr.bucket.isNull() ) {
            return BSONObj();
        }
        return _curr.bucket.btree<V1>()->keyNode( _curr.pos ).key.toBson();
    }


    void IntervalBtreeCursor::noteLocation() {
        _currRecoverable = LogicalBtreePosition( _indexDetails, _ordering, _curr );
        _currRecoverable.init();
    }

    void IntervalBtreeCursor::checkLocation() {
        _multikeyFlag = _namespaceDetails.isMultikey( _indexNo );
        _curr = _currRecoverable.currentLocation();
        skipUnused( &_curr );
        relocateEnd();
    }

    bool IntervalBtreeCursor::getsetdup( DiskLoc loc ) {
        // TODO _multikeyFlag may be set part way through an iteration by checkLocation().  In this
        // case results returned earlier, when _multikeyFlag was false, will not be deduped.  This
        // is an old issue with all mongo btree cursor implementations.
        return _multikeyFlag && !_dups.insert( loc ).second;
    }

    BSONObj IntervalBtreeCursor::prettyIndexBounds() const {
        return BSON( "lower" << _lowerBound.replaceFieldNames( _indexDetails.keyPattern() ) <<
                     "upper" << _upperBound.replaceFieldNames( _indexDetails.keyPattern() ) );
    }

    BtreeKeyLocation IntervalBtreeCursor::locateKey( const BSONObj& key, bool afterKey ) {
        bool found;
        BtreeKeyLocation ret;

        // To find the first btree location equal to the specified key, specify a record location of
        // minDiskLoc, which is below any actual Record location.  To find the first btree location
        // greater than the specified key, specify a record location of maxDiskLoc, which is above
        // any actual Record location.
        DiskLoc targetRecord = afterKey ? maxDiskLoc : minDiskLoc;

        // Find the requested location in the btree.
        ret.bucket = _indexDetails.head.btree<V1>()->locate( _indexDetails,
                                                             _indexDetails.head,
                                                             key,
                                                             _ordering,
                                                             ret.pos,
                                                             found,
                                                             targetRecord,
                                                             1 );
        return ret;
    }

    void IntervalBtreeCursor::relocateEnd() {
        if ( eof() ) {
            return;
        }

        // If the current key is above the upper bound ...
        int32_t cmp = currKey().woCompare( _upperBound, _ordering, false );
        if ( cmp > 0 || ( cmp == 0 && !_upperBoundInclusive ) ) {

            // ... then iteration is complete.
            _curr.bucket.Null();
            return;
        }

        // Otherwise, relocate _end.
        _end = locateKey( _upperBound, _upperBoundInclusive );
        skipUnused( &_end );
    }

} // namespace mongo
