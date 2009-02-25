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

    /**
       NOTE: the implementation for this is temporary.
             it only currently works for a single numeric field
     */
    /* A ShardKey is a pattern indicating what data to extract from the object to make the shard key from. 
       */
    class ShardKey {
    public:
        ShardKey( BSONObj fieldsAndOrder = emptyObj );
        void init( BSONObj fieldsAndOrder );
        
        /**
           global min is the lowest possible value for this key
         */
        void globalMin( BSONObjBuilder & b );
        BSONObj globalMin(){ BSONObjBuilder b; globalMin( b ); return b.obj(); }

        /**
           global max is the lowest possible value for this key
         */
        void globalMax( BSONObjBuilder & b );
        BSONObj globalMax(){ BSONObjBuilder b; globalMax( b ); return b.obj(); }
        
        /**
           return the key inbetween min and max
           note: min and max could cross type boundaries
         */
        void middle( BSONObjBuilder & b , BSONObj & min , BSONObj & max );
        BSONObj middle( BSONObj & min , BSONObj & max ){ BSONObjBuilder b; middle( b , min , max ); return b.obj(); }

        /** compare shard keys from the objects specified
           l < r negative
           l == r 0
           l > r positive
         */
        int compare( const BSONObj& l , const BSONObj& r ) const;
        
        /**
         * @return whether or not obj has all fields in this shard key
         */
        bool hasShardKey( const BSONObj& obj );
        
        /**
           returns a query that filters results only for the range desired, i.e. returns 
             { $gte : keyval(min), $lt : keyval(max) }
        */
        void getFilter( BSONObjBuilder& b , const BSONObj& min, const BSONObj& max );
        
        /**
???
           @return whether or not shard should be looked at for query
         */
        bool relevantForQuery( const BSONObj& query , Shard * shard );
        
        //int ___numFields() const{ return _fieldsAndOrder.nFields(); }

        /**
???
           0 if sort either doesn't have all the fields or has extra fields
           < 0 if sort is descending
           > 1 if sort is ascending
         */
        int isMatchAndOrder( const BSONObj& sort );

        BSONObj& key(){
            return _fieldsAndOrder;
        }

        string toString() const;

    private:
        void _init();
        BSONObj _fieldsAndOrder;
        string _fieldName;
    };
} 
