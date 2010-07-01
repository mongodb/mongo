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

#include "pch.h"
#include "chunk.h"
#include "config.h"
#include "../util/unittest.h"
#include "../client/connpool.h"
#include "../client/distlock.h"
#include "../db/queryutil.h"
#include "cursors.h"
#include "strategy.h"

namespace mongo {

    inline bool allOfType(BSONType type, const BSONObj& o){
        BSONObjIterator it(o);
        while(it.more()){
            if (it.next().type() != type)
                return false;
        }
        return true;
    }

    RWLock chunkSplitLock("rw:chunkSplitLock");

    // -------  Shard --------

    int Chunk::MaxChunkSize = 1024 * 1024 * 200;
    
    Chunk::Chunk( ChunkManager * manager )
      : _manager(manager),
        _lastmod(0), _modified(false), _dataWritten(0)
    {}

    Chunk::Chunk(ChunkManager * info , const BSONObj& min, const BSONObj& max, const Shard& shard)
      : _manager(info), _min(min), _max(max), _shard(shard),
        _lastmod(0), _modified(false), _dataWritten(0)
    {}


    void Chunk::setShard( const Shard& s ){
        _shard = s;
        _manager->_migrationNotification(this);
        _markModified();
    }
    
    bool Chunk::contains( const BSONObj& obj ) const{
        return
            _manager->getShardKey().compare( getMin() , obj ) <= 0 &&
            _manager->getShardKey().compare( obj , getMax() ) < 0;
    }

    bool ChunkRange::contains(const BSONObj& obj) const {
    // same as Chunk method
        return 
            _manager->getShardKey().compare( getMin() , obj ) <= 0 &&
            _manager->getShardKey().compare( obj , getMax() ) < 0;
    }

    bool Chunk::minIsInf() const {
        return _manager->getShardKey().globalMin().woCompare( getMin() ) == 0;
    }

    bool Chunk::maxIsInf() const {
        return _manager->getShardKey().globalMax().woCompare( getMax() ) == 0;
    }
    
    BSONObj Chunk::pickSplitPoint() const{
        int sort = 0;
        
        if ( minIsInf() ){
            sort = 1;
        }
        else if ( maxIsInf() ){
            sort = -1;
        }
        
        if ( sort ){
            ShardConnection conn( getShard().getConnString() , _manager->getns() );
            Query q;
            if ( sort == 1 )
                q.sort( _manager->getShardKey().key() );
            else {
                BSONObj k = _manager->getShardKey().key();
                BSONObjBuilder r;
                
                BSONObjIterator i(k);
                while( i.more() ) {
                    BSONElement e = i.next();
                    uassert( 10163 ,  "can only handle numbers here - which i think is correct" , e.isNumber() );
                    r.append( e.fieldName() , -1 * e.number() );
                }
                
                q.sort( r.obj() );
            }
            BSONObj end = conn->findOne( _manager->getns() , q );
            conn.done();

            if ( ! end.isEmpty() )
                return _manager->getShardKey().extractKey( end );
        }
        
        ScopedDbConnection conn( getShard().getConnString() );
        BSONObj result;
        if ( ! conn->runCommand( "admin" , BSON( "medianKey" << _manager->getns()
                                                 << "keyPattern" << _manager->getShardKey().key()
                                                 << "min" << getMin()
                                                 << "max" << getMax()
                                                 ) , result ) ){
            stringstream ss;
            ss << "medianKey command failed: " << result;
            uassert( 10164 ,  ss.str() , 0 );
        }

        BSONObj median = result.getObjectField( "median" );
        if (median == getMin()){
            //TODO compound support
            BSONElement key = getMin().firstElement();
            BSONObjBuilder b;
            b.appendAs(key, "$gt");

            Query q = QUERY(key.fieldName() << b.obj());
            q.sort(_manager->getShardKey().key());

            median = conn->findOne(_manager->getns(), q);
            median = _manager->getShardKey().extractKey( median );
            PRINT(median);
        }

        conn.done();
        
        return median.getOwned();
    }

    ChunkPtr Chunk::split(){
        return split( pickSplitPoint() );
    }
    
