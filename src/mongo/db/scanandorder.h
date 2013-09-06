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

#pragma once

#include "mongo/db/diskloc.h"
#include "mongo/db/projection.h"
#include "mongo/db/queryutil.h"

namespace mongo {

    class ParsedQuery;

    static const int ScanAndOrderMemoryLimitExceededAssertionCode = 10128;

    class KeyType : boost::noncopyable {
    public:
        BSONObj _keyPattern;
        FieldRangeVector _keyCutter;
    public:
        KeyType(const BSONObj &pattern, const FieldRangeSet &frs)
            : _keyPattern(pattern), _keyCutter(frs, pattern, 1) {
            verify(!pattern.isEmpty());
        }

        /**
         * @return first key of the object that would be encountered while
         * scanning an index with keySpec 'pattern' using constraints 'frs', or
         * BSONObj() if no such key.
         */
        BSONObj getKeyFromObject(const BSONObj &o) const {
            return _keyCutter.firstMatch(o);
        }
    };

    /* todo:
       _ response size limit from runquery; push it up a bit.
    */

    inline void fillQueryResultFromObj(BufBuilder& bb, const Projection *filter, const BSONObj& js,
                                       const MatchDetails* details = NULL,
                                       const DiskLoc* loc=NULL) {
        if ( filter ) {
            BSONObjBuilder b( bb );
            filter->transform( js , b, details );
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

        ScanAndOrder(int startFrom, int limit, const BSONObj &order, const FieldRangeSet &frs) :
            _best( BSONObjCmp( order ) ),
            _startFrom(startFrom), _order(order, frs) {
            _limit = limit > 0 ? limit + _startFrom : 0x7fffffff;
            _approxSize = 0;
        }

        int size() const { return _best.size(); }

        /**
         * @throw ScanAndOrderMemoryLimitExceededAssertionCode if adding would grow memory usage
         * to ScanAndOrder::MaxScanAndOrderBytes.
         */
        void add(const BSONObj &o, const DiskLoc* loc);

        /* scanning complete. stick the query result in b for n objects. */
        void fill(BufBuilder& b, const ParsedQuery *query, int& nout) const;

    /** Functions for testing. */
    protected:

        unsigned approxSize() const { return _approxSize; }

    private:

        void _add(const BSONObj& k, const BSONObj& o, const DiskLoc* loc);

        void _addIfBetter(const BSONObj& k, const BSONObj& o, const BestMap::iterator& i,
                          const DiskLoc* loc);

        /**
         * @throw ScanAndOrderMemoryLimitExceededAssertionCode if approxSize would grow too high,
         * otherwise update _approxSize.
         */
        void _validateAndUpdateApproxSize( const int approxSizeDelta );

        BestMap _best; // key -> full object
        int _startFrom;
        int _limit;   // max to send back.
        KeyType _order;
        unsigned _approxSize;

    };

} // namespace mongo
