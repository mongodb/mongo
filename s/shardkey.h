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
    
    class ShardKey {
    public:
        ShardKey( BSONObj fieldsAndOrder = emptyObj );
        void init( BSONObj fieldsAndOrder );
        virtual ~ShardKey() {}

        void globalMin( BSONObjBuilder & b );
        BSONObj globalMin(){ BSONObjBuilder b; globalMin( b ); return b.obj(); }
        
        void globalMax( BSONObjBuilder & b );
        BSONObj globalMax(){ BSONObjBuilder b; globalMax( b ); return b.obj(); }
        
        void middle( BSONObjBuilder & b , BSONObj & min , BSONObj & max );
        BSONObj middle( BSONObj & min , BSONObj & max ){ BSONObjBuilder b; middle( b , min , max ); return b.obj(); }

        int compare( const BSONObj& l , const BSONObj& r ) const;
        
        BSONObj& key(){
            return _fieldsAndOrder;
        }

        virtual string toString() const;

    private:
        void _init();
        BSONObj _fieldsAndOrder;
        const char * _fieldName;
    };
} 
