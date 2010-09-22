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
    
    class Chunk;

    /* A ShardKeyPattern is a pattern indicating what data to extract from the object to make the shard key from.
       Analogous to an index key pattern.
    */
    class ShardKeyPattern {
    public:
        ShardKeyPattern( BSONObj p = BSONObj() );
        
        /**
           global min is the lowest possible value for this key
		   e.g. { num : MinKey }
         */
        BSONObj globalMin() const { return gMin; }

        /**
           global max is the highest possible value for this key
         */
        BSONObj globalMax() const { return gMax; }

        bool isGlobalMin( const BSONObj& k ) const{
            return k.woCompare( globalMin() ) == 0;
        }

        bool isGlobalMax( const BSONObj& k ) const{
            return k.woCompare( globalMax() ) == 0;
        }
        
        bool isGlobal( const BSONObj& k ) const{
            return isGlobalMin( k ) || isGlobalMax( k );
        }

        /** compare shard keys from the objects specified
           l < r negative
           l == r 0
           l > r positive
         */
        int compare( const BSONObj& l , const BSONObj& r ) const;
        
        /**
           @return whether or not obj has all fields in this shard key pattern
		   e.g. 
		     ShardKey({num:1}).hasShardKey({ name:"joe", num:3 }) is true
         */
        bool hasShardKey( const BSONObj& obj ) const;
        
        BSONObj key() const { return pattern; }

        string toString() const;

        BSONObj extractKey(const BSONObj& from) const;
        
        bool partOfShardKey(const char* key ) const {
            return pattern.hasField(key);
        }
        bool partOfShardKey(const string& key ) const {
            return pattern.hasField(key.c_str());
        }

        /**
         * @return
         * true if 'this' is a prefix (not necessarily contained) of 'otherPattern'.
         */
        bool isPrefixOf( const BSONObj& otherPattern ) const;
        
    private:
        BSONObj pattern;
        BSONObj gMin;
        BSONObj gMax;

        /* question: better to have patternfields precomputed or not?  depends on if we use copy constructor often. */
        set<string> patternfields;
    };

    inline BSONObj ShardKeyPattern::extractKey(const BSONObj& from) const { 
        BSONObj k = from.extractFields(pattern);
        uassert(13334, "Shard Key must be less than 512 bytes", k.objsize() < 512);
        return k;
    }

} 