    ChunkPtr Chunk::split( const BSONObj& m ){
        uassert( 10165 ,  "can't split as shard that doesn't have a manager" , _manager );

        log(1) << " before split on: "  << m << '\n'
               << "\t self  : " << toString() << endl;
        
        BSONObjBuilder detail(256);
        appendShortVersion( "before" , detail );
        

        uassert( 10166 ,  "locking namespace on server failed" , lockNamespaceOnServer( getShard() , _manager->getns() ) );
        uassert( 13003 ,  "can't split chunk. does it have only one distinct value?" ,
                          !m.isEmpty() && _min.woCompare(m) && _max.woCompare(m)); 

        ChunkPtr s( new Chunk( _manager, m.getOwned(), _max , _shard) );

        s->_markModified();
        _markModified();
        
        {
            rwlock lk( _manager->_lock , true );
            _manager->_chunkMap[s->getMax()] = s;
            
            setMax(m.getOwned());
            DEV assert( shared_from_this() );
            _manager->_chunkMap[_max] = shared_from_this();
        }
        
        log(1) << " after split:\n" 
               << "\t left : " << toString() << '\n' 
               << "\t right: "<< s->toString() << endl;
        
        appendShortVersion( "left" , detail );
        s->appendShortVersion( "right" , detail );
        
        _manager->save();
        
        configServer.logChange( "split" , _manager->getns() , detail.obj() );

        return s;
    }

    bool Chunk::moveAndCommit( const Shard& to , string& errmsg ){
        uassert( 10167 ,  "can't move shard to its current location!" , getShard() != to );
        
        BSONObjBuilder detail;
        detail.append( "from" , _shard.toString() );
        detail.append( "to" , to.toString() );
        appendShortVersion( "chunk" , detail );

        log() << "moving chunk ns: " << _manager->getns() << " moving ( " << toString() << ") " << _shard.toString() << " -> " << to.toString() << endl;
        
        Shard from = _shard;
        ShardChunkVersion oldVersion = _manager->getVersion( from );
        BSONObj filter;
        {
            BSONObjBuilder b;
            getFilter( b );
            filter = b.obj();
        }
        
        ScopedDbConnection fromconn( from.getConnString() );

        BSONObj startRes;
        bool worked = fromconn->runCommand( "admin" ,
                                            BSON( "movechunk.start" << _manager->getns() << 
                                                  "from" << from.getConnString() <<
                                                  "to" << to.getConnString() <<
                                                  "filter" << filter
                                                  ) ,
                                            startRes
                                            );
        
        if ( ! worked ){
            errmsg = (string)"movechunk.start failed: " + startRes.jsonString();
            fromconn.done();
            return false;
        }

        setShard( to );
        
        // need to increment version # for old server
        ChunkPtr randomChunkOnOldServer = _manager->findChunkOnServer( from );

        if ( randomChunkOnOldServer ){
            randomChunkOnOldServer->_markModified();
        }

        // update config db
        _manager->save();
        
        BSONObj finishRes;
        {

            ShardChunkVersion newVersion = _manager->getVersion( from );
            if ( newVersion == 0 && oldVersion > 0 ){
                newVersion = oldVersion;
                ++newVersion;
                _manager->save();
            }
            else if ( newVersion <= oldVersion ){
                log() << "newVersion: " << newVersion << " oldVersion: " << oldVersion << endl;
                uassert( 10168 ,  "version has to be higher" , newVersion > oldVersion );
            }
            
            BSONObjBuilder b;
            b << "movechunk.finish" << _manager->getns();
            b << "to" << to.getConnString();
            b.appendTimestamp( "newVersion" , newVersion );
            b.append( startRes["finishToken"] );
        
            worked = fromconn->runCommand( "admin" ,
                                           b.done() , 
                                           finishRes );
        }

        if ( ! worked ){
            errmsg = (string)"movechunk.finish failed: " + finishRes.toString();
            fromconn.done();
            return false;
        }

        fromconn.done();
        
        configServer.logChange( "migrate" , _manager->getns() , detail.obj() );
        return true;
    }
    
    bool Chunk::splitIfShould( long dataWritten ){
        _dataWritten += dataWritten;
        
        int myMax = MaxChunkSize;
        if ( minIsInf() || maxIsInf() ){
            myMax = (int)( (double)myMax * .9 );
        }

        if ( _dataWritten < myMax / 5 )
            return false;
        
        if ( ! chunkSplitLock.lock_try(0) )
            return false;
        
        rwlock lk( chunkSplitLock , 1 , true );

        log(1) << "\t splitIfShould : " << this << endl;

        _dataWritten = 0;
        
        BSONObj split_point = pickSplitPoint();
        if ( split_point.isEmpty() || _min == split_point || _max == split_point) {
            log() << "SHARD PROBLEM** shard is too big, but can't split: " << toString() << endl;
            return false;
        }

        long size = getPhysicalSize();
        if ( size < myMax )
            return false;
        
        log() << "autosplitting " << _manager->getns() << " size: " << size << " shard: " << toString() << endl;
        ChunkPtr newShard = split(split_point);

        moveIfShould( newShard );
        
        return true;
    }

