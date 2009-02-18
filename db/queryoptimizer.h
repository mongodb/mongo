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
        FieldBound( BSONElement e = emptyObj.firstElement() );
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
        BSONObj addObj( BSONObj o );
        string simpleRegexEnd( string regex );
        BSONElement lower_;
        BSONElement upper_;
        vector< BSONObj > objData_;
    };
    
    class FieldBoundSet {
    public:
        FieldBoundSet( BSONObj query );
        const FieldBound &bound( const char *fieldName ) const {
            map< string, FieldBound >::const_iterator f = bounds_.find( fieldName );
            if ( f == bounds_.end() )
                return trivialBound();
            return f->second;
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
        QueryPlan( const FieldBoundSet &fbs, BSONObj order, BSONObj idxKey );
        bool optimal() const { return optimal_; }
        /* ScanAndOrder processing will be required if true */
        bool scanAndOrderRequired() const { return scanAndOrderRequired_; }
        /* When true, the index we are using has keys such that it can completely resolve the
         query expression to match by itself without ever checking the main object.
         */
        bool keyMatch() const { return keyMatch_; }
        bool exactKeyMatch() const { return exactKeyMatch_; }
    private:
        bool optimal_;
        bool scanAndOrderRequired_;
        bool keyMatch_;
        bool exactKeyMatch_;
    };

//    /* We put these objects inside the Database objects: that way later if we want to do
//       stats, it's in the right place.
//    */
//    class QueryOptimizer {
//    public:
//        static QueryPlan getPlan(
//            const char *ns,
//            BSONObj* query,
//            BSONObj* order = 0,
//            BSONObj* hint = 0);
//    };

} // namespace mongo
