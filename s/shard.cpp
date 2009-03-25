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
#include "../client/connpool.h"

namespace mongo {

    // -------  Shard --------
    
    Shard::Shard( ShardManager * manager ) : _manager( manager ){
        _modified = false;
        _lastmod = 0;
    }

    void Shard::setServer( string s ){
        _server = s;
        _markModified();
    }
    
    bool Shard::contains( const BSONObj& obj ){
        return
            _manager->getShardKey().compare( getMin() , obj ) <= 0 &&
            _manager->getShardKey().compare( obj , getMax() ) < 0;
    }

    Shard * Shard::split(){
        return split( _manager->getShardKey().middle( getMin() , getMax() ) );
    }
    
    Shard * Shard::split( const BSONObj& m ){
        uassert( "can't split as shard that doesn't have a manager" , _manager );
        
        log(1) << " before split on: "  << m << "\n"
               << "\t self  : " << toString() << endl;

        Shard * s = new Shard( _manager );
        s->_ns = _ns;
        s->_server = _server;
        s->_min = m.getOwned();
        s->_max = _max;
        
        s->_markModified();
        _markModified();
        
        _manager->_shards.push_back( s );
        
        _max = m.getOwned(); 
        
        log(1) << " after split:\n" 
               << "\t left : " << toString() << "\n" 
               << "\t right: "<< s->toString() << endl;

        return s;
    }
    
    bool Shard::operator==( const Shard& s ){
        return 
            _manager->getShardKey().compare( _min , s._min ) == 0 &&
            _manager->getShardKey().compare( _max , s._max ) == 0
            ;
    }

    void Shard::getFilter( BSONObjBuilder& b ){
        _manager->_key.getFilter( b , _min , _max );
    }
    
    void Shard::serialize(BSONObjBuilder& to){
        if ( _lastmod )
            to.appendDate( "lastmod" , _lastmod );
        else 
            to.appendTimestamp( "lastmod" );

        to << "ns" << _ns;
        to << "min" << _min;
        to << "max" << _max;
        to << "server" << _server;
    }
    
    void Shard::unserialize(const BSONObj& from){
        _ns = from.getStringField( "ns" );
        _min = from.getObjectField( "min" ).getOwned();
        _max = from.getObjectField( "max" ).getOwned();
        _server = from.getStringField( "server" );
        _lastmod = from.hasField( "lastmod" ) ? from["lastmod"].date() : 0;
        
        uassert( "Shard needs a ns" , ! _ns.empty() );
        uassert( "Shard needs a server" , ! _ns.empty() );

        uassert( "Shard needs a min" , ! _min.isEmpty() );
        uassert( "Shard needs a max" , ! _max.isEmpty() );
    }

    string Shard::modelServer() {
        // TODO: this could move around?
        return configServer.modelServer();
    }
    
    void Shard::_markModified(){
        _modified = true;

        unsigned long long t = time(0);
        t *= 1000;
        _lastmod = 0;
    }

    string Shard::toString() const {
        stringstream ss;
        ss << "shard  ns:" << _ns << " server: " << _server << " min: " << _min << " max: " << _max;
        return ss.str();
    }
    
    // -------  ShardManager --------

    ShardManager::ShardManager( DBConfig * config , string ns , ShardKeyPattern pattern ) : _config( config ) , _ns( ns ) , _key( pattern ){
        Shard temp(0);
        
        ScopedDbConnection conn( temp.modelServer() );
        auto_ptr<DBClientCursor> cursor = conn->query( temp.getNS() , BSON( "ns" <<  ns ) );
        while ( cursor->more() ){
            Shard * s = new Shard( this );
            BSONObj d = cursor->next();
            s->unserialize( d );
            _shards.push_back( s );
        }
        conn.done();
        
        if ( _shards.size() == 0 ){
            Shard * s = new Shard( this );
            s->_ns = ns;
            s->_min = _key.globalMin();
            s->_max = _key.globalMax();
            s->_server = config->getPrimary();
            s->_markModified();

            _shards.push_back( s );

            log() << "no shards for:" << ns << " so creating first: " << s->toString() << endl;
        }
    }
    
    ShardManager::~ShardManager(){
        for ( vector<Shard*>::iterator i=_shards.begin(); i != _shards.end(); i++ ){
            delete( *i );
        }
        _shards.clear();
    }

    bool ShardManager::hasShardKey( const BSONObj& obj ){
        return _key.hasShardKey( obj );
    }

    Shard& ShardManager::findShard( const BSONObj & obj ){
        
        for ( vector<Shard*>::iterator i=_shards.begin(); i != _shards.end(); i++ ){
            Shard * s = *i;
            if ( s->contains( obj ) )
                return *s;
        }
        throw UserException( "couldn't find a shard which should be impossible" );
    }

    int ShardManager::getShardsForQuery( vector<Shard*>& shards , const BSONObj& query ){
        int added = 0;
        
        for ( vector<Shard*>::iterator i=_shards.begin(); i != _shards.end(); i++  ){
            Shard* s = *i;
            if ( _key.relevantForQuery( query , s ) ){
                shards.push_back( s );
                added++;
            }
        }
        return added;
    }
    
    void ShardManager::save(){
        for ( vector<Shard*>::const_iterator i=_shards.begin(); i!=_shards.end(); i++ ){
            Shard* s = *i;
            if ( ! s->_modified )
                continue;
            s->save( true );
        }
    }

    ServerShardVersion ShardManager::getVersion( const string& server ) const{
        // TODO: cache or something?
        
        ServerShardVersion max = 0;
        cout << "getVersion for: " << server << endl;
        for ( vector<Shard*>::const_iterator i=_shards.begin(); i!=_shards.end(); i++ ){
            Shard* s = *i;
            cout << "\t" << s->getServer() << endl;
            if ( s->getServer() != server )
                continue;
            
            if ( s->_lastmod > max )
                max = s->_lastmod;
        }        

        return max;
    }

    string ShardManager::toString() const {
        stringstream ss;
        ss << "ShardManager: " << _ns << " key:" << _key.toString() << "\n";
        for ( vector<Shard*>::const_iterator i=_shards.begin(); i!=_shards.end(); i++ ){
            const Shard* s = *i;
            ss << "\t" << s->toString() << "\n";
        }
        return ss.str();
    }
    
    
    class ShardObjUnitTest : public UnitTest {
    public:
        void runShard(){

        }
        
        void run(){
            runShard();
            log(1) << "shardObjTest passed" << endl;
        }
    } shardObjTest;


} // namespace mongo
