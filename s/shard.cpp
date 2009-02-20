// shard.cpp

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
#include "config.h"
#include "../util/unittest.h"

namespace mongo {

    // -------  Shard --------
    
    Shard::Shard( ShardInfo * info , BSONObj data ) : _info( info ) , _data( data ){
        _min = _data.getObjectField( "min" );
        _max = _data.getObjectField( "max" );
    }

    string ShardInfo::modelServer() {
        // TODO: this could move around?
        return configServer.modelServer();
    }

    bool Shard::contains( const BSONObj& obj ){
        return
            _info->getShardKey().compare( getMin() , obj ) <= 0 &&
            _info->getShardKey().compare( obj , getMax() ) < 0;
    }

    void Shard::split(){
        split( _info->getShardKey().middle( getMin() , getMax() ) );
    }
        
    void Shard::split( const BSONObj& m ){

        {
            BSONObjBuilder l;
            l.append( "min" , _min );
            l.append( "max" , m );
            l.append( "server" , getServer() );
            _info->_shards.push_back( new Shard( _info , l.obj() ) );
        }

        {
            BSONObjBuilder r;
            r.append( "min" , m );
            r.append( "max" , _max );
            r.append( "server" , getServer() );
            _info->_shards.push_back( new Shard( _info , r.obj() ) );
        }

        for ( vector<Shard*>::iterator i=_info->_shards.begin(); i != _info->_shards.end(); i++ ){
            Shard * s = *i;
            if ( s == this ){
                _info->_shards.erase( i );
                delete( s );
                break;
            }
        }

    }

    bool Shard::operator==( const Shard& s ){
        return 
            _info->getShardKey().compare( _min , s._min ) == 0 &&
            _info->getShardKey().compare( _max , s._max ) == 0
            ;
    }

    
    string Shard::toString() const {
        return _data.toString();
    }

    // -------  ShardInfo --------

    ShardInfo::ShardInfo( DBConfig * config ) : _config( config ){
    }

    ShardInfo::~ShardInfo(){
        for ( vector<Shard*>::iterator i=_shards.begin(); i != _shards.end(); i++ ){
            delete( *i );
        }
        _shards.clear();
    }

    bool ShardInfo::hasShardKey( const BSONObj& obj ){
        return _key.hasShardKey( obj );
    }

    Shard& ShardInfo::findShard( const BSONObj & obj ){

        for ( vector<Shard*>::iterator i=_shards.begin(); i != _shards.end(); i++ ){
            Shard * s = *i;
            if ( s->contains( obj ) )
                return *s;
        }
        throw UserException( "couldn't find a shard which should be impossible" );
    }
    
    void ShardInfo::serialize(BSONObjBuilder& to){
        to.append( "ns", _ns );
        to.append( "key" , _key.key() );
        
        BSONObjBuilder shards;
        int num=0;
        for ( vector<Shard*>::iterator i=_shards.begin(); i != _shards.end(); i++  ){
            string s = shards.numStr( num++ );
            shards.append( s.c_str() , (*i)->_data );
        }
        to.appendArray( "shards" , shards.obj() );
    }

    void ShardInfo::unserialize(BSONObj& from) {
        _ns = from.getStringField("ns");
        uassert("bad config.shards.name", !_ns.empty());
        
        _key.init( from.getObjectField( "key" ) );

        assert( _shards.size() == 0 );

        BSONObj shards = from.getObjectField( "shards" );
        if ( shards.isEmpty() ){
            BSONObjBuilder all;

            all.append( "min" , _key.globalMin() );
            all.append( "max" , _key.globalMax() );
            all.append( "server" , _config ? _config->getPrimary() : "noserver" );
            
            _shards.push_back( new Shard( this , all.obj() ) );
        }
        else {
            int num=0;
            while ( true ){
                string s = BSONObjBuilder::numStr( num++ );            
                BSONObj next = shards.getObjectField( s.c_str() );
                if ( next.isEmpty() )
                    break;
                _shards.push_back( new Shard( this , next ) );
            }
        }
    }

    bool ShardInfo::loadByName( const string& ns ){
        BSONObjBuilder b;
        b.append("ns", ns);
        BSONObj q = b.done();
        return load(q);
    }

    string ShardInfo::toString() const {
        stringstream ss;
        ss << "ShardInfo: " << _ns << " key:" << _key.toString() << "\n";
        for ( vector<Shard*>::const_iterator i=_shards.begin(); i!=_shards.end(); i++ ){
            const Shard* s = *i;
            ss << "\t" << s->toString() << "\n";
        }
        return ss.str();
    }

    
    class ShardObjUnitTest : public UnitTest {
    public:
        void run(){
            string ns = "alleyinsider.blog.posts";
            BSONObj o = BSON( "ns" << ns << "key" << BSON( "num" << 1 ) );
            
            ShardInfo si(0);
            si.unserialize( o );
            assert( si.getns() == ns );
            
            BSONObjBuilder b;
            si.serialize( b );
            BSONObj a = b.obj();
            assert( ns == a.getStringField( "ns" ) );
            assert( 1 == a.getObjectField( "key" )["num"].number() );
            
            log(2) << a << endl;
            
            {
                ShardInfo si2(0);
                si2.unserialize( a );
                BSONObjBuilder b2;
                si2.serialize( b2 );
                assert( b2.obj().jsonString() == a.jsonString() );
            }
            
            {
                BSONObj num = BSON( "num" << 5 );
                si.findShard( num );
                
                assert( si.findShard( BSON( "num" << -1 ) ) == 
                        si.findShard( BSON( "num" << 1 ) ) );
                
                log(2) << "before split: " << si << endl;
                si.findShard( num ).split();
                log(2) << "after split: " << si << endl;
                
                assert( si.findShard( BSON( "num" << -1 ) ) != 
                        si.findShard( BSON( "num" << 1 ) ) );
                
                string s1 = si.toString();
                BSONObjBuilder b2;
                si.serialize( b2 );
                ShardInfo s3(0);
                BSONObj temp = b2.obj();
                s3.unserialize( temp );
                assert( s1 == s3.toString() );
                
            }

            log(1) << "shardObjTest passed" << endl;
        }
    } shardObjTest;


} // namespace mongo
