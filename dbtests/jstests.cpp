// javajstests.cpp 
//

/**
 *    Copyright (C) 2009 10gen Inc.
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

#include "../scripting/engine.h"

#include "dbtests.h"

namespace JSTests {

    class Fundamental {
    public:
        void run() {
            // By calling JavaJSImpl() inside run(), we ensure the unit test framework's
            // signal handlers are pre-installed from JNI's perspective.  This allows
            // JNI to catch signals generated within the JVM and forward other signals
            // as appropriate.
            ScriptEngine::setup();
            globalScriptEngine->runTest();
        }
    };
    
    class BasicScope {
    public:
        void run(){
            Scope * s = globalScriptEngine->createScope();
            
            s->setNumber( "x" , 5 );
            assert( 5 == s->getNumber( "x" ) );

            s->setNumber( "x" , 1.67 );
            assert( 1.67 == s->getNumber( "x" ) );

            s->setString( "s" , "eliot was here" );
            assert( "eliot was here" == s->getString( "s" ) );
            
            s->setBoolean( "b" , true );
            assert( s->getBoolean( "b" ) );

            if ( 0 ){
                s->setBoolean( "b" , false );
                assert( ! s->getBoolean( "b" ) );
            }

            delete s;
        }
    };

    class FalseTests {
    public:
        void run(){
            Scope * s = globalScriptEngine->createScope();

            assert( ! s->getBoolean( "x" ) );
            
            s->setString( "z" , "" );
            assert( ! s->getBoolean( "z" ) );
            
            
            delete s ;
        }
    };

    class SimpleFunctions {
    public:
        void run(){
            Scope * s = globalScriptEngine->createScope();

            s->invoke( "x=5;" , BSONObj() );
            assert( 5 == s->getNumber( "x" ) );
            
            s->invoke( "return 17;" , BSONObj() );
            assert( 17 == s->getNumber( "return" ) );
            
            s->invoke( "function(){ return 17; }" , BSONObj() );
            assert( 17 == s->getNumber( "return" ) );
            
            s->setNumber( "x" , 1.76 );
            s->invoke( "return x == 1.76; " , BSONObj() );
            assert( s->getBoolean( "return" ) );

            s->setNumber( "x" , 1.76 );
            s->invoke( "return x == 1.79; " , BSONObj() );
            assert( ! s->getBoolean( "return" ) );

            delete s;
        }
    };

    class ObjectMapping {
    public:
        void run(){
            Scope * s = globalScriptEngine->createScope();
            
            BSONObj o = BSON( "x" << 17 << "y" << "eliot" << "z" << "sara" );
            s->setObject( "blah" , o );

            s->invoke( "return blah.x;" , BSONObj() );
            ASSERT_EQUALS( 17 , s->getNumber( "return" ) );
            s->invoke( "return blah.y;" , BSONObj() );
            ASSERT_EQUALS( "eliot" , s->getString( "return" ) );

            s->setThis( & o );
            s->invoke( "return this.z;" , BSONObj() );
            ASSERT_EQUALS( "sara" , s->getString( "return" ) );
            
            s->invoke( "return this.x == 17;" , BSONObj() );
            ASSERT_EQUALS( true , s->getBoolean( "return" ) );

            s->invoke( "return this.x == 18;" , BSONObj() );
            ASSERT_EQUALS( false , s->getBoolean( "return" ) );

            s->invoke( "function(){ return this.x == 17; }" , BSONObj() );
            ASSERT_EQUALS( true , s->getBoolean( "return" ) );

            s->invoke( "function(){ return this.x == 18; }" , BSONObj() );
            ASSERT_EQUALS( false , s->getBoolean( "return" ) );

            s->invoke( "function (){ return this.x == 17; }" , BSONObj() );
            ASSERT_EQUALS( true , s->getBoolean( "return" ) );

            s->invoke( "function z(){ return this.x == 18; }" , BSONObj() );
            ASSERT_EQUALS( false , s->getBoolean( "return" ) );
            
            delete s;
        }
    };

    class ObjectDecoding {
    public:
        void run(){
            Scope * s = globalScriptEngine->createScope();
            
            s->invoke( "z = { num : 1 };" , BSONObj() );
            BSONObj out = s->getObject( "z" );
            ASSERT_EQUALS( 1 , out["num"].number() );
            ASSERT_EQUALS( 1 , out.nFields() );

            s->invoke( "z = { x : 'eliot' };" , BSONObj() );
            out = s->getObject( "z" );
            ASSERT_EQUALS( (string)"eliot" , out["x"].valuestr() );
            ASSERT_EQUALS( 1 , out.nFields() );
                           
            BSONObj o = BSON( "x" << 17 );
            s->setObject( "blah" , o );   
            out = s->getObject( "blah" );
            ASSERT_EQUALS( 17 , out["x"].number() );
            
            delete s;
        }
    };
    
    class JSOIDTests {
    public:
        void run(){
            Scope * s = globalScriptEngine->createScope();
            
            s->localConnect( "blah" );
            
            s->invoke( "z = { _id : new ObjectId() , a : 123 };" , BSONObj() );
            BSONObj out = s->getObject( "z" );
            ASSERT_EQUALS( 123 , out["a"].number() );
            ASSERT_EQUALS( jstOID , out["_id"].type() );
            
            OID save = out["_id"].__oid();
            
            s->setObject( "a" , out );
            
            s->invoke( "y = { _id : a._id , a : 124 };" , BSONObj() );            
            out = s->getObject( "y" );
            ASSERT_EQUALS( 124 , out["a"].number() );
            ASSERT_EQUALS( jstOID , out["_id"].type() );            
            ASSERT_EQUALS( out["_id"].__oid().str() , save.str() );

            s->invoke( "y = { _id : new ObjectId( a._id ) , a : 125 };" , BSONObj() );            
            out = s->getObject( "y" );
            ASSERT_EQUALS( 125 , out["a"].number() );
            ASSERT_EQUALS( jstOID , out["_id"].type() );            
            ASSERT_EQUALS( out["_id"].__oid().str() , save.str() );


            delete s;
        }
    };

    class All : public UnitTest::Suite {
    public:
        All() {
            add< Fundamental >();
            add< BasicScope >();
            add< FalseTests >();
            add< SimpleFunctions >();
            add< ObjectMapping >();
            add< ObjectDecoding >();
            add< JSOIDTests >();
        }
    };
    
} // namespace JavaJSTests

UnitTest::TestPtr jsTests() {
    return UnitTest::createSuite< JSTests::All >();
}
