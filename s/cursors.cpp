// cursors.cpp

#include "stdafx.h"
#include "cursors.h"
#include "../client/connpool.h"
#include "../db/queryutil.h"

namespace mongo {
    
    // --------  ShardedCursor -----------

    ShardedCursor::ShardedCursor( QueryMessage& q ){
        _ns = q.ns;
        _query = q.query.copy();
        _options = q.queryOptions;
        _skip = q.ntoskip;
        _ntoreturn = q.ntoreturn;
        
        _totalSent = 0;
        _done = false;

        if ( q.fields.get() ){
            BSONObjBuilder b;
            for ( set<string>::iterator i=q.fields->begin(); i!=q.fields->end(); i++)
                b.append( i->c_str() , 1 );
            _fields = b.obj();
        }
        else {
            _fields = BSONObj();
        }
        
        do {
            _id = security.getNonce();
        } while ( _id == 0 );

    }

    ShardedCursor::~ShardedCursor(){
        _done = true; // just in case
    }
    
    auto_ptr<DBClientCursor> ShardedCursor::query( const string& server , int num , BSONObj extra ){
        uassert( "cursor already done" , ! _done );
        
        BSONObj q = _query;
        if ( ! extra.isEmpty() ){
            q = concatQuery( q , extra );
        }

        ScopedDbConnection conn( server );
        checkShardVersion( conn.conn() , _ns );

        log(5) << "ShardedCursor::query  server:" << server << " ns:" << _ns << " query:" << q << " num:" << num << " _fields:" << _fields << " options: " << _options << endl;
        auto_ptr<DBClientCursor> cursor = conn->query( _ns.c_str() , q , num , 0 , ( _fields.isEmpty() ? 0 : &_fields ) , _options );
        conn.done();
        return cursor;
    }

    BSONObj ShardedCursor::concatQuery( const BSONObj& query , const BSONObj& extraFilter ){
        if ( ! query.hasField( "query" ) )
            return _concatFilter( query , extraFilter );
        
        BSONObjBuilder b;
        BSONObjIterator i( query );
        while ( i.more() ){
            BSONElement e = i.next();
            if ( e.eoo() )
                break;

            if ( strcmp( e.fieldName() , "query" ) ){
                b.append( e );
                continue;
            }
            
            b.append( "query" , _concatFilter( e.embeddedObjectUserCheck() , extraFilter ) );
        }
        return b.obj();
    }
    
    BSONObj ShardedCursor::_concatFilter( const BSONObj& filter , const BSONObj& extra ){
        BSONObjBuilder b;
        b.appendElements( filter );
        b.appendElements( extra );
        
        FieldBoundSet s( "wrong" , b.obj() );
        return s.simplifiedQuery();
    }

    bool ShardedCursor::sendNextBatch( Request& r , int ntoreturn ){
        uassert( "cursor already done" , ! _done );
                
        int maxSize = 1024 * 1024;
        if ( _totalSent > 0 )
            maxSize *= 3;
        
        BufBuilder b(32768);
        
        int num = 0;
        bool sendMore = true;

        while ( more() ){
            BSONObj o = next();

            b.append( (void*)o.objdata() , o.objsize() );
            num++;
            
            if ( b.len() > maxSize )
                break;

            if ( num == ntoreturn ){
                // soft limit aka batch size
                break;
            }

            if ( ( -1 * num + _totalSent ) == ntoreturn ){
                // hard limit - total to send
                sendMore = false;
                break;
            }
        }

        bool hasMore = sendMore && more();
        log(6) << "\t hasMore:" << hasMore << " id:" << _id << endl;
        
        replyToQuery( 0 , r.p() , r.m() , b.buf() , b.len() , num , 0 , hasMore ? _id : 0 );
        _totalSent += num;
        _done = ! hasMore;
        
        return hasMore;
    }
    
    // --------  SerialServerShardedCursor -----------
    
