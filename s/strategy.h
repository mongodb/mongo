// strategy.h

#pragma once

#include "stdafx.h"
#include "shard.h"

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

    void checkShardVersion( DBClientBase & conn , const string& ns , bool authoritative = false );
    
    bool setShardVersion( DBClientBase & conn , const string& ns , ServerShardVersion version , bool authoritative , BSONObj& result );

    extern Strategy * SINGLE;
    extern Strategy * SHARDED;
}

