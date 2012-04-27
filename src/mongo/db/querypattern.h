// @file querypattern.h - Query pattern matching for selecting similar plans given similar queries.

/*    Copyright 2011 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include "jsobj.h"

namespace mongo {

    class FieldRangeSet;
    
    /**
     * Implements query pattern matching, used to determine if a query is
     * similar to an earlier query and should use the same plan.
     *
     * Two queries will generate the same QueryPattern, and therefore match each
     * other, if their fields have the same Types and they have the same sort
     * spec.
     */
    class QueryPattern {
    public:
        QueryPattern( const FieldRangeSet &frs, const BSONObj &sort );
        enum Type {
            Empty,
            Equality,
            LowerBound,
            UpperBound,
            UpperAndLowerBound,
            ConstraintPresent
        };
        bool operator<( const QueryPattern &other ) const;
        /** for testing only */
        bool operator==( const QueryPattern &other ) const;
        /** for testing only */
        bool operator!=( const QueryPattern &other ) const;
        /** for development / debugging */
        string toString() const;
    private:
        void setSort( const BSONObj sort );
        static BSONObj normalizeSort( const BSONObj &spec );
        map<string,Type> _fieldTypes;
        BSONObj _sort;
    };

    /** Summarizes the candidate plans that may run for a query. */
    class CandidatePlanCharacter {
    public:
        CandidatePlanCharacter( bool mayRunInOrderPlan, bool mayRunOutOfOrderPlan ) :
        _mayRunInOrderPlan( mayRunInOrderPlan ),
        _mayRunOutOfOrderPlan( mayRunOutOfOrderPlan ) {
        }
        CandidatePlanCharacter() :
        _mayRunInOrderPlan(),
        _mayRunOutOfOrderPlan() {
        }
        bool mayRunInOrderPlan() const { return _mayRunInOrderPlan; }
        bool mayRunOutOfOrderPlan() const { return _mayRunOutOfOrderPlan; }
        bool valid() const { return mayRunInOrderPlan() || mayRunOutOfOrderPlan(); }
        bool hybridPlanSet() const { return mayRunInOrderPlan() && mayRunOutOfOrderPlan(); }
    private:
        bool _mayRunInOrderPlan;
        bool _mayRunOutOfOrderPlan;
    };

    /** Information about a query plan that ran successfully for a QueryPattern. */
    class CachedQueryPlan {
    public:
        CachedQueryPlan() :
        _nScanned() {
        }
        CachedQueryPlan( const BSONObj &indexKey, long long nScanned,
                        CandidatePlanCharacter planCharacter );
        BSONObj indexKey() const { return _indexKey; }
        long long nScanned() const { return _nScanned; }
        CandidatePlanCharacter planCharacter() const { return _planCharacter; }
    private:
        BSONObj _indexKey;
        long long _nScanned;
        CandidatePlanCharacter _planCharacter;
    };

    inline bool QueryPattern::operator<( const QueryPattern &other ) const {
        map<string,Type>::const_iterator i = _fieldTypes.begin();
        map<string,Type>::const_iterator j = other._fieldTypes.begin();
        while( i != _fieldTypes.end() ) {
            if ( j == other._fieldTypes.end() )
                return false;
            if ( i->first < j->first )
                return true;
            else if ( i->first > j->first )
                return false;
            if ( i->second < j->second )
                return true;
            else if ( i->second > j->second )
                return false;
            ++i;
            ++j;
        }
        if ( j != other._fieldTypes.end() )
            return true;
        return _sort.woCompare( other._sort ) < 0;
    }
        
} // namespace mongo
