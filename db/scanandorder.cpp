/* scanandorder.cpp
   Order results (that aren't already indexes and in order.)
*/

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
#include "scanandorder.h"

namespace mongo {

    const unsigned ScanAndOrder::MaxScanAndOrderBytes = 32 * 1024 * 1024;

    void ScanAndOrder::_add(BSONObj& k, BSONObj o, DiskLoc* loc) {
        if (!loc) {
            _best.insert(make_pair(k.getOwned(),o.getOwned()));
        }
        else {
            BSONObjBuilder b;
            b.appendElements(o);
            b.append("$diskLoc", loc->toBSONObj());
            _best.insert(make_pair(k.getOwned(), b.obj().getOwned()));
        }
    }

    void ScanAndOrder::_addIfBetter(BSONObj& k, BSONObj o, BestMap::iterator i, DiskLoc* loc) {
        /* todo : we don't correct _approxSize here. */
        const BSONObj& worstBestKey = i->first;
        int c = worstBestKey.woCompare(k, _order._spec.keyPattern);
        if ( c > 0 ) {
            // k is better, 'upgrade'
            _best.erase(i);
            _add(k, o, loc);
        }
    }


    void ScanAndOrder::add(BSONObj o, DiskLoc* loc) {
        assert( o.isValid() );
        BSONObj k;
        try {
            k = _order.getKeyFromObject(o);
        }
        catch (UserException &e) {
            if ( e.getCode() == ParallelArraysCode ) { // cannot get keys for parallel arrays
                // fix lasterror text to be more accurate.
                uasserted( 15925, "cannot sort with keys that are parallel arrays" );
            }
            else
                throw;
        }

        if ( k.isEmpty() ) {
            return;   
        }
        if ( (int) _best.size() < _limit ) {
            _approxSize += k.objsize();
            _approxSize += o.objsize();
            
            /* note : adjust when bson return limit adjusts. note this limit should be a bit higher. */
            uassert( 10128 ,  "too much data for sort() with no index.  add an index or specify a smaller limit", _approxSize < MaxScanAndOrderBytes );
            
            _add(k, o, loc);
            return;
        }
        BestMap::iterator i;
        assert( _best.end() != _best.begin() );
        i = _best.end();
        i--;
        _addIfBetter(k, o, i, loc);
    }


    void ScanAndOrder::fill(BufBuilder& b, Projection *filter, int& nout ) const {
        int n = 0;
        int nFilled = 0;
        for ( BestMap::const_iterator i = _best.begin(); i != _best.end(); i++ ) {
            n++;
            if ( n <= _startFrom )
                continue;
            const BSONObj& o = i->second;
            fillQueryResultFromObj(b, filter, o);
            nFilled++;
            if ( nFilled >= _limit )
                break;
            uassert( 10129 ,  "too much data for sort() with no index", b.len() < (int)MaxScanAndOrderBytes ); // appserver limit
        }
        nout = nFilled;
    }

} // namespace mongo
