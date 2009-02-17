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

        void globalMin( BSONObjBuilder & b );
        BSONObj globalMin(){ BSONObjBuilder b; globalMin( b ); return b.obj(); }
        
        void globalMax( BSONObjBuilder & b );
        BSONObj globalMax(){ BSONObjBuilder b; globalMax( b ); return b.obj(); }
        
        void split( BSONObjBuilder & b , BSONObj & min , BSONObj & max );
        BSONObj split( BSONObj & min , BSONObj & max ){ BSONObjBuilder b; split( b , min , max ); return b.obj(); }

        int compare( BSONObj& l , BSONObj& r );
        
        BSONObj& key(){
            return _fieldsAndOrder;
        }

    private:
        void _init();
        BSONObj _fieldsAndOrder;
        const char * _fieldName;
    };

    void shardKeyTest();
} 