    SerialServerShardedCursor::SerialServerShardedCursor( set<ServerAndQuery> servers , QueryMessage& q , int sortOrder) : ShardedCursor( q ){
        for ( set<ServerAndQuery>::iterator i = servers.begin(); i!=servers.end(); i++ )
            _servers.push_back( *i );
        
        if ( sortOrder > 0 )
            sort( _servers.begin() , _servers.end() );
        else if ( sortOrder < 0 )
            sort( _servers.rbegin() , _servers.rend() );
        
        _serverIndex = 0;
    }
    
    bool SerialServerShardedCursor::more(){
        if ( _current.get() && _current->more() )
            return true;
        
        if ( _serverIndex >= _servers.size() )
            return false;
        
        ServerAndQuery& sq = _servers[_serverIndex++];
        _current = query( sq._server , 0 , sq._extra );
        return _current->more();
    }
    
    BSONObj SerialServerShardedCursor::next(){
        uassert( "no more items" , more() );
        return _current->next();
    }

    // --------  ParallelSortShardedCursor -----------
    
    ParallelSortShardedCursor::ParallelSortShardedCursor( set<ServerAndQuery> servers , QueryMessage& q , const BSONObj& sortKey ) : ShardedCursor( q ) , _servers( servers ){
        _numServers = servers.size();
        _sortKey = sortKey.getOwned();

        _cursors = new auto_ptr<DBClientCursor>[_numServers];
        _nexts = new BSONObj[_numServers];
            
        // TODO: parellize
        int num = 0;
        for ( set<ServerAndQuery>::iterator i = servers.begin(); i!=servers.end(); i++ ){
            const ServerAndQuery& sq = *i;
            _cursors[num++] = query( sq._server , 0 , sq._extra );
        }
            
    }
    
    ParallelSortShardedCursor::~ParallelSortShardedCursor(){
        delete [] _cursors;
        delete [] _nexts;
    }

    bool ParallelSortShardedCursor::more(){
        for ( int i=0; i<_numServers; i++ ){
            if ( ! _nexts[i].isEmpty() )
                return true;

            if ( _cursors[i].get() && _cursors[i]->more() )
                return true;
        }
        return false;
    }
        
    BSONObj ParallelSortShardedCursor::next(){
        advance();
            
        BSONObj best = BSONObj();
        int bestFrom = -1;
            
        for ( int i=0; i<_numServers; i++){
            if ( _nexts[i].isEmpty() )
                continue;

            if ( best.isEmpty() ){
                best = _nexts[i];
                bestFrom = i;
                continue;
            }
                
            int comp = best.woSortOrder( _nexts[i] , _sortKey );
            if ( comp < 0 )
                continue;
                
            best = _nexts[i];
            bestFrom = i;
        }
            
        uassert( "no more elements" , ! best.isEmpty() );
        _nexts[bestFrom] = BSONObj();
            
        return best;
    }

    void ParallelSortShardedCursor::advance(){
        for ( int i=0; i<_numServers; i++ ){

            if ( ! _nexts[i].isEmpty() ){
                // already have a good object there
                continue;
            }
                
            if ( ! _cursors[i]->more() ){
                // cursor is dead, oh well
                continue;
            }

            _nexts[i] = _cursors[i]->next();
        }
            
    }

    CursorCache::CursorCache(){
    }

    CursorCache::~CursorCache(){
        // TODO: delete old cursors?
    }

    ShardedCursor* CursorCache::get( long long id ){
        map<long long,ShardedCursor*>::iterator i = _cursors.find( id );
        if ( i == _cursors.end() ){
            OCCASIONALLY log() << "Sharded CursorCache missing cursor id: " << id << endl;
            return 0;
        }
        return i->second;
    }
    
    void CursorCache::store( ShardedCursor * cursor ){
        _cursors[cursor->getId()] = cursor;
    }
    void CursorCache::remove( long long id ){
        _cursors.erase( id );
    }

    CursorCache cursorCache;
}
