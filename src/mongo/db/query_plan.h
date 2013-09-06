/**
 *    Copyright (C) 2008 10gen Inc.
 *
 *   This program is free software: you can redistribute it and/or  modify
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
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher.h"
#include "mongo/db/projection.h"
#include "mongo/db/querypattern.h"

namespace mongo {

    class Cursor;
    class FieldRangeSet;
    class FieldRangeSetPair;
    class IndexDetails;
    class NamespaceDetails;
    class ParsedQuery;
    struct QueryPlanSummary;

    /**
     * A plan for executing a query using the given index spec and FieldRangeSet.  An object of this
     * class may only be used by one thread at a time.
     */
    class QueryPlan : boost::noncopyable {
    public:

        /**
         * @param originalFrsp - original constraints for this query clause.  If null, frsp will be
         * used instead.
         */
        static QueryPlan* make( NamespaceDetails* d,
                                int idxNo, // -1 = no index
                                const FieldRangeSetPair& frsp,
                                const FieldRangeSetPair* originalFrsp,
                                const BSONObj& originalQuery,
                                const BSONObj& order,
                                const shared_ptr<const ParsedQuery>& parsedQuery =
                                        shared_ptr<const ParsedQuery>(),
                                const BSONObj& startKey = BSONObj(),
                                const BSONObj& endKey = BSONObj(),
                                const std::string& special = "" );

        /** Categorical classification of a QueryPlan's utility. */
        enum Utility {
            Impossible, // Cannot produce any matches, so the query must have an empty result set.
                        // No other plans need to be considered.
            Optimal,    // Should run as the only candidate plan in the absence of an Impossible
                        // plan.
            Helpful,    // Should be considered.
            Unhelpful,  // Should not be considered.
            Disallowed  // Must not be considered unless explicitly hinted.  May produce a
                        // semantically incorrect result set.
        };

        Utility utility() const { return _utility; }
        
        /** @return true if ScanAndOrder processing will be required for result set. */
        bool scanAndOrderRequired() const { return _scanAndOrderRequired; }

        /**
         * @return false if document matching can be determined entirely using index keys and the
         * FieldRangeSetPair generated for the query, without using a Matcher.  This function may
         * return false positives but not false negatives.  For example, if the field range set's
         * mustBeExactMatchRepresentation() returns a false negative, this function will return a
         * false positive.
         */
        bool mayBeMatcherNecessary() const { return _matcherNecessary; }

        /** @return true if this QueryPlan would perform an unindexed scan. */
        bool willScanTable() const { return _idxNo < 0 && ( _utility != Impossible ); }

        /**
         * @return 'special' attribute of the plan, which was either set explicitly or generated
         * from the index.
         */
        const string& special() const { return _special; }
                
        /** @return a new cursor based on this QueryPlan's index and FieldRangeSet. */
        shared_ptr<Cursor> newCursor( const DiskLoc& startLoc = DiskLoc(),
                                      bool requestIntervalCursor = false ) const;

        /** @return a new reverse cursor if this is an unindexed plan. */
        shared_ptr<Cursor> newReverseCursor() const;

        /** Register this plan as a winner for its QueryPattern, with specified 'nscanned'. */
        void registerSelf( long long nScanned, CandidatePlanCharacter candidatePlans ) const;

        int direction() const { return _direction; }

        BSONObj indexKey() const;

        bool indexed() const { return _index != 0; }

        const IndexDetails* index() const { return _index; }

        int idxNo() const { return _idxNo; }

        const char* ns() const;

        NamespaceDetails* nsd() const { return _d; }

        BSONObj originalQuery() const { return _originalQuery; }

        shared_ptr<FieldRangeVector> originalFrv() const { return _originalFrv; }

        const FieldRangeSet& multikeyFrs() const { return _frsMulti; }
        
        shared_ptr<Projection::KeyOnly> keyFieldsOnly() const { return _keyFieldsOnly; }

        const ParsedQuery* parsedQuery() const { return _parsedQuery.get(); }

        /** @return a shared, lazily initialized matcher for the query plan. */
        shared_ptr<CoveredIndexMatcher> matcher() const;
        
        QueryPlanSummary summary() const;

        // The following member functions are for testing, or public for testing.
        
        shared_ptr<FieldRangeVector> frv() const { return _frv; }
        bool isMultiKey() const;
        string toString() const;
        bool queryBoundsExactOrderSuffix() const;

    private:
        
        QueryPlan( NamespaceDetails* d,
                   int idxNo,
                   const FieldRangeSetPair& frsp,
                   const BSONObj& originalQuery,
                   const BSONObj& order,
                   const shared_ptr<const ParsedQuery>& parsedQuery,
                   const std::string& special );
        void init( const FieldRangeSetPair* originalFrsp,
                   const BSONObj& startKey,
                   const BSONObj& endKey );

        void checkTableScanAllowed() const;

        int independentRangesSingleIntervalLimit() const;

        /** @return true when the plan's query may contains an $exists:false predicate. */
        bool hasPossibleExistsFalsePredicate() const;

        NamespaceDetails* _d;
        int _idxNo;
        const FieldRangeSet& _frs;
        const FieldRangeSet& _frsMulti;
        const BSONObj _originalQuery;
        const BSONObj _order;
        shared_ptr<const ParsedQuery> _parsedQuery;
        const IndexDetails* _index;
        bool _scanAndOrderRequired;
        bool _matcherNecessary;
        int _direction;
        shared_ptr<FieldRangeVector> _frv;
        shared_ptr<FieldRangeVector> _originalFrv;
        BSONObj _startKey;
        BSONObj _endKey;
        bool _endKeyInclusive;
        Utility _utility;
        string _special;
        bool _startOrEndSpec;
        shared_ptr<Projection::KeyOnly> _keyFieldsOnly;
        mutable shared_ptr<CoveredIndexMatcher> _matcher; // Lazy initialization.
        auto_ptr<IndexDescriptor> _descriptor;
        string _specialIndexName;
    };

    std::ostream &operator<< ( std::ostream& out, const QueryPlan::Utility& utility );

} // namespace mongo