    bool Chunk::moveIfShould( ChunkPtr newChunk ){
        ChunkPtr toMove;
       
        if ( newChunk->countObjects() <= 1 ){
            toMove = newChunk;
        }
        else if ( this->countObjects() <= 1 ){
            DEV assert( shared_from_this() );
            toMove = shared_from_this();
        }
        else {
            log(1) << "don't know how to decide if i should move inner shard" << endl;
        }

        if ( ! toMove )
            return false;
        
        Shard newLocation = Shard::pick();
        if ( getShard() == newLocation ){
            // if this is the best server, then we shouldn't do anything!
            log(1) << "not moving chunk: " << toString() << " b/c would move to same place  " << newLocation.toString() << " -> " << getShard().toString() << endl;
            return 0;
        }

        log() << "moving chunk (auto): " << toMove->toString() << " to: " << newLocation.toString() << " #objects: " << toMove->countObjects() << endl;

        string errmsg;
        massert( 10412 ,  (string)"moveAndCommit failed: " + errmsg , 
                 toMove->moveAndCommit( newLocation , errmsg ) );
        
        return true;
    }

    long Chunk::getPhysicalSize() const{
        ScopedDbConnection conn( getShard().getConnString() );
        
        BSONObj result;
        uassert( 10169 ,  "datasize failed!" , conn->runCommand( "admin" , 
                                                                 BSON( "datasize" << _manager->getns()
                                                                       << "keyPattern" << _manager->getShardKey().key() 
                                                                       << "min" << getMin() 
                                                                       << "max" << getMax() 
                                                                       << "maxSize" << ( MaxChunkSize + 1 )
                                                                       ) , result ) );
        
        conn.done();
        return (long)result["size"].number();
    }


    template <typename ChunkType>
    inline long countObjectsHelper(const ChunkType* chunk, const BSONObj& filter){
        ShardConnection conn( chunk->getShard().getConnString() , chunk->getManager()->getns() );
        
        BSONObj f = chunk->getFilter();
        if ( ! filter.isEmpty() )
            f = ClusteredCursor::concatQuery( f , filter );

        BSONObj result;
        unsigned long long n = conn->count( chunk->getManager()->getns() , f );
        
        conn.done();
        return (long)n;
    }
    
    long Chunk::countObjects( const BSONObj& filter ) const { return countObjectsHelper(this, filter); }
    long ChunkRange::countObjects( const BSONObj& filter ) const { return countObjectsHelper(this, filter); }

    void Chunk::appendShortVersion( const char * name , BSONObjBuilder& b ){
        BSONObjBuilder bb( b.subobjStart( name ) );
        bb.append( "min" , _min );
        bb.append( "max" , _max );
        bb.done();
    }
    
    bool Chunk::operator==( const Chunk& s ) const{
        return 
            _manager->getShardKey().compare( _min , s._min ) == 0 &&
            _manager->getShardKey().compare( _max , s._max ) == 0
            ;
    }

    void Chunk::getFilter( BSONObjBuilder& b ) const{
        _manager->getShardKey().getFilter( b , _min , _max );
    }
    void ChunkRange::getFilter( BSONObjBuilder& b ) const{
        _manager->getShardKey().getFilter( b , _min , _max );
    }
    
    void Chunk::serialize(BSONObjBuilder& to,ShardChunkVersion myLastMod){
        
        to.append( "_id" , genID( _manager->getns() , _min ) );

        if ( myLastMod.isSet() ){
            to.appendTimestamp( "lastmod" , myLastMod );
        }
        else if ( _lastmod.isSet() ){
            assert( _lastmod > 0 && _lastmod < 1000 );
            to.appendTimestamp( "lastmod" , _lastmod );
        }
        else {
            assert(0);
        }

        to << "ns" << _manager->getns();
        to << "min" << _min;
        to << "max" << _max;
        to << "shard" << _shard.getName();
    }

    string Chunk::genID( const string& ns , const BSONObj& o ){
        StringBuilder buf( ns.size() + o.objsize() + 16 );
        buf << ns << "-";

        BSONObjIterator i(o);
        while ( i.more() ){
            BSONElement e = i.next();
            buf << e.fieldName() << "_" << e.toString( false );
        }

        return buf.str();
    }
    
