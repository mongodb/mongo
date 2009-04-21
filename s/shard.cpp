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
#include "strategy.h"

namespace mongo {

    // -------  Shard --------

    long Shard::MaxShardSize = 1024 * 1204 * 50;
    
    Shard::Shard( ShardManager * manager ) : _manager( manager ){
        _modified = false;
        _lastmod = 0;
        _dataWritten = 0;
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

    BSONObj Shard::pickSplitPoint(){
        int sort = 0;
        
        if ( _manager->getShardKey().globalMin().woCompare( getMin() ) == 0 ){
            sort = 1;
        }
        else if ( _manager->getShardKey().globalMax().woCompare( getMax() ) == 0 ){
            sort = -1;
        }
        
        if ( sort ){
            ScopedDbConnection conn( getServer() );
            Query q;
            if ( sort == 1 )
                q.sort( _manager->getShardKey().key() );
            else {
                BSONObj k = _manager->getShardKey().key();
                BSONObjBuilder r;
                
                BSONObjIterator i(k);
                while( i.more() ) {
                    BSONElement e = i.next();
                    if ( e.eoo() )
                        break;
                    uassert( "can only handle numbers here - which i think is correct" , e.isNumber() );
                    r.append( e.fieldName() , -1 * e.number() );
                }
                
                q.sort( r.obj() );
            }
            BSONObj end = conn->findOne( _ns , q );
            conn.done();
            
            if ( ! end.isEmpty() )
                return _manager->getShardKey().extractKey( end );
        }
        
        ScopedDbConnection conn( getServer() );
        BSONObj result;
        uassert( "medianKey failed!" , conn->runCommand( "admin" , BSON( "medianKey" << _ns
                                                                         << "keyPattern" << _manager->getShardKey().key() 
                                                                         << "min" << getMin() 
                                                                         << "max" << getMax() 
                                                                         ) , result ) );
        conn.done();
        
        return result.getObjectField( "median" ).getOwned();
    }

    Shard * Shard::split(){
        return split( pickSplitPoint() );
    }
    
    Shard * Shard::split( const BSONObj& m ){
        uassert( "can't split as shard that doesn't have a manager" , _manager );
        
        log(1) << " before split on: "  << m << "\n"
               << "\t self  : " << toString() << endl;

        uassert( "locking namespace on server failed" , lockNamespaceOnServer( getServer() , _ns ) );

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
        
        
        _manager->save();
        
        return s;
    }

    bool Shard::moveAndCommit( const string& to , string& errmsg ){
        uassert( "can't move shard to its current location!" , to != getServer() );

        log() << "moving shard ns: " << _ns << " moving shard: " << toString() << " " << _server << " -> " << to << endl;
        
        string from = _server;
        ServerShardVersion oldVersion = _manager->getVersion( from );
        
        BSONObj filter;
        {
            BSONObjBuilder b;
            getFilter( b );
            filter = b.obj();
        }
        
        ScopedDbConnection fromconn( from );

        BSONObj startRes;
        bool worked = fromconn->runCommand( "admin" ,
                                            BSON( "moveshard.start" << _ns << 
                                                  "from" << from <<
                                                  "to" << to <<
                                                  "filter" << filter
                                                  ) ,
                                            startRes
                                            );
        
        if ( ! worked ){
            errmsg = (string)"moveshard.start failed: " + startRes.toString();
            return false;
        }
        
        // update config db
        setServer( to );
        
        // need to increment version # for old server
        Shard * randomShardOnOldServer = _manager->findShardOnServer( from );
        if ( randomShardOnOldServer )
            randomShardOnOldServer->_markModified();
        
        _manager->save();
        
        BSONObj finishRes;
        {

            ServerShardVersion newVersion = _manager->getVersion( from );
            uassert( "version has to be higher" , newVersion > oldVersion );

            BSONObjBuilder b;
            b << "moveshard.finish" << _ns;
            b << "to" << to;
            b.appendTimestamp( "newVersion" , newVersion );
            b.append( startRes["finishToken"] );
        
            worked = fromconn->runCommand( "admin" ,
                                           b.done() , 
                                           finishRes );
        }
        
        if ( ! worked ){
            errmsg = (string)"moveshard.finish failed: " + finishRes.toString();
            return false;
        }
        
        fromconn.done();
        return true;
    }
    
    bool Shard::splitIfShould( long dataWritten ){
        _dataWritten += dataWritten;
        
        if ( _dataWritten < MaxShardSize / 5 )
            return false;

        _dataWritten = 0;
        
        if ( _min.woCompare( _max ) == 0 ){
            log() << "SHARD PROBLEM** shard is too big, but can't split: " << toString() << endl;
            return false;
        }

        long size = getPhysicalSize();
        if ( size < MaxShardSize )
            return false;
        
        log() << "autosplitting " << _ns << " size: " << size << " shard: " << toString() << endl;
        Shard * newShard = split();

        moveIfShould( newShard );
        
        return true;
    }

    bool Shard::moveIfShould( Shard * newShard ){
        Shard * toMove = 0;
       
        if ( newShard->countObjects() <= 1 ){
            toMove = newShard;
        }
        else if ( this->countObjects() <= 1 ){
            toMove = this;
        }
        else {
            log(1) << "don't know how to decide if i should move inner shard" << endl;
        }

        if ( ! toMove )
            return false;
        
        string newLocation = grid.pickServerForNewDB();
        if ( newLocation == getServer() ){
            // if this is the best server, then we shouldn't do anything!
            log(1) << "not moving shard: " << toString() << " b/c would move to same place  " << newLocation << " -> " << getServer() << endl;
            return 0;
        }

        log() << "moving shard (auto): " << toMove->toString() << " to: " << newLocation << " #objcets: " << toMove->countObjects() << endl;

        string errmsg;
        massert( (string)"moveAndCommit failed: " + errmsg , 
                 toMove->moveAndCommit( newLocation , errmsg ) );
        
        return true;
    }

    long Shard::getPhysicalSize(){
        ScopedDbConnection conn( getServer() );
        
        BSONObj result;
        uassert( "datasize failed!" , conn->runCommand( "admin" , BSON( "datasize" << _ns
                                                                        << "keyPattern" << _manager->getShardKey().key() 
                                                                        << "min" << getMin() 
                                                                        << "max" << getMax() 
                                                                        ) , result ) );
        
        conn.done();
        return (long)result["size"].number();
    }

    
    long Shard::countObjects(){
        ScopedDbConnection conn( getServer() );
        
        BSONObj result;
        uassert( "datasize failed!" , conn->runCommand( "admin" , BSON( "datasize" << _ns
                                                                        << "keyPattern" << _manager->getShardKey().key() 
                                                                        << "min" << getMin() 
                                                                        << "max" << getMax() 
                                                                        ) , result ) );
        
        conn.done();
        return (long)result["numObjects"].number();
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

    void Shard::save( bool check ){
        cout << "HERE: " << _id << endl;
        bool reload = ! _lastmod;
        Model::save( check );
        cout << "\t" << _id << endl;
        if ( reload ){
            // need to do this so that we get the new _lastMod and therefore version number

            massert( "_id has to be filled in already" , ! _id.isEmpty() );
            
            string b = toString();
            BSONObj q = _id.copy();
            massert( "how could load fail?" , load( q ) );
            cout << "before: " << q << "\t" << b << endl;
            cout << "after : " << _id << "\t" << toString() << endl;
            massert( "shard reload changed content!" , b == toString() );
            massert( "id changed!" , q["_id"] == _id["_id"] );
        }
    }
    
    void Shard::ensureIndex(){
        ScopedDbConnection conn( getServer() );
        conn->ensureIndex( _ns , _manager->getShardKey().key() );
        conn.done();
    }

    string Shard::toString() const {
        stringstream ss;
        ss << "shard  ns:" << _ns << " server: " << _server << " min: " << _min << " max: " << _max;
        return ss.str();
    }
    
    
    ShardKeyPattern Shard::skey(){
        return _manager->getShardKey();
    }

    // -------  ShardManager --------

    unsigned long long ShardManager::NextSequenceNumber = 1;

    ShardManager::ShardManager( DBConfig * config , string ns , ShardKeyPattern pattern ) : _config( config ) , _ns( ns ) , _key( pattern ){
        Shard temp(0);
        
        ScopedDbConnection conn( temp.modelServer() );
        auto_ptr<DBClientCursor> cursor = conn->query( temp.getNS() , BSON( "ns" <<  ns ) );
        while ( cursor->more() ){
            Shard * s = new Shard( this );
            BSONObj d = cursor->next();
            s->unserialize( d );
            _shards.push_back( s );
            s->_id = d["_id"].wrap().getOwned();
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

        _sequenceNumber = ++NextSequenceNumber;
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

    Shard* ShardManager::findShardOnServer( const string& server ) const {

        for ( vector<Shard*>::const_iterator i=_shards.begin(); i!=_shards.end(); i++ ){
            Shard* s = *i;
            if ( s->getServer() == server )
                return s;
        }

        return 0;
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
    
    void ShardManager::ensureIndex(){
        set<string> seen;
        
        for ( vector<Shard*>::const_iterator i=_shards.begin(); i!=_shards.end(); i++ ){
            Shard* s = *i;
            if ( seen.count( s->getServer() ) )
                continue;
            seen.insert( s->getServer() );
            s->ensureIndex();
        }
    }

    void ShardManager::save(){
        ServerShardVersion a = getVersion();
        
        for ( vector<Shard*>::const_iterator i=_shards.begin(); i!=_shards.end(); i++ ){
            Shard* s = *i;
            if ( ! s->_modified )
                continue;
            s->save( true );
            _sequenceNumber = ++NextSequenceNumber;
        }
        
        massert( "how did version get smalled" , getVersion() >= a );

        ensureIndex(); // TODO: this is too aggressive - but not really sooo bad
    }
    
    ServerShardVersion ShardManager::getVersion( const string& server ) const{
        // TODO: cache or something?
        
        ServerShardVersion max = 0;

        for ( vector<Shard*>::const_iterator i=_shards.begin(); i!=_shards.end(); i++ ){
            Shard* s = *i;
            if ( s->getServer() != server )
                continue;
            
            if ( s->_lastmod > max )
                max = s->_lastmod;
        }        

        return max;
    }

    ServerShardVersion ShardManager::getVersion() const{
        ServerShardVersion max = 0;

        for ( vector<Shard*>::const_iterator i=_shards.begin(); i!=_shards.end(); i++ ){
            Shard* s = *i;
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
