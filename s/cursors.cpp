// cursors.cpp

#include "stdafx.h"
#include "cursors.h"
#include "../client/connpool.h"
#include "../db/queryutil.h"

namespace mongo {
    
    // --------  ShardedCursor -----------

    ShardedClientCursor::ShardedClientCursor( QueryMessage& q , ClusteredCursor * cursor ){
        assert( cursor );
        _cursor = cursor;
        
        _skip = q.ntoskip;
        _ntoreturn = q.ntoreturn;
        
        _totalSent = 0;
        _done = false;

        do {
            _id = security.getNonce();
        } while ( _id == 0 );

    }

    ShardedClientCursor::~ShardedClientCursor(){
        assert( _cursor );
        delete _cursor;
        _cursor = 0;
    }

    bool ShardedClientCursor::sendNextBatch( Request& r , int ntoreturn ){
        uassert( "cursor already done" , ! _done );
                
        int maxSize = 1024 * 1024;
        if ( _totalSent > 0 )
            maxSize *= 3;
        
        BufBuilder b(32768);
        
        int num = 0;
        bool sendMore = true;

        while ( _cursor->more() ){
            BSONObj o = _cursor->next();

            b.append( (void*)o.objdata() , o.objsize() );
            num++;
            
            if ( b.len() > maxSize )
                break;

            if ( num == ntoreturn ){
                // soft limit aka batch size
                break;
            }

            if ( ntoreturn != 0 && ( -1 * num + _totalSent ) == ntoreturn ){
                // hard limit - total to send
                sendMore = false;
                break;
            }
        }

        bool hasMore = sendMore && _cursor->more();
        log(6) << "\t hasMore:" << hasMore << " wouldSendMoreIfHad: " << sendMore << " id:" << _id << " totalSent: " << _totalSent << endl;
        
        replyToQuery( 0 , r.p() , r.m() , b.buf() , b.len() , num , _totalSent , hasMore ? _id : 0 );
        _totalSent += num;
        _done = ! hasMore;
        
        return hasMore;
    }
    

    CursorCache::CursorCache(){
    }

    CursorCache::~CursorCache(){
        // TODO: delete old cursors?
    }

    ShardedClientCursor* CursorCache::get( long long id ){
        map<long long,ShardedClientCursor*>::iterator i = _cursors.find( id );
        if ( i == _cursors.end() ){
            OCCASIONALLY log() << "Sharded CursorCache missing cursor id: " << id << endl;
            return 0;
        }
        return i->second;
    }
    
    void CursorCache::store( ShardedClientCursor * cursor ){
        _cursors[cursor->getId()] = cursor;
    }
    void CursorCache::remove( long long id ){
        _cursors.erase( id );
    }

    CursorCache cursorCache;
}
