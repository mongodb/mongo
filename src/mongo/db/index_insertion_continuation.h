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

#include "mongo/db/diskloc.h"
#include "mongo/db/jsobj.h"

namespace mongo {

    class IndexDetails;
    template <typename V> class BtreeBucket;

    /**
     * This class represents the write phase of the two-phase index insertion.
     */
    class IndexInsertionContinuation : private boost::noncopyable {
    public:
        enum Op { Nothing, SetUsed, InsertHere };

        virtual ~IndexInsertionContinuation();
        virtual void doIndexInsertionWrites() const = 0;
    };

    template< class V >
    struct IndexInsertionContinuationImpl : public IndexInsertionContinuation {

        IndexInsertionContinuationImpl(DiskLoc thisLoc, DiskLoc _recordLoc, const BSONObj &_key,
                                       Ordering _order, IndexDetails& _idx) :
            bLoc(thisLoc), recordLoc(_recordLoc), key(_key), order(_order), idx(_idx) {
            op = Nothing;
        }

        DiskLoc bLoc;
        DiskLoc recordLoc;
        typename V::KeyOwned key;
        const Ordering order;
        IndexDetails& idx;
        Op op;

        int pos;
        const BtreeBucket<V> *b;

        void doIndexInsertionWrites() const {
            if( op == Nothing )
                return;
            else if( op == SetUsed ) {
                const typename V::_KeyNode& kn = b->k(pos);
                kn.writing().setUsed();
            }
            else {
                b->insertHere(bLoc, pos, recordLoc, key, order, DiskLoc(), DiskLoc(), idx);
            }
        }
    };


}  // namespace mongo