    void Chunk::unserialize(const BSONObj& from){
        string ns = from.getStringField( "ns" );
        _shard.reset( from.getStringField( "shard" ) );

        _lastmod = from.hasField( "lastmod" ) ? from["lastmod"]._numberLong() : 0;
        assert( _lastmod > 0 );

        BSONElement e = from["minDotted"];

        if (e.eoo()){
            _min = from.getObjectField( "min" ).getOwned();
            _max = from.getObjectField( "max" ).getOwned();
        } 
        else { // TODO delete this case after giving people a chance to migrate
            _min = e.embeddedObject().getOwned();
            _max = from.getObjectField( "maxDotted" ).getOwned();
        }
        
        uassert( 10170 ,  "Chunk needs a ns" , ! ns.empty() );
        uassert( 13327 ,  "Chunk ns must match server ns" , ns == _manager->getns() );

        uassert( 10171 ,  "Chunk needs a server" , _shard.ok() );

        uassert( 10172 ,  "Chunk needs a min" , ! _min.isEmpty() );
        uassert( 10173 ,  "Chunk needs a max" , ! _max.isEmpty() );
    }

    string Chunk::modelServer() {
        // TODO: this could move around?
        return configServer.modelServer();
    }
    
    void Chunk::_markModified(){
        _modified = true;
    }

    void Chunk::ensureIndex(){
        ScopedDbConnection conn( getShard().getConnString() );
        conn->ensureIndex( _manager->getns() , _manager->getShardKey().key() , _manager->_unique );
        conn.done();
    }

    string Chunk::toString() const {
        stringstream ss;
        ss << "shard  ns:" << _manager->getns() << " shard: " << _shard.toString() << " lastmod: " << _lastmod.toString() << " min: " << _min << " max: " << _max;
        return ss.str();
    }
    
    
    ShardKeyPattern Chunk::skey() const{
        return _manager->getShardKey();
    }

    // -------  ChunkManager --------

    AtomicUInt ChunkManager::NextSequenceNumber = 1;

    ChunkManager::ChunkManager( DBConfig * config , string ns , ShardKeyPattern pattern , bool unique ) : 
        _config( config ) , _ns( ns ) , 
        _key( pattern ) , _unique( unique ) , 
        _sequenceNumber(  ++NextSequenceNumber ), _lock("rw:ChunkManager")
    {
        _reload_inlock();
        
        if ( _chunkMap.empty() ){
            ChunkPtr c( new Chunk(this, _key.globalMin(), _key.globalMax(), config->getPrimary()) );
            c->_markModified();
            
            _chunkMap[c->getMax()] = c;
            _chunkRanges.reloadAll(_chunkMap);

            _shards.insert(c->getShard());

            save_inlock();
            log() << "no chunks for:" << ns << " so creating first: " << c->toString() << endl;
        }
    }
    
    ChunkManager::~ChunkManager(){
        _chunkMap.clear();
        _chunkRanges.clear();
        _shards.clear();
    }
    
    void ChunkManager::_reload(){
        rwlock lk( _lock , true );
        _reload_inlock();
    }

    void ChunkManager::_reload_inlock(){
        int tries = 3;
        while (tries--){
            _chunkMap.clear();
            _chunkRanges.clear();
            _shards.clear();
            _load();

            if (_isValid()){
                _chunkRanges.reloadAll(_chunkMap);
                return;
            }

            _printChunks();
            sleepmillis(10 * (3-tries));
            sleepsecs(10);
        }
        msgasserted(13282, "Couldn't load a valid config for " + _ns + " after 3 tries. Giving up");
        
    }

    void ChunkManager::_load(){
        static Chunk temp(0);
        
        ScopedDbConnection conn( temp.modelServer() );

        auto_ptr<DBClientCursor> cursor = conn->query( temp.getNS() , BSON( "ns" <<  _ns ) );
        while ( cursor->more() ){
            BSONObj d = cursor->next();
            if ( d["isMaxMarker"].trueValue() ){
                continue;
            }
            
            ChunkPtr c( new Chunk( this ) );
            c->unserialize( d );

            _chunkMap[c->getMax()] = c;
            _shards.insert(c->getShard());

        }
        conn.done();
    }

    bool ChunkManager::_isValid() const {
#define ENSURE(x) do { if(!(x)) { log() << "ChunkManager::_isValid failed: " #x << endl; return false; } } while(0)

        if (_chunkMap.empty())
            return true;

        // Check endpoints
        ENSURE(allOfType(MinKey, _chunkMap.begin()->second->getMin()));
        ENSURE(allOfType(MaxKey, prior(_chunkMap.end())->second->getMax()));

        // Make sure there are no gaps or overlaps
        for (ChunkMap::const_iterator it=boost::next(_chunkMap.begin()), end=_chunkMap.end(); it != end; ++it){
            ChunkMap::const_iterator last = prior(it);
            ENSURE(it->second->getMin() == last->second->getMax());
        }

        return true;

#undef ENSURE
    }

