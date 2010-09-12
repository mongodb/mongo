/* scanandorder.h
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

#pragma once

namespace mongo {

    /* todo:
       _ handle compound keys with differing directions.  we don't handle this yet: neither here nor in indexes i think!!!
       _ limit amount of data
    */

    /* see also IndexDetails::getKeysFromObject, which needs some merging with this. */

    class KeyType : boost::noncopyable {
    public:
        BSONObj pattern; // e.g., { ts : -1 }
    public:
        KeyType(BSONObj _keyPattern) {
            pattern = _keyPattern;
            assert( !pattern.isEmpty() );
        }

        // returns the key value for o
        BSONObj getKeyFromObject(BSONObj o) {
            return o.extractFields(pattern,true);
        }
    };

    /* todo:
       _ respect limit
       _ check for excess mem usage
       _ response size limit from runquery; push it up a bit.
    */

    inline void fillQueryResultFromObj(BufBuilder& bb, FieldMatcher *filter, BSONObj& js, DiskLoc* loc=NULL) {
        if ( filter ) {
            BSONObjBuilder b( bb );
            BSONObjIterator i( js );
            while ( i.more() ){
                BSONElement e = i.next();
                const char * fname = e.fieldName();
                
                if ( strcmp( fname , "_id" ) == 0 ){
                    if (filter->includeID())
                        b.append( e );
                } else {
                    filter->append( b , e );
                }
            }
            if (loc)
                b.append("$diskLoc", loc->toBSONObj());
            b.done();
        } else if (loc) {
            BSONObjBuilder b( bb );
            b.appendElements(js);
            b.append("$diskLoc", loc->toBSONObj());
            b.done();
        } else {
            bb.appendBuf((void*) js.objdata(), js.objsize());
        }
    }
    
    typedef multimap<BSONObj,BSONObj,BSONObjCmp> BestMap;
    class ScanAndOrder {
        BestMap best; // key -> full object
        int startFrom;
        int limit;   // max to send back.
        KeyType order;
        unsigned approxSize;

        void _add(BSONObj& k, BSONObj o, DiskLoc* loc) {
            if (!loc){
                best.insert(make_pair(k.getOwned(),o.getOwned()));
            } else {
                BSONObjBuilder b;
                b.appendElements(o);
                b.append("$diskLoc", loc->toBSONObj());
                best.insert(make_pair(k.getOwned(), b.obj().getOwned()));
            }
        }

        void _addIfBetter(BSONObj& k, BSONObj o, BestMap::iterator i, DiskLoc* loc) {
            /* todo : we don't correct approxSize here. */
            const BSONObj& worstBestKey = i->first;
            int c = worstBestKey.woCompare(k, order.pattern);
            if ( c > 0 ) {
                // k is better, 'upgrade'
                best.erase(i);
                _add(k, o, loc);
            }
        }

    public:
        ScanAndOrder(int _startFrom, int _limit, BSONObj _order) :
                best( BSONObjCmp( _order ) ),
                startFrom(_startFrom), order(_order) {
            limit = _limit > 0 ? _limit + startFrom : 0x7fffffff;
            approxSize = 0;
        }

        int size() const {
            return best.size();
        }

        void add(BSONObj o, DiskLoc* loc) {
            assert( o.isValid() );
            BSONObj k = order.getKeyFromObject(o);
            if ( (int) best.size() < limit ) {
                approxSize += k.objsize();
                approxSize += o.objsize();

                /* note : adjust when bson return limit adjusts. note this limit should be a bit higher. */
                uassert( 10128 ,  "too much data for sort() with no index.  add an index or specify a smaller limit", approxSize < 32 * 1024 * 1024 );

                _add(k, o, loc);
                return;
            }
            BestMap::iterator i;
            assert( best.end() != best.begin() );
            i = best.end();
            i--;
            _addIfBetter(k, o, i, loc);
        }

        void _fill(BufBuilder& b, FieldMatcher *filter, int& nout, BestMap::iterator begin, BestMap::iterator end) {
            int n = 0;
            int nFilled = 0;
            for ( BestMap::iterator i = begin; i != end; i++ ) {
                n++;
                if ( n <= startFrom )
                    continue;
                BSONObj& o = i->second;
                fillQueryResultFromObj(b, filter, o);
                nFilled++;
                if ( nFilled >= limit )
                    break;
                uassert( 10129 ,  "too much data for sort() with no index", b.len() < 4000000 ); // appserver limit
            }
            nout = nFilled;
        }

        /* scanning complete. stick the query result in b for n objects. */
        void fill(BufBuilder& b, FieldMatcher *filter, int& nout) {
            _fill(b, filter, nout, best.begin(), best.end());
        }

    };

} // namespace mongo
