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
       NOTE: the implementation for this is tempoary.
             it only currently works for a single numeric field
     */
    class ShardKey {
    public:
        ShardKey( BSONObj fieldsAndOrder = emptyObj );
        void init( BSONObj fieldsAndOrder );
        virtual ~ShardKey() {}
        
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

        /**
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
           returns a filter relevant that returns results only for that range
        */
        void getFilter( BSONObjBuilder& b , const BSONObj& min, const BSONObj& max );
        
        /**
           @return whether or not shard should be looked at for query
         */
        bool relevantForQuery( const BSONObj& query , Shard * shard );

        BSONObj& key(){
            return _fieldsAndOrder;
        }

        virtual string toString() const;

    private:
        void _init();
        BSONObj _fieldsAndOrder;
        string _fieldName;
    };
} 
