// strategy.h

#pragma once

#include "../stdafx.h"
#include "chunk.h"
#include "request.h"

namespace mongo {
    
    class Strategy {
    public:
        Strategy(){}
        virtual ~Strategy() {}
        virtual void queryOp( Request& r ) = 0;
        virtual void getMore( Request& r ) = 0;
        virtual void writeOp( int op , Request& r ) = 0;
        
    protected:
        void doWrite( int op , Request& r , string server );
        void doQuery( Request& r , string server );
        
        void insert( string server , const char * ns , const BSONObj& obj );
        
    };

    extern Strategy * SINGLE;
    extern Strategy * SHARDED;

    bool setShardVersion( DBClientBase & conn , const string& ns , ShardChunkVersion version , bool authoritative , BSONObj& result );

    bool lockNamespaceOnServer( const string& server , const string& ns );
    bool lockNamespaceOnServer( DBClientBase& conn , const string& ns );

}

