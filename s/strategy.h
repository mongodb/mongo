// strategy.h

#pragma once

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

    class ShardedCursor {
    public:
        ShardedCursor( QueryMessage& q );
        virtual ~ShardedCursor();

        virtual bool more() = 0;
        virtual BSONObj next() = 0;
        
        void sendNextBatch( Request& r );
        
    protected:
        auto_ptr<DBClientCursor> query( const string& server , int num = 0 , BSONObj extraFilter = emptyObj );

        BSONObj concatQuery( const BSONObj& query , const BSONObj& extraFilter );
        BSONObj _concatFilter( const BSONObj& filter , const BSONObj& extraFilter );

        string _ns;
        int _options;
        BSONObj _query;
        BSONObj _fields;

        long long _id;
    };
    
    extern Strategy * SINGLE;
    extern Strategy * SHARDED;
}

