// client.cpp

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

    class All : public Suite {
    public:
        All() : Suite( "client" ){
        }

        void setupTests(){
            add<DropIndex>();
            add<ReIndex>();
        }

    } all;
}
