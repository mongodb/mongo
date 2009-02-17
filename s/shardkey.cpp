// shardkey.cpp

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

#include "stdafx.h"
#include "shard.h"
#include "../db/jsobj.h"

/**
   TODO: this only works with numbers right now
         this is very temporary, need to make work with anything
*/

namespace mongo {
    
    ShardKey::ShardKey( BSONObj fieldsAndOrder ) : _fieldsAndOrder( fieldsAndOrder ){
        if ( _fieldsAndOrder.nFields() > 0 ){
            _init();
        }
        else {
            _fieldName = 0;
        }
    }

    void ShardKey::init( BSONObj fieldsAndOrder ){
        _fieldsAndOrder = fieldsAndOrder;
        _init();
    }

    void ShardKey::_init(){
        _fieldName = _fieldsAndOrder.firstElement().fieldName();
        uassert( "shard key only supports 1 field right now" , 1 == _fieldsAndOrder.nFields() );
        uassert( "shard key has to be a number right now" , _fieldsAndOrder.firstElement().isNumber() );
    }

    
    void ShardKey::globalMin( BSONObjBuilder& b ){
        uassert( "not valid yet" , _fieldName );
        b << _fieldName << (int)(-0xfffffff);
    }
    
    void ShardKey::globalMax( BSONObjBuilder& b ){
        uassert( "not valid yet" , _fieldName );
        b << _fieldName << (int)(0xfffffff);
    }

    int ShardKey::compare( BSONObj& lObject , BSONObj& rObject ){
        uassert( "not valid yet" , _fieldName );

        BSONElement lElement = lObject[ _fieldsAndOrder.firstElement().fieldName() ];
        uassert( "left key doesn't have the shard key" , ! lElement.eoo() );
        uassert( "left key isn't number" , lElement.isNumber() );

        BSONElement rElement = rObject[ _fieldsAndOrder.firstElement().fieldName() ];
        uassert( "right key doesn't have the shard key" , ! rElement.eoo() );
        uassert( "right key isn't number" , rElement.isNumber() );

        if ( lElement.number() < rElement.number() )
            return -1;

        if ( lElement.number() > rElement.number() )
            return 1;
        
        return 0;
    }
    
    void shardKeyTest(){
        ShardKey k( BSON( "key" << 1 ) );
        
        BSONObj min = k.globalMin();
        BSONObj max = k.globalMax();

        log(3) << "globalMin: " << min << endl;
        log(3) << "globalMax: " << max << endl;

        assert( k.compare( min , max ) < 0 );
        assert( k.compare( max , min ) > 0 );
        assert( k.compare( min , min ) == 0 );
        
    }

    
} // namespace mongo
