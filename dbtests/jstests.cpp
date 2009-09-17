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

#include "../db/instance.h"

#include "../stdafx.h"
#include "../scripting/engine.h"

#include "dbtests.h"

namespace mongo {
    bool dbEval(const char *ns, BSONObj& cmd, BSONObjBuilder& result, string& errmsg);
} // namespace mongo

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

            s->invoke( "this.z == 'sara';" , BSONObj() );
            ASSERT_EQUALS( true , s->getBoolean( "return" ) );

            s->invoke( "this.z == 'asara';" , BSONObj() );
            ASSERT_EQUALS( false , s->getBoolean( "return" ) );
            
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

            s->invoke( "x = 5; for( ; x <10; x++){ a = 1; }" , BSONObj() );
            ASSERT_EQUALS( 10 , s->getNumber( "x" ) );
            
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
#ifdef MOZJS
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
#endif
        }
    };

    class ObjectModTests {
    public:
        void run(){
            Scope * s = globalScriptEngine->createScope();
            
            BSONObj o = BSON( "x" << 17 << "y" << "eliot" << "z" << "sara" );
            s->setObject( "blah" , o , true );
            
            s->invoke( "blah.a = 19;" , BSONObj() );
            BSONObj out = s->getObject( "blah" );
            ASSERT( out["a"].eoo() );

            delete s;
        }
    };

    class OtherJSTypes {
    public:
        void run(){
            Scope * s = globalScriptEngine->createScope();
            
            { // date
                BSONObj o;
                { 
                    BSONObjBuilder b;
                    b.appendDate( "d" , 123456789 );
                    o = b.obj();
                }
                s->setObject( "x" , o );
                
                s->invoke( "return x.d.getTime() != 12;" , BSONObj() );
                ASSERT_EQUALS( true, s->getBoolean( "return" ) );
                
                s->invoke( "z = x.d.getTime();" , BSONObj() );
                ASSERT_EQUALS( 123456789 , s->getNumber( "z" ) );
                
                s->invoke( "z = { z : x.d }" , BSONObj() );
                BSONObj out = s->getObject( "z" );
                ASSERT( out["z"].type() == Date );
            }

            { // regex
                BSONObj o;
                { 
                    BSONObjBuilder b;
                    b.appendRegex( "r" , "^a" , "i" );
                    o = b.obj();
                }
                s->setObject( "x" , o );
                
                s->invoke( "z = x.r.test( 'b' );" , BSONObj() );
                ASSERT_EQUALS( false , s->getBoolean( "z" ) );

                s->invoke( "z = x.r.test( 'a' );" , BSONObj() );
                ASSERT_EQUALS( true , s->getBoolean( "z" ) );

                s->invoke( "z = x.r.test( 'ba' );" , BSONObj() );
                ASSERT_EQUALS( false , s->getBoolean( "z" ) );

                s->invoke( "z = { a : x.r };" , BSONObj() );

                BSONObj out = s->getObject("z");
                ASSERT_EQUALS( (string)"^a" , out["a"].regex() );
                ASSERT_EQUALS( (string)"i" , out["a"].regexFlags() );

            }
            
            delete s;
        }
    };

    class SpecialDBTypes {
    public:
        void run(){
            Scope * s = globalScriptEngine->createScope();

            BSONObjBuilder b;
            b.appendTimestamp( "a" , 123456789 );
            b.appendMinKey( "b" );
            b.appendMaxKey( "c" );
            b.appendTimestamp( "d" , 1234000 , 9876 );
            

            {
                BSONObj t = b.done();
                ASSERT_EQUALS( 1234000U , t["d"].timestampTime() );
                ASSERT_EQUALS( 9876U , t["d"].timestampInc() );
            }

            s->setObject( "z" , b.obj() );
            
            assert( s->invoke( "y = { a : z.a , b : z.b , c : z.c , d: z.d }" , BSONObj() ) == 0 );

            BSONObj out = s->getObject( "y" );
            ASSERT_EQUALS( Timestamp , out["a"].type() );
            ASSERT_EQUALS( MinKey , out["b"].type() );
            ASSERT_EQUALS( MaxKey , out["c"].type() );
            ASSERT_EQUALS( Timestamp , out["d"].type() );

            ASSERT_EQUALS( 9876U , out["d"].timestampInc() );
            ASSERT_EQUALS( 1234000U , out["d"].timestampTime() );
            ASSERT_EQUALS( 123456789U , out["a"].date() );


            delete s;
        }
    };
    
    class TypeConservation {
    public:
        void run(){
            Scope * s = globalScriptEngine->createScope();
            
            //  --  A  --
            
            BSONObj o;
            {
                BSONObjBuilder b ;
                b.append( "a" , (int)5 );
                b.append( "b" , 5.6 );
                o = b.obj();
            }
            ASSERT_EQUALS( NumberInt , o["a"].type() );
            ASSERT_EQUALS( NumberDouble , o["b"].type() );
            
            s->setObject( "z" , o );
            s->invoke( "return z" , BSONObj() );
            BSONObj out = s->getObject( "return" );
            ASSERT_EQUALS( 5 , out["a"].number() );
            ASSERT_EQUALS( 5.6 , out["b"].number() );

            ASSERT_EQUALS( NumberDouble , out["b"].type() );
            ASSERT_EQUALS( NumberInt , out["a"].type() );

            //  --  B  --
            
            {
                BSONObjBuilder b ;
                b.append( "a" , (int)5 );
                b.append( "b" , 5.6 );
                o = b.obj();
            }

            s->setObject( "z" , o , false );
            s->invoke( "return z" , BSONObj() );
            out = s->getObject( "return" );
            ASSERT_EQUALS( 5 , out["a"].number() );
            ASSERT_EQUALS( 5.6 , out["b"].number() );

            ASSERT_EQUALS( NumberDouble , out["b"].type() );
            ASSERT_EQUALS( NumberInt , out["a"].type() );

            
            //  -- C --
            
            {
                BSONObjBuilder b ;
                
                {
                    BSONObjBuilder c;
                    c.append( "0" , 5.5 );
                    c.append( "1" , 6 );
                    b.appendArray( "a" , c.obj() );
                }
                
                o = b.obj();
            }
            
            ASSERT_EQUALS( NumberDouble , o["a"].embeddedObjectUserCheck()["0"].type() );
            ASSERT_EQUALS( NumberInt , o["a"].embeddedObjectUserCheck()["1"].type() );
            
            s->setObject( "z" , o , false );
            out = s->getObject( "z" );

            ASSERT_EQUALS( NumberDouble , out["a"].embeddedObjectUserCheck()["0"].type() );
            ASSERT_EQUALS( NumberInt , out["a"].embeddedObjectUserCheck()["1"].type() );
            
            s->invokeSafe( "z.z = 5;" , BSONObj() );
            out = s->getObject( "z" );
            ASSERT_EQUALS( 5 , out["z"].number() );
            ASSERT_EQUALS( NumberDouble , out["a"].embeddedObjectUserCheck()["0"].type() );
            ASSERT_EQUALS( NumberDouble , out["a"].embeddedObjectUserCheck()["1"].type() ); // TODO: this is technically bad, but here to make sure that i understand the behavior

            delete s;
        }
        
    };
    
    class WeirdObjects {
    public:

        BSONObj build( int depth ){
            BSONObjBuilder b;
            b.append( "0" , depth );
            if ( depth > 0 )
                b.appendArray( "1" , build( depth - 1 ) );
            return b.obj();
        }
        
        void run(){
            Scope * s = globalScriptEngine->createScope();

            s->localConnect( "blah" );
            
            for ( int i=5; i<100 ; i += 10 ){
                s->setObject( "a" , build(i) , false );
                s->invokeSafe( "tojson( a )" , BSONObj() );
                
                s->setObject( "a" , build(5) , true );
                s->invokeSafe( "tojson( a )" , BSONObj() );
            }

            delete s;
        }
    };


    void dummy_function_to_force_dbeval_cpp_linking() {
        BSONObj cmd;
        BSONObjBuilder result;
        string errmsg;
        dbEval( "", cmd, result, errmsg);
    }

    DBDirectClient client;
    
    class Utf8Check {
    public:
        Utf8Check() { reset(); }
        ~Utf8Check() { reset(); }
        void run() {
            if( !globalScriptEngine->utf8Ok() ) {
                log() << "warning: utf8 not supported" << endl;
                return;
            }
            string utf8ObjSpec = "{'_id':'\\u0001\\u007f\\u07ff\\uffff'}";
            BSONObj utf8Obj = fromjson( utf8ObjSpec );
            client.insert( ns(), utf8Obj );
            client.eval( "unittest", "v = db.jstests.utf8check.findOne(); db.jstests.utf8check.remove( {} ); db.jstests.utf8check.insert( v );" );
            check( utf8Obj, client.findOne( ns(), BSONObj() ) );
        }
    private:
        void check( const BSONObj &one, const BSONObj &two ) {
            if ( one.woCompare( two ) != 0 ) {
                static string fail = string( "Assertion failure expected " ) + string( one ) + ", got " + string( two );
                FAIL( fail.c_str() );
            }
        }
        void reset() {
            client.dropCollection( ns() );
        }        
        static const char *ns() { return "unittest.jstests.utf8check"; }
    };

    class LongUtf8String {
    public:
        LongUtf8String() { reset(); }
        ~LongUtf8String() { reset(); }
        void run() {
            if( !globalScriptEngine->utf8Ok() )
                return;
            client.eval( "unittest", "db.jstests.longutf8string.save( {_id:'\\uffff\\uffff\\uffff\\uffff'} )" );
        }
    private:
        void reset() {
            client.dropCollection( ns() );
        }        
        static const char *ns() { return "unittest.jstests.longutf8string"; }
    };
    
    class CodeTests {
    public:
        void run(){
            Scope * s = globalScriptEngine->createScope();
            
            {
                BSONObjBuilder b;
                b.append( "a" , 1 );
                b.appendCode( "b" , "function(){ out.b = 11; }" );
                b.appendCodeWScope( "c" , "function(){ out.c = 12; }" , BSONObj() );
                b.appendCodeWScope( "d" , "function(){ out.d = 13 + bleh; }" , BSON( "bleh" << 5 ) );
                s->setObject( "foo" , b.obj() );
            }
            
            s->invokeSafe( "out = {}; out.a = foo.a; foo.b(); foo.c();" , BSONObj() );
            BSONObj out = s->getObject( "out" );
            
            ASSERT_EQUALS( 1 , out["a"].number() );
            ASSERT_EQUALS( 11 , out["b"].number() );
            ASSERT_EQUALS( 12 , out["c"].number() );

            //s->invokeSafe( "foo.d() " , BSONObj() );
            //out = s->getObject( "out" );
            //ASSERT_EQUALS( 18 , out["d"].number() );
            

            delete s;
        }
    };

    class DBRefTest {
    public:
        DBRefTest(){
            _a = "unittest.dbref.a";
            _b = "unittest.dbref.b";
            reset();
        }
        ~DBRefTest(){
            //reset();
        }
        
        void run(){

            client.insert( _a , BSON( "a" << "17" ) );
            
            {
                BSONObj fromA = client.findOne( _a , BSONObj() );
                cout << "Froma : " << fromA << endl;
                BSONObjBuilder b;
                b.append( "b" , 18 );
                b.appendDBRef( "c" , "dbref.a" , fromA["_id"].__oid() );
                client.insert( _b , b.obj() );
            }
            
            assert( client.eval( "unittest" , "x = db.dbref.b.findOne(); assert.eq( 17 , x.c.fetch().a , 'ref working' );" ) );
        }
        
        void reset(){
            client.dropCollection( _a );
            client.dropCollection( _b );
        }
        
        const char * _a;
        const char * _b;
    };
    
    class BinDataType {
    public:
        
        void pp( const char * s , BSONElement e ){
            int len;
            const char * data = e.binData( len );
            cout << s << ":" << e.binDataType() << "\t" << len << endl;
            cout << "\t";
            for ( int i=0; i<len; i++ )
                cout << (int)(data[i]) << " ";
            cout << endl;
        }

        void run(){
            Scope * s = globalScriptEngine->createScope();
            s->localConnect( "asd" );
            const char * foo = "asdasdasdasd";

            
            BSONObj in;
            {
                BSONObjBuilder b;
                b.append( "a" , 7 );
                b.appendBinData( "b" , strlen( foo ) , ByteArray , foo );
                in = b.obj();
                s->setObject( "x" , in );
            }
            
            s->invokeSafe( "myb = x.b; print( myb ); printjson( myb );" , BSONObj() );
            s->invokeSafe( "y = { c : myb };" , BSONObj() );
            
            BSONObj out = s->getObject( "y" );
            ASSERT_EQUALS( BinData , out["c"].type() );
            //blah( "in " , in["b"] );
            //blah( "out" , out["c"] );
            ASSERT_EQUALS( 0 , in["b"].woCompare( out["c"] , false ) );

            delete s;
        }
    };

    class VarTests {
    public:
        void run(){
            Scope * s = globalScriptEngine->createScope();
            
            assert( s->exec( "a = 5;" , "a" , false , true , false ) );
            ASSERT_EQUALS( 5 , s->getNumber("a" ) );

            assert( s->exec( "var b = 6;" , "b" , false , true , false ) );
            ASSERT_EQUALS( 6 , s->getNumber("b" ) );
            cout << "WTF: " << s->getNumber("b" ) << endl;
            delete s;
        }
    };

    class All : public Suite {
    public:
        All() : Suite( "js" ) {
        }
        
        void setupTests(){
            add< Fundamental >();
            add< BasicScope >();
            add< FalseTests >();
            add< SimpleFunctions >();

            add< ObjectMapping >();
            add< ObjectDecoding >();
            add< JSOIDTests >();
            add< ObjectModTests >();
            add< OtherJSTypes >();
            add< SpecialDBTypes >();
            add< TypeConservation >();

            add< WeirdObjects >();
            add< Utf8Check >();
            add< LongUtf8String >();
            add< CodeTests >();
            add< DBRefTest >();
            add< BinDataType >();

            add< VarTests >();
        }
    } myall;
    
} // namespace JavaJSTests