    void ChunkManager::_printChunks() const {
        for (ChunkMap::const_iterator it=_chunkMap.begin(), end=_chunkMap.end(); it != end; ++it) {
            log() << *it->second << endl;
        }
    }

    bool ChunkManager::hasShardKey( const BSONObj& obj ){
        return _key.hasShardKey( obj );
    }

    ChunkPtr ChunkManager::findChunk( const BSONObj & obj , bool retry ){
        BSONObj key = _key.extractKey(obj);
        
        {
            BSONObj foo;
            ChunkPtr c;
            {
                rwlock lk( _lock , false ); 
                ChunkMap::iterator it = _chunkMap.upper_bound(key);
                if (it != _chunkMap.end()){
                    foo = it->first;
                    c = it->second;
                }
            }
            
            if ( c ){
                if ( c->contains( obj ) )
                    return c;
                
                PRINT(foo);
                PRINT(*c);
                PRINT(key);
                
                _reload();
                massert(13141, "Chunk map pointed to incorrect chunk", false);
            }
        }

        if ( retry ){
            stringstream ss;
            ss << "couldn't find a chunk aftry retry which should be impossible extracted: " << key;
            throw UserException( 8070 , ss.str() );
        }
        
        log() << "ChunkManager: couldn't find chunk for: " << key << " going to retry" << endl;
        _reload();
        return findChunk( obj , true );
    }

    ChunkPtr ChunkManager::findChunkOnServer( const Shard& shard ) const {
        rwlock lk( _lock , false ); 
 
        for ( ChunkMap::const_iterator i=_chunkMap.begin(); i!=_chunkMap.end(); ++i ){
            ChunkPtr c = i->second;
            if ( c->getShard() == shard )
                return c;
        }

        return ChunkPtr();
    }

    int ChunkManager::_getChunksForQuery( vector<shared_ptr<ChunkRange> >& chunks , const BSONObj& query ){
        rwlock lk( _lock , false ); 

        FieldRangeSet ranges(_ns.c_str(), query, false);
        BSONObjIterator fields(_key.key());
        BSONElement field = fields.next();
        FieldRange range = ranges.range(field.fieldName());
        
        uassert(13088, "no support for special queries yet", range.getSpecial().empty());

        if (range.empty()) {
            return 0;

        } else if (range.equality()) {
            chunks.push_back( _chunkRanges.upper_bound(BSON(field.fieldName() << range.min()))->second );
            return 1;
        } else if (!range.nontrivial()) {
            return -1; // all chunks
        } else {
            set<shared_ptr<ChunkRange>, ChunkCmp> chunkSet;

            for (vector<FieldInterval>::const_iterator it=range.intervals().begin(), end=range.intervals().end();
                 it != end;
                 ++it)
            {
                const FieldInterval& fi = *it;
                assert(fi.valid());
                BSONObj minObj = BSON(field.fieldName() << fi._lower._bound);
                BSONObj maxObj = BSON(field.fieldName() << fi._upper._bound);
                ChunkRangeMap::const_iterator min, max;
                min = (fi._lower._inclusive ? _chunkRanges.upper_bound(minObj) : _chunkRanges.lower_bound(minObj));
                max = (fi._upper._inclusive ? _chunkRanges.upper_bound(maxObj) : _chunkRanges.lower_bound(maxObj));

                assert(min != _chunkRanges.ranges().end());

                // make max non-inclusive like end iterators
                if(max != _chunkRanges.ranges().end())
                    ++max;

                for (ChunkRangeMap::const_iterator it=min; it != max; ++it){
                    chunkSet.insert(it->second);
                }
            }

            chunks.assign(chunkSet.begin(), chunkSet.end());
            return chunks.size();
        }
    }

    int ChunkManager::getChunksForQuery( vector<shared_ptr<ChunkRange> >& chunks , const BSONObj& query ){
        int ret = _getChunksForQuery(chunks, query);

        if (ret == -1){
            for (ChunkRangeMap::const_iterator it=_chunkRanges.ranges().begin(), end=_chunkRanges.ranges().end(); it != end; ++it){
                chunks.push_back(it->second);
            }
        }
        return chunks.size();
        //return ret;
    }

