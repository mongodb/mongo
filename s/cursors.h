// cursors.h

#pragma once 

#include "../stdafx.h"

#include "../db/jsobj.h"
#include "../db/dbmessage.h"
#include "../client/dbclient.h"
#include "../client/parallel.h"

#include "request.h"

namespace mongo {

    class ShardedClientCursor {
    public:
        ShardedClientCursor( QueryMessage& q , ClusteredCursor * cursor );
        virtual ~ShardedClientCursor();

        long long getId(){ return _id; }
        
        /**
         * @return whether there is more data left
         */
        bool sendNextBatch( Request& r ){ return sendNextBatch( r , _ntoreturn ); }
        bool sendNextBatch( Request& r , int ntoreturn );
        
    protected:
        
        ClusteredCursor * _cursor;
        
        int _skip;
        int _ntoreturn;

        int _totalSent;
        bool _done;

        long long _id;
    };
    
    class CursorCache {
    public:
        CursorCache();
        ~CursorCache();
        
        ShardedClientCursor * get( long long id );
        void store( ShardedClientCursor* cursor );
        void remove( long long id );

    private:
        map<long long,ShardedClientCursor*> _cursors;
    };
    
    extern CursorCache cursorCache;
}
