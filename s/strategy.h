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

        virtual void sendNextBatch( Request& r ) = 0;
        
    protected:
        auto_ptr<DBClientCursor> query( const string& server , int num );
                                        
        string _ns;
        int _options;
        BSONObj _query;
        BSONObj _fields;

        long long _id;
    };
    
    extern Strategy * SINGLE;
    extern Strategy * SHARDED;
}

