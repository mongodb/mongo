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

#pragma once

#include "mongo/db/btreeposition.h"
#include "mongo/db/cursor.h"
#include "mongo/db/namespace_details.h"
#include "mongo/platform/cstdint.h"
#include "mongo/platform/unordered_set.h"

namespace mongo {

    /**
     * An optimized btree cursor that iterates through all btree keys between a lower bound and an
     * upper bound.  The contents of the individual keys are not examined by the implementation,
     * which simply advances through the tree until reaching a predetermined end location.  The goal
     * is to optimize count operations where the keys must only be counted, not tested for matching.
     *
     * Limitations compared to a standard BtreeCursor (partial list):
     * - Only supports index constraints consisting of a single interval within an index.
     * - Only supports forward direction iteration.
     * - Does not support covered index projections.
     * - Does not support get more.
     * - Only supports V1 indexes (not V0).
     */
    class IntervalBtreeCursor : public Cursor {
    public:

        /**
         * @return a cursor, or NULL if no cursor can be created.
         * @param namespaceDetails - Collection metadata that will not be modified.
         * @param indexDetails - Index metadata, if not a v1 index then make() will return NULL.
         * @param lowerBound - Lower bound of the key range to iterate, according to the index's
         *     native ordering.
         * @param lowerBoundInclusive - If true, the lower bound includes the endpoint.
         * @param upperBound - Upper bound of the key range to iterate.
         * @param upperBoundInclusive - If true, the upper bound includes the endpoint.
         */
        static IntervalBtreeCursor* make( /* const */ NamespaceDetails* namespaceDetails,
                                          const IndexDetails& indexDetails,
                                          const BSONObj& lowerBound,
                                          bool lowerBoundInclusive,
                                          const BSONObj& upperBound,
                                          bool upperBoundInclusive );

        /** Virtuals from Cursor. */

        virtual bool ok();

        virtual Record* _current() { return currLoc().rec(); }

        virtual BSONObj current() { return currLoc().obj(); }

        virtual DiskLoc currLoc();

        virtual bool advance();

        virtual BSONObj currKey() const;

        virtual DiskLoc refLoc() { return currLoc(); }

        static void aboutToDeleteBucket( const DiskLoc& b );

        virtual BSONObj indexKeyPattern() { return _indexDetails.keyPattern(); }

        virtual bool supportGetMore() { return false; }

        virtual void noteLocation();

        virtual void checkLocation();

        virtual bool supportYields() { return true; }

        virtual string toString() { return "IntervalBtreeCursor"; }

        virtual bool getsetdup( DiskLoc loc );

        virtual bool isMultiKey() const { return _multikeyFlag; }

        virtual bool modifiedKeys() const { return _multikeyFlag; }

        virtual BSONObj prettyIndexBounds() const;

        virtual long long nscanned() { return _nscanned; }

        virtual CoveredIndexMatcher* matcher() const { return _matcher.get(); }

        virtual void setMatcher( shared_ptr<CoveredIndexMatcher> matcher ) { _matcher = matcher; }

        virtual ~IntervalBtreeCursor();

    private:
        IntervalBtreeCursor( NamespaceDetails* namespaceDetails,
                             const IndexDetails& indexDetails,
                             const BSONObj& lowerBound,
                             bool lowerBoundInclusive,
                             const BSONObj& upperBound,
                             bool upperBoundInclusive );

        // For handling bucket deletion.
        static unordered_set<IntervalBtreeCursor*> _activeCursors;
        static SimpleMutex _activeCursorsMutex;

        void init();

        /**
         * @return a location in the btree, determined by the parameters specified.
         * @param key - The key to search for.
         * @param afterKey - If true, return the first btree key greater than the supplied 'key'.
         *     If false, return the first key equal to the supplied 'key'.
         */
        BtreeKeyLocation locateKey( const BSONObj& key, bool afterKey );

        /** Find the iteration end location and set _end to it. */
        void relocateEnd();

        const NamespaceDetails& _namespaceDetails;
        const int32_t _indexNo;
        const IndexDetails& _indexDetails;
        const Ordering _ordering;
        const BSONObj _lowerBound;
        const bool _lowerBoundInclusive;
        const BSONObj _upperBound;
        const bool _upperBoundInclusive;

        BtreeKeyLocation _curr; // Current position in the btree.
        LogicalBtreePosition _currRecoverable; // Helper to track the position of _curr if the
                                               // btree is modified during a mutex yield.
        BtreeKeyLocation _end; // Exclusive end location in the btree.
        int64_t _nscanned;

        shared_ptr<CoveredIndexMatcher> _matcher;
        bool _multikeyFlag;
        unordered_set<DiskLoc,DiskLoc::Hasher> _dups;
    };

} // namespace mongo