    int ChunkManager::getShardsForQuery( set<Shard>& shards , const BSONObj& query ){
        vector<shared_ptr<ChunkRange> > chunks;
        int ret = _getChunksForQuery(chunks, query);

        if (ret == -1){
            getAllShards(shards);
        } 
        else {
            for ( vector<shared_ptr<ChunkRange> >::iterator it=chunks.begin(), end=chunks.end(); it != end; ++it ){
                shared_ptr<ChunkRange> c = *it;
                shards.insert(c->getShard());
            }
        }

        return shards.size();
    }

    void ChunkManager::getAllShards( set<Shard>& all ){
        rwlock lk( _lock , false ); 
        all = _shards;
    }
    
    void ChunkManager::ensureIndex(){
        rwlock lk( _lock , false ); 
 
        set<Shard> seen;
        
        for ( ChunkMap::const_iterator i=_chunkMap.begin(); i!=_chunkMap.end(); ++i ){
            ChunkPtr c = i->second;
            if ( seen.count( c->getShard() ) )
                continue;
            seen.insert( c->getShard() );
            c->ensureIndex();
        }
    }
    
    void ChunkManager::drop( ChunkManagerPtr me ){
        rwlock lk( _lock , true ); 
        
        uassert( 10174 ,  "config servers not all up" , configServer.allUp() );
        
        map<Shard,ShardChunkVersion> seen;
        
        log(1) << "ChunkManager::drop : " << _ns << endl;

        // lock all shards so no one can do a split/migrate
        for ( ChunkMap::const_iterator i=_chunkMap.begin(); i!=_chunkMap.end(); ++i ){
            ChunkPtr c = i->second;
            ShardChunkVersion& version = seen[ c->getShard() ];
            if ( version.isSet() ) 
                continue;
            version = lockNamespaceOnServer( c->getShard() , _ns );
            if ( version.isSet() )
                continue;

            // rollback
            uassert( 10175 ,  "don't know how to rollback locks b/c drop can't lock all shards" , 0 );
        }
        
        log(1) << "ChunkManager::drop : " << _ns << "\t all locked" << endl;        

        // wipe my meta-data
        _chunkMap.clear();
        _chunkRanges.clear();
        _shards.clear();

        
        // delete data from mongod
        for ( map<Shard,ShardChunkVersion>::iterator i=seen.begin(); i!=seen.end(); i++ ){
            ScopedDbConnection conn( i->first );
            conn->dropCollection( _ns );
            conn.done();
        }
        
        log(1) << "ChunkManager::drop : " << _ns << "\t removed shard data" << endl;        

        // clean up database meta-data
        uassert( 10176 ,  "no sharding data?" , _config->removeSharding( _ns ) );
        _config->save();
        
        
        // remove chunk data
        static Chunk temp(0);
        ScopedDbConnection conn( temp.modelServer() );
        conn->remove( temp.getNS() , BSON( "ns" << _ns ) );
        conn.done();
        log(1) << "ChunkManager::drop : " << _ns << "\t removed chunk data" << endl;                
        
        for ( map<Shard,ShardChunkVersion>::iterator i=seen.begin(); i!=seen.end(); i++ ){
            ScopedDbConnection conn( i->first );
            BSONObj res;
            if ( ! setShardVersion( conn.conn() , _ns , 0 , true , res ) )
                throw UserException( 8071 , (string)"OH KNOW, cleaning up after drop failed: " + res.toString() );
            conn.done();
        }


        log(1) << "ChunkManager::drop : " << _ns << "\t DONE" << endl;        
    }
    
    void ChunkManager::save(){
        rwlock lk( _lock , false ); 
        save_inlock();
    }
    
