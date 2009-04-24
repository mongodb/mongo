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
            cout << s->getString( "return" ) << endl;
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
            
            BSONObj o = BSON( "x" << 17 );
            s->setObject( "blah" , o );
            s->invoke( "return z = blah.x;" , BSONObj() );
            assert( 17 == s->getNumber( "return" ) );

            delete s;
        }
    };

    // TODO:
    // setThis
    // init
    
    class All : public UnitTest::Suite {
    public:
        All() {
            add< Fundamental >();
            add< BasicScope >();
            add< FalseTests >();
            add< SimpleFunctions >();
            add< ObjectMapping >();
        }
    };
    
} // namespace JavaJSTests

UnitTest::TestPtr jsTests() {
    return UnitTest::createSuite< JSTests::All >();
}
