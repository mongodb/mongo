// shardkey.h

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

#include "../client/dbclient.h"

namespace mongo {
    
    class Shard;

    /* A ShardKeyPattern is a pattern indicating what data to extract from the object to make the shard key from.
       Analogous to an index key pattern.
    */
    class ShardKeyPattern {
    public:
        ShardKeyPattern( BSONObj p = BSONObj() ) : pattern( p.getOwned() ) {
            pattern.getFieldNames(patternfields);
        }
        
        /**
           global min is the lowest possible value for this key
		   e.g. { num : MinKey }
         */
        BSONObj globalMin() const;

        /**
           global max is the lowest possible value for this key
         */
        BSONObj globalMax() const;

        bool isGlobalMin( const BSONObj& k ){
            return k.woCompare( globalMin() ) == 0;
        }

        bool isGlobalMax( const BSONObj& k ){
            return k.woCompare( globalMax() ) == 0;
        }
        
        bool isGlobal( const BSONObj& k ){
            return isGlobalMin( k ) || isGlobalMax( k );
        }


        /**
           @return the key central between min and max
           note: min and max could cross type boundaries
         */
        BSONObj middle( const BSONObj & min , const BSONObj & max );

        /** compare shard keys from the objects specified
           l < r negative
           l == r 0
           l > r positive
         */
        int compare( const BSONObj& l , const BSONObj& r );
        
        /**
           @return whether or not obj has all fields in this shard key pattern
		   e.g. 
		     ShardKey({num:1}).hasShardKey({ name:"joe", num:3 }) is true
         */
        bool hasShardKey( const BSONObj& obj );
        
        /**
           returns a query that filters results only for the range desired, i.e. returns 
             { $gte : keyval(min), $lt : keyval(max) }
        */
        void getFilter( BSONObjBuilder& b , const BSONObj& min, const BSONObj& max );
        
        /** @return true if shard s is relevant for query q.

            Example:
              q:     { x : 3 }
              *this: { x : 1 }
              s:     x:2..x:7
               -> true
         */
        bool relevantForQuery( const BSONObj& q , Shard * s );
        
        /**
           Returns if the given sort pattern can be ordered by the shard key pattern.
           Example
            sort:   { ts: -1 }
            *this:  { ts:1 }
              -> -1

              @return
              0 if sort either doesn't have all the fields or has extra fields
              < 0 if sort is descending
              > 1 if sort is ascending
         */
        int canOrder( const BSONObj& sort );

        BSONObj key() { return pattern; }

        string toString() const;

        ShardKeyPattern(const ShardKeyPattern& p) { 
            pattern = p.pattern;
            patternfields = p.patternfields;
        }

        BSONObj extractKey(const BSONObj& from) const;

    private:
        /* question: better to have patternfields precomputed or not?  depends on if we use copy contructor often. */
        BSONObj pattern;
        set<string> patternfields;
        bool relevant(const BSONObj& query, BSONObj& L, BSONObj& R);
    };

    inline BSONObj ShardKeyPattern::extractKey(const BSONObj& from) const { 
        return from.extractFields(pattern);
    }

} 
