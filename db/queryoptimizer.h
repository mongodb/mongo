/* queryoptimizer.h */

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

#include "cursor.h"
#include "jsobj.h"

namespace mongo {

    class FieldBound {
    public:
        FieldBound( const BSONElement &e = emptyObj.firstElement() );
        FieldBound &operator&=( const FieldBound &other );
        BSONElement lower() const { return lower_; }
        BSONElement upper() const { return upper_; }
        bool equality() const { return lower_.woCompare( upper_, false ) == 0; }
        bool nontrivial() const {
            return
            minKey.firstElement().woCompare( lower_, false ) != 0 ||
            maxKey.firstElement().woCompare( upper_, false ) != 0;
        }
    private:
        BSONObj addObj( const BSONObj &o );
        string simpleRegexEnd( string regex );
        BSONElement lower_;
        BSONElement upper_;
        vector< BSONObj > objData_;
    };
    
    class FieldBoundSet {
    public:
        FieldBoundSet( const BSONObj &query );
        const FieldBound &bound( const char *fieldName ) const {
            map< string, FieldBound >::const_iterator f = bounds_.find( fieldName );
            if ( f == bounds_.end() )
                return trivialBound();
            return f->second;
        }
        int nBounds() const {
            int count = 0;
            for( map< string, FieldBound >::const_iterator i = bounds_.begin(); i != bounds_.end(); ++i )
                ++count;
            return count;
        }
        int nNontrivialBounds() const {
            int count = 0;
            for( map< string, FieldBound >::const_iterator i = bounds_.begin(); i != bounds_.end(); ++i )
                if ( i->second.nontrivial() )
                    ++count;
            return count;
        }
    private:
        static FieldBound *trivialBound_;
        static FieldBound &trivialBound();
        map< string, FieldBound > bounds_;
        BSONObj query_;
    };
    
    class QueryPlan {
    public:
        QueryPlan( const FieldBoundSet &fbs, const BSONObj &order, const BSONObj &idxKey );
        /* If true, no other index can do better. */
        bool optimal() const { return optimal_; }
        /* ScanAndOrder processing will be required if true */
        bool scanAndOrderRequired() const { return scanAndOrderRequired_; }
        /* When true, the index we are using has keys such that it can completely resolve the
         query expression to match by itself without ever checking the main object.
         */
        bool keyMatch() const { return keyMatch_; }
        /* True if keyMatch() is true, and all matches will be equal according to woEqual() */
        bool exactKeyMatch() const { return exactKeyMatch_; }
        int direction() const { return direction_; }
        BSONObj startKey() const { return startKey_; }
        BSONObj endKey() const { return endKey_; }
    private:
        bool optimal_;
        bool scanAndOrderRequired_;
        bool keyMatch_;
        bool exactKeyMatch_;
        int direction_;
        BSONObj startKey_;
        BSONObj endKey_;
    };

    class QueryPlanSet {
    public:
        QueryPlanSet( const char *ns, const BSONObj &query, const BSONObj &order, const BSONElement *hint = 0 );
        int nPlans() const { return plans_.size(); }
    private:
        FieldBoundSet fbs_;
        vector< QueryPlan > plans_;
    };
    
//    class QueryOptimizer {
//    public:
//        static QueryPlan getPlan(
//            const char *ns,
//            BSONObj* query,
//            BSONObj* order = 0,
//            BSONObj* hint = 0);
//    };

} // namespace mongo
