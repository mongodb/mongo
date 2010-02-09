// client.cpp

#include "stdafx.h"
#include "../client/dbclient.h"
#include "dbtests.h"
 
namespace ClientTests {
    
    class Base {
    public:
        
        Base( string coll ){
            _ns = (string)"test." + coll;
        }
        
        virtual ~Base(){
            db.dropCollection( _ns );
        }
        
        const char * ns(){ return _ns.c_str(); }
        
        string _ns;
        DBDirectClient db;
    };
        

    class DropIndex : public Base {
    public:
        DropIndex() : Base( "dropindex" ){}
        void run(){
            
            db.insert( ns() , BSON( "x" << 2 ) );
            ASSERT_EQUALS( 1 , db.getIndexes( ns() )->itcount() );
            
            db.ensureIndex( ns() , BSON( "x" << 1 ) );
            ASSERT_EQUALS( 2 , db.getIndexes( ns() )->itcount() );
            
            db.dropIndex( ns() , BSON( "x" << 1 ) );
            ASSERT_EQUALS( 1 , db.getIndexes( ns() )->itcount() );
            
            db.ensureIndex( ns() , BSON( "x" << 1 ) );
            ASSERT_EQUALS( 2 , db.getIndexes( ns() )->itcount() );

            db.dropIndexes( ns() );
            ASSERT_EQUALS( 1 , db.getIndexes( ns() )->itcount() );
        }
    };
    
    class ReIndex : public Base {
    public:
        ReIndex() : Base( "reindex" ){}
        void run(){
            
            db.insert( ns() , BSON( "x" << 2 ) );
            ASSERT_EQUALS( 1 , db.getIndexes( ns() )->itcount() );
            
            db.ensureIndex( ns() , BSON( "x" << 1 ) );
            ASSERT_EQUALS( 2 , db.getIndexes( ns() )->itcount() );
            
            db.reIndex( ns() );
            ASSERT_EQUALS( 2 , db.getIndexes( ns() )->itcount() );
        }

    };

    class ReIndex2 : public Base {
    public:
        ReIndex2() : Base( "reindex2" ){}
        void run(){
            
            db.insert( ns() , BSON( "x" << 2 ) );
            ASSERT_EQUALS( 1 , db.getIndexes( ns() )->itcount() );
            
            db.ensureIndex( ns() , BSON( "x" << 1 ) );
            ASSERT_EQUALS( 2 , db.getIndexes( ns() )->itcount() );
            
            BSONObj out;
            ASSERT( db.runCommand( "test" , BSON( "reIndex" << "reindex2" ) , out ) );
            ASSERT_EQUALS( 2 , out["nIndexes"].number() );
            ASSERT_EQUALS( 2 , db.getIndexes( ns() )->itcount() );
        }

    };

    class CS_10 : public Base {
    public:
        CS_10() : Base( "CS_10" ) {}
        void run() {
            string longs( 770, 'c' );
            for( int i = 0; i < 11; ++i )
                db.insert( ns(), BSON( "a" << i << "b" << longs ) );
            db.ensureIndex( ns(), BSON( "a" << 1 << "b" << 1 ) );
            
            auto_ptr< DBClientCursor > c = db.query( ns(), Query().sort( BSON( "a" << 1 << "b" << 1 ) ), 2 );
            ASSERT_EQUALS( 11, c->itcount() );
        }
    };
    

    class All : public Suite {
    public:
        All() : Suite( "client" ){
        }

        void setupTests(){
            add<DropIndex>();
            add<ReIndex>();
            add<ReIndex2>();
            add<CS_10>();
        }
        
    } all;
}
