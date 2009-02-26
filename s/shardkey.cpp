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
#include "../util/unittest.h"

/**
   TODO: this only works with numbers right now
         this is very temporary, need to make work with anything
*/

namespace mongo {


    
    ShardKeyPattern::ShardKeyPattern( BSONObj fieldsAndOrder ) : _fieldsAndOrder( fieldsAndOrder.getOwned() ){
        if ( _fieldsAndOrder.nFields() > 0 ){
            _init();
        }
        else {
            _fieldName = "";
        }
    }

    void ShardKeyPattern::_init(){
        _fieldName = _fieldsAndOrder.firstElement().fieldName();
        uassert( "shard key only supports 1 field right now" , 1 == _fieldsAndOrder.nFields() );
        uassert( "shard key has to be a number right now" , _fieldsAndOrder.firstElement().isNumber() );
    }

    
    void ShardKeyPattern::globalMin( BSONObjBuilder& b ){
        uassert( "not valid yet" , _fieldName.size() );
        b << _fieldName << (int)(-0xfffffff);
    }
    
    void ShardKeyPattern::globalMax( BSONObjBuilder& b ){
        uassert( "not valid yet" , _fieldName.size() );
        b << _fieldName << (int)(0xfffffff);
    }

    int ShardKeyPattern::compare( const BSONObj& lObject , const BSONObj& rObject ) const {
        uassert( "not valid yet" , _fieldName.size() );
        
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
    
    void ShardKeyPattern::middle( BSONObjBuilder & b , BSONObj & lObject , BSONObj & rObject ){
        BSONElement lElement = lObject[ _fieldsAndOrder.firstElement().fieldName() ];
        uassert( "left key doesn't have the shard key" , ! lElement.eoo() );
        uassert( "left key isn't number" , lElement.isNumber() );

        BSONElement rElement = rObject[ _fieldsAndOrder.firstElement().fieldName() ];
        uassert( "right key doesn't have the shard key" , ! rElement.eoo() );
        uassert( "right key isn't number" , rElement.isNumber() );        

        b.append( _fieldName.c_str() , ( lElement.number() + rElement.number() ) / 2 );
    }

    bool ShardKeyPattern::hasShardKey( const BSONObj& obj ){
        return ! obj[_fieldName.c_str()].eoo();
    }

    bool ShardKeyPattern::relevantForQuery( const BSONObj& query , Shard * shard ){
        if ( ! hasShardKey( query ) ){
            // if the shard key isn't in the query, then we have to go everywhere
            // therefore this shard is relevant
            return true;
        }

        // the rest of this is crap
        
        BSONElement e = query[_fieldName.c_str()];
        if ( e.isNumber() ){ // TODO: this shoudl really be if its an actual value, and not a range
            return shard->contains( query );
        }

        return true;
    }

    void ShardKeyPattern::getFilter( BSONObjBuilder& b , const BSONObj& min, const BSONObj& max ){
        BSONObjBuilder temp;
        temp.append( "$gte" , min[_fieldName.c_str()].number() );
        temp.append( "$lt" , max[_fieldName.c_str()].number() );
        b.append( _fieldName.c_str() , temp.obj() );
    }    

    int ShardKeyPattern::canOrder( const BSONObj& sort ){
        if ( sort.nFields() != _fieldsAndOrder.nFields() )
            return 0;

        if ( ! sort.hasField( _fieldName.c_str() ) )
            return 0;

        if ( sort[_fieldName.c_str()].number() <= 0 )
            return -1;
        return 1;
    }

    string ShardKeyPattern::toString() const {
        return _fieldsAndOrder.toString();
    }

    class ShardKeyUnitTest : public UnitTest {
    public:
        void run(){
            ShardKeyPattern k( BSON( "key" << 1 ) );
            
            BSONObj min = k.globalMin();
            BSONObj max = k.globalMax();
            
            BSONObj k1 = BSON( "key" << 5 );

            assert( k.compare( min , max ) < 0 );
            assert( k.compare( min , k1 ) < 0 );
            assert( k.compare( max , min ) > 0 );
            assert( k.compare( min , min ) == 0 );
            
            assert( k.hasShardKey( k1 ) );
            assert( ! k.hasShardKey( BSON( "key2" << 1 ) ) );

            BSONObj a = k1;
            BSONObj b = BSON( "key" << 999 );

            assert( k.compare(a,b) < 0 );

            assert( k.compare(a,k.middle(a,a)) == 0 );
            assert( k.compare(a,k.middle(a,b)) <= 0 );
            assert( k.compare(k.middle(a,b),b) <= 0 );

            assert( k.canOrder( fromjson("{key:1}") ) == 1 );
            assert( k.canOrder( fromjson("{zz:1}") ) == 0 );
            assert( k.canOrder( fromjson("{key:-1}") ) == -1 );

        }
    } shardKeyTest;
    
} // namespace mongo
