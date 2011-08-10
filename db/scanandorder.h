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

#include "indexkey.h"
#include "queryutil.h"
#include "projection.h"

namespace mongo {

    /* todo:
       _ limit amount of data
    */

    class KeyType : boost::noncopyable {
    public:
        IndexSpec _spec;
        FieldRangeVector _keyCutter;
    public:
        KeyType(BSONObj pattern, const FieldRangeSet &frs):
        _spec((assert(!pattern.isEmpty()),pattern)),
        _keyCutter(frs, _spec, 1) {
        }

        /**
         * @return first key of the object that would be encountered while
         * scanning index with keySpec 'pattern' using constraints 'frs', or
         * BSONObj() if no such key.
         */
        BSONObj getKeyFromObject(BSONObj o) {
            return _keyCutter.firstMatch(o);
        }
    };

    /* todo:
       _ respect limit
       _ check for excess mem usage
       _ response size limit from runquery; push it up a bit.
    */

    inline void fillQueryResultFromObj(BufBuilder& bb, Projection *filter, const BSONObj& js, DiskLoc* loc=NULL) {
        if ( filter ) {
            BSONObjBuilder b( bb );
            filter->transform( js , b );
            if (loc)
                b.append("$diskLoc", loc->toBSONObj());
            b.done();
        }
        else if (loc) {
            BSONObjBuilder b( bb );
            b.appendElements(js);
            b.append("$diskLoc", loc->toBSONObj());
            b.done();
        }
        else {
            bb.appendBuf((void*) js.objdata(), js.objsize());
        }
    }

    typedef multimap<BSONObj,BSONObj,BSONObjCmp> BestMap;
    class ScanAndOrder {
    public:
        static const unsigned MaxScanAndOrderBytes;

        ScanAndOrder(int startFrom, int limit, BSONObj order, const FieldRangeSet &frs) :
            _best( BSONObjCmp( order ) ),
            _startFrom(startFrom), _order(order, frs) {
            _limit = limit > 0 ? limit + _startFrom : 0x7fffffff;
            _approxSize = 0;
        }

        int size() const { return _best.size(); }

        void add(BSONObj o, DiskLoc* loc);

        /* scanning complete. stick the query result in b for n objects. */
        void fill(BufBuilder& b, Projection *filter, int& nout ) const;

    private:

        void _add(BSONObj& k, BSONObj o, DiskLoc* loc);

        void _addIfBetter(BSONObj& k, BSONObj o, BestMap::iterator i, DiskLoc* loc);

        BestMap _best; // key -> full object
        int _startFrom;
        int _limit;   // max to send back.
        KeyType _order;
        unsigned _approxSize;

    };

} // namespace mongo