    void ChunkManager::save_inlock(){
        
        ShardChunkVersion a = getVersion_inlock();
        assert( a > 0 || _chunkMap.size() <= 1 );
        ShardChunkVersion nextChunkVersion = a.incMajor();
        vector<ChunkPtr> toFix;
        vector<ShardChunkVersion> newVersions;
        
        BSONObjBuilder cmdBuilder;
        BSONArrayBuilder updates( cmdBuilder.subarrayStart( "applyOps" ) );
        
        
        int numOps = 0;
        for ( ChunkMap::const_iterator i=_chunkMap.begin(); i!=_chunkMap.end(); ++i ){
            ChunkPtr c = i->second;
            if ( ! c->_modified )
                continue;

            numOps++;
            _sequenceNumber = ++NextSequenceNumber;

            ShardChunkVersion myVersion = nextChunkVersion;
            ++nextChunkVersion;
            toFix.push_back( c );
            newVersions.push_back( myVersion );

            BSONObjBuilder op;
            op.append( "op" , "u" );
            op.appendBool( "b" , true );
            op.append( "ns" , ShardNS::chunk );

            BSONObjBuilder n( op.subobjStart( "o" ) );
            c->serialize( n , myVersion );
            n.done();

            BSONObjBuilder q( op.subobjStart( "o2" ) );
            q.append( "_id" , c->genID() );
            q.done();

            updates.append( op.obj() );
        }
        
        if ( numOps == 0 )
            return;
        
        updates.done();
        
        if ( a > 0 || _chunkMap.size() > 1 ){
            BSONArrayBuilder temp( cmdBuilder.subarrayStart( "preCondition" ) );
            BSONObjBuilder b;
            b.append( "ns" , ShardNS::chunk );
            b.append( "q" , BSON( "query" << BSON( "ns" << _ns ) << "orderby" << BSON( "lastmod" << -1 ) ) );
            {
                BSONObjBuilder bb( b.subobjStart( "res" ) );
                bb.appendTimestamp( "lastmod" , a );
                bb.done();
            }
            temp.append( b.obj() );
            temp.done();
        }

        BSONObj cmd = cmdBuilder.obj();
        
        ScopedDbConnection conn( Chunk(0).modelServer() );
        BSONObj res;
        bool ok = conn->runCommand( "config" , cmd , res );
        //conn.done();
        if ( ! ok ){
            stringstream ss;
            ss << "saving chunks failed.  cmd: " << cmd << " result: " << res;
            log( LL_ERROR ) << ss.str() << endl;
            msgasserted( 13327 , ss.str() );
        }

        for ( unsigned i=0; i<toFix.size(); i++ ){
            toFix[i]->_lastmod = newVersions[i];
        }

        massert( 10417 ,  "how did version get smalled" , getVersion_inlock() >= a );
        
        ensureIndex(); // TODO: this is too aggressive - but not really sooo bad
    }
    
    ShardChunkVersion ChunkManager::getVersion( const Shard& shard ) const{
        rwlock lk( _lock , false ); 
        // TODO: cache or something?
        
        ShardChunkVersion max = 0;

        for ( ChunkMap::const_iterator i=_chunkMap.begin(); i!=_chunkMap.end(); ++i ){
            ChunkPtr c = i->second;
            DEV assert( c );
            if ( c->getShard() != shard )
                continue;
            if ( c->_lastmod > max )
                max = c->_lastmod;
        }        
        return max;
    }

    ShardChunkVersion ChunkManager::getVersion() const{
        rwlock lk( _lock , false ); 
        return getVersion_inlock();
    }
    
    ShardChunkVersion ChunkManager::getVersion_inlock() const{
        rwlock lk( _lock , false ); 
        
        ShardChunkVersion max = 0;
        
        for ( ChunkMap::const_iterator i=_chunkMap.begin(); i!=_chunkMap.end(); ++i ){
            ChunkPtr c = i->second;
            if ( c->_lastmod > max )
                max = c->_lastmod;
        }        

        return max;
    }

    string ChunkManager::toString() const {
        rwlock lk( _lock , false );         

        stringstream ss;
        ss << "ChunkManager: " << _ns << " key:" << _key.toString() << '\n';
        for ( ChunkMap::const_iterator i=_chunkMap.begin(); i!=_chunkMap.end(); ++i ){
            const ChunkPtr c = i->second;
            ss << "\t" << c->toString() << '\n';
        }
        return ss.str();
    }

    void ChunkManager::_migrationNotification(Chunk* c){
        _chunkRanges.reloadRange(_chunkMap, c->getMin(), c->getMax());
        _shards.insert(c->getShard());
    }

    
    void ChunkRangeManager::assertValid() const{
        if (_ranges.empty())
            return;

        try {
            // No Nulls
            for (ChunkRangeMap::const_iterator it=_ranges.begin(), end=_ranges.end(); it != end; ++it){
                assert(it->second);
            }
            
            // Check endpoints
            assert(allOfType(MinKey, _ranges.begin()->second->getMin()));
            assert(allOfType(MaxKey, prior(_ranges.end())->second->getMax()));

            // Make sure there are no gaps or overlaps
            for (ChunkRangeMap::const_iterator it=boost::next(_ranges.begin()), end=_ranges.end(); it != end; ++it){
                ChunkRangeMap::const_iterator last = prior(it);
                assert(it->second->getMin() == last->second->getMax());
            }

            // Check Map keys
            for (ChunkRangeMap::const_iterator it=_ranges.begin(), end=_ranges.end(); it != end; ++it){
                assert(it->first == it->second->getMax());
            }

            // Make sure we match the original chunks
            const ChunkMap chunks = _ranges.begin()->second->getManager()->_chunkMap;
            for ( ChunkMap::const_iterator i=chunks.begin(); i!=chunks.end(); ++i ){
                const ChunkPtr chunk = i->second;

                ChunkRangeMap::const_iterator min = _ranges.upper_bound(chunk->getMin());
                ChunkRangeMap::const_iterator max = _ranges.lower_bound(chunk->getMax());

                assert(min != _ranges.end());
                assert(max != _ranges.end());
                assert(min == max);
                assert(min->second->getShard() == chunk->getShard());
                assert(min->second->contains( chunk->getMin() ));
                assert(min->second->contains( chunk->getMax() ) || (min->second->getMax() == chunk->getMax()));
            }
            
        } catch (...) {
            cout << "\t invalid ChunkRangeMap! printing ranges:" << endl;

            for (ChunkRangeMap::const_iterator it=_ranges.begin(), end=_ranges.end(); it != end; ++it)
                cout << it->first << ": " << *it->second << endl;

            throw;
        }
    }

