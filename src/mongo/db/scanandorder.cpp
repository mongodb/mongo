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

#include "mongo/pch.h"

#include "mongo/db/scanandorder.h"

#include "mongo/db/matcher.h"
#include "mongo/db/parsed_query.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    const unsigned ScanAndOrder::MaxScanAndOrderBytes = 32 * 1024 * 1024;

    void ScanAndOrder::add(const BSONObj& o, const DiskLoc* loc) {
        verify( o.isValid() );
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
            _add(k, o, loc);
            return;
        }
        BestMap::iterator i;
        verify( _best.end() != _best.begin() );
        i = _best.end();
        i--;
        _addIfBetter(k, o, i, loc);
    }


    void ScanAndOrder::fill( BufBuilder& b, const ParsedQuery *parsedQuery, int& nout ) const {
        int n = 0;
        int nFilled = 0;
        Projection *projection = parsedQuery ? parsedQuery->getFields() : NULL;
        scoped_ptr<Matcher> arrayMatcher;
        scoped_ptr<MatchDetails> details;
        if ( projection && projection->getArrayOpType() == Projection::ARRAY_OP_POSITIONAL ) {
            // the projection specified an array positional match operator; create a new matcher
            // for the projected array
            arrayMatcher.reset( new Matcher( parsedQuery->getFilter() ) );
            details.reset( new MatchDetails );
            details->requestElemMatchKey();
        }
        for ( BestMap::const_iterator i = _best.begin(); i != _best.end(); i++ ) {
            n++;
            if ( n <= _startFrom )
                continue;
            const BSONObj& o = i->second;
            massert( 16355, "positional operator specified, but no array match",
                     ! arrayMatcher || arrayMatcher->matches( o, details.get() ) );
            fillQueryResultFromObj( b, projection, o, details.get() );
            nFilled++;
            if ( nFilled >= _limit )
                break;
        }
        nout = nFilled;
    }

    void ScanAndOrder::_add(const BSONObj& k, const BSONObj& o, const DiskLoc* loc) {
        BSONObj docToReturn = o;
        if ( loc ) {
            BSONObjBuilder b;
            b.appendElements(o);
            b.append("$diskLoc", loc->toBSONObj());
            docToReturn = b.obj();
        }
        _validateAndUpdateApproxSize( k.objsize() + docToReturn.objsize() );
        _best.insert(make_pair(k.getOwned(),docToReturn.getOwned()));
    }
    
    void ScanAndOrder::_addIfBetter(const BSONObj& k, const BSONObj& o, const BestMap::iterator& i,
                                    const DiskLoc* loc) {
        const BSONObj& worstBestKey = i->first;
        int cmp = worstBestKey.woCompare(k, _order._spec.keyPattern);
        if ( cmp > 0 ) {
            // k is better, 'upgrade'
            _validateAndUpdateApproxSize( -i->first.objsize() + -i->second.objsize() );
            _best.erase(i);
            _add(k, o, loc);
        }
    }

    void ScanAndOrder::_validateAndUpdateApproxSize( const int approxSizeDelta ) {
        // note : adjust when bson return limit adjusts. note this limit should be a bit higher.
        int newApproxSize = _approxSize + approxSizeDelta;
        verify( newApproxSize >= 0 );
        uassert( ScanAndOrderMemoryLimitExceededAssertionCode,
                "too much data for sort() with no index.  add an index or specify a smaller limit",
                (unsigned)newApproxSize < MaxScanAndOrderBytes );
        _approxSize = newApproxSize;
    }

} // namespace mongo