    void ChunkRangeManager::reloadRange(const ChunkMap& chunks, const BSONObj& min, const BSONObj& max){
        if (_ranges.empty()){
            reloadAll(chunks);
            return;
        }
        
        ChunkRangeMap::iterator low  = _ranges.upper_bound(min);
        ChunkRangeMap::iterator high = _ranges.lower_bound(max);
        
        assert(low != _ranges.end());
        assert(high != _ranges.end());
        assert(low->second);
        assert(high->second);

        ChunkMap::const_iterator begin = chunks.upper_bound(low->second->getMin());
        ChunkMap::const_iterator end   = chunks.lower_bound(high->second->getMax());

        assert(begin != chunks.end());
        assert(end != chunks.end());

        // C++ end iterators are one-past-last
        ++high;
        ++end;

        // update ranges
        _ranges.erase(low, high); // invalidates low
        _insertRange(begin, end);

        assert(!_ranges.empty());
        DEV assertValid();

        // merge low-end if possible
        low = _ranges.upper_bound(min);
        assert(low != _ranges.end());
        if (low != _ranges.begin()){
            shared_ptr<ChunkRange> a = prior(low)->second;
            shared_ptr<ChunkRange> b = low->second;
            if (a->getShard() == b->getShard()){
                shared_ptr<ChunkRange> cr (new ChunkRange(*a, *b));
                _ranges.erase(prior(low));
                _ranges.erase(low); // invalidates low
                _ranges[cr->getMax()] = cr;
            }
        }

        DEV assertValid();

        // merge high-end if possible
        high = _ranges.lower_bound(max);
        if (high != prior(_ranges.end())){
            shared_ptr<ChunkRange> a = high->second;
            shared_ptr<ChunkRange> b = boost::next(high)->second;
            if (a->getShard() == b->getShard()){
                shared_ptr<ChunkRange> cr (new ChunkRange(*a, *b));
                _ranges.erase(boost::next(high));
                _ranges.erase(high); //invalidates high
                _ranges[cr->getMax()] = cr;
            }
        }

        DEV assertValid();
    }

    void ChunkRangeManager::reloadAll(const ChunkMap& chunks){
        _ranges.clear();
        _insertRange(chunks.begin(), chunks.end());

        DEV assertValid();
    }

    void ChunkRangeManager::_insertRange(ChunkMap::const_iterator begin, const ChunkMap::const_iterator end){
        while (begin != end){
            ChunkMap::const_iterator first = begin;
            Shard shard = first->second->getShard();
            while (begin != end && (begin->second->getShard() == shard))
                ++begin;

            shared_ptr<ChunkRange> cr (new ChunkRange(first, begin));
            _ranges[cr->getMax()] = cr;
        }
    }
    
    class ChunkObjUnitTest : public UnitTest {
    public:
        void runShard(){
            ChunkPtr c;
            assert( ! c );
            c.reset( new Chunk( 0 ) );
            assert( c );
        }
        
        void runShardChunkVersion(){
            vector<ShardChunkVersion> all;
            all.push_back( ShardChunkVersion(1,1) );
            all.push_back( ShardChunkVersion(1,2) );
            all.push_back( ShardChunkVersion(2,1) );
            all.push_back( ShardChunkVersion(2,2) );
            
            for ( unsigned i=0; i<all.size(); i++ ){
                for ( unsigned j=i+1; j<all.size(); j++ ){
                    assert( all[i] < all[j] );
                }
            }

        }

        void run(){
            runShard();
            runShardChunkVersion();
            log(1) << "shardObjTest passed" << endl;
        }
    } shardObjTest;


} // namespace mongo
