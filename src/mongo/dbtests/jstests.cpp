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

#include "pch.h"
#include "../db/instance.h"
#include "mongo/db/json.h"

#include "../pch.h"
#include "../scripting/engine.h"
#include "../util/timer.h"

#include "dbtests.h"

namespace mongo {
    bool dbEval(const string& dbName , BSONObj& cmd, BSONObjBuilder& result, string& errmsg);
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
        void run() {
            auto_ptr<Scope> s;
            s.reset( globalScriptEngine->newScope() );

            s->setNumber( "x" , 5 );
            ASSERT( 5 == s->getNumber( "x" ) );

            s->setNumber( "x" , 1.67 );
            ASSERT( 1.67 == s->getNumber( "x" ) );

            s->setString( "s" , "eliot was here" );
            ASSERT( "eliot was here" == s->getString( "s" ) );

            s->setBoolean( "b" , true );
            ASSERT( s->getBoolean( "b" ) );

            if ( 0 ) {
                s->setBoolean( "b" , false );
                ASSERT( ! s->getBoolean( "b" ) );
            }
        }
    };

    class ResetScope {
    public:
        void run() {
            // Not worrying about this for now SERVER-446.
            /*
            auto_ptr<Scope> s;
            s.reset( globalScriptEngine->newScope() );

            s->setBoolean( "x" , true );
            ASSERT( s->getBoolean( "x" ) );

            s->reset();
            ASSERT( !s->getBoolean( "x" ) );
            */
        }
    };

    class FalseTests {
    public:
        void run() {
            Scope * s = globalScriptEngine->newScope();

            ASSERT( ! s->getBoolean( "x" ) );

            s->setString( "z" , "" );
            ASSERT( ! s->getBoolean( "z" ) );


            delete s ;
        }
    };

    class SimpleFunctions {
    public:
        void run() {
            Scope * s = globalScriptEngine->newScope();

            s->invoke( "x=5;" , 0, 0 );
            ASSERT( 5 == s->getNumber( "x" ) );

            s->invoke( "return 17;" , 0, 0 );
            ASSERT( 17 == s->getNumber( "return" ) );

            s->invoke( "function(){ return 17; }" , 0, 0 );
            ASSERT( 17 == s->getNumber( "return" ) );

            s->setNumber( "x" , 1.76 );
            s->invoke( "return x == 1.76; " , 0, 0 );
            ASSERT( s->getBoolean( "return" ) );

            s->setNumber( "x" , 1.76 );
            s->invoke( "return x == 1.79; " , 0, 0 );
            ASSERT( ! s->getBoolean( "return" ) );

            BSONObj obj = BSON( "" << 11.0 );
            s->invoke( "function( z ){ return 5 + z; }" , &obj, 0 );
            ASSERT_EQUALS( 16 , s->getNumber( "return" ) );

            delete s;
        }
    };

    /** Installs a tee for auditing log messages, including those logged with tlog(). */
    class LogRecordingScope {
    public:
        LogRecordingScope() :
            _oldTLogLevel( tlogLevel ) {
            tlogLevel = 0;
            Logstream::get().addGlobalTee( &_tee );
        }
        ~LogRecordingScope() {
            Logstream::get().removeGlobalTee( &_tee );
            tlogLevel = _oldTLogLevel;
        }
        /** @return most recent log entry. */
        bool logged() const { return _tee.logged(); }
    private:
        class Tee : public mongo::Tee {
        public:
            Tee() :
                _logged() {
            }
            virtual void write( LogLevel level, const string &str ) { _logged = true; }
            bool logged() const { return _logged; }
        private:
            bool _logged;
        };
        int _oldTLogLevel;
        Tee _tee;
    };
    
    /** Error logging in Scope::exec(). */
    class ExecLogError {
    public:
        void run() {
            Scope *scope = globalScriptEngine->newScope();

            // No error is logged when reportError == false.
            ASSERT( !scope->exec( "notAFunction()", "foo", false, false, false ) );
            ASSERT( !_logger.logged() );
            
            // No error is logged for a valid statement.
            ASSERT( scope->exec( "validStatement = true", "foo", false, true, false ) );
            ASSERT( !_logger.logged() );

            // An error is logged for an invalid statement when reportError == true.
            ASSERT( !scope->exec( "notAFunction()", "foo", false, true, false ) );
            ASSERT( _logger.logged() );
        }
    private:
        LogRecordingScope _logger;
    };
    
    /** Error logging in Scope::invoke(). */
    class InvokeLogError {
    public:
        void run() {
            Scope *scope = globalScriptEngine->newScope();

            // No error is logged for a valid statement.
            ASSERT_EQUALS( 0, scope->invoke( "validStatement = true", 0, 0 ) );
            ASSERT( !_logger.logged() );

            // An error is logged for an invalid statement.
            ASSERT_NOT_EQUALS( 0, scope->invoke( "notAFunction()", 0, 0 ) );
            ASSERT( _logger.logged() );
        }
    private:
        LogRecordingScope _logger;
    };

    class ObjectMapping {
    public:
        void run() {
            Scope * s = globalScriptEngine->newScope();

            BSONObj o = BSON( "x" << 17.0 << "y" << "eliot" << "z" << "sara" );
            s->setObject( "blah" , o );

            s->invoke( "return blah.x;" , 0, 0 );
            ASSERT_EQUALS( 17 , s->getNumber( "return" ) );
            s->invoke( "return blah.y;" , 0, 0 );
            ASSERT_EQUALS( "eliot" , s->getString( "return" ) );

            s->invoke( "return this.z;" , 0, &o );
            ASSERT_EQUALS( "sara" , s->getString( "return" ) );

            s->invoke( "return this.z == 'sara';" , 0, &o );
            ASSERT_EQUALS( true , s->getBoolean( "return" ) );

            s->invoke( "this.z == 'sara';" , 0, &o );
            ASSERT_EQUALS( true , s->getBoolean( "return" ) );

            s->invoke( "this.z == 'asara';" , 0, &o );
            ASSERT_EQUALS( false , s->getBoolean( "return" ) );

            s->invoke( "return this.x == 17;" , 0, &o );
            ASSERT_EQUALS( true , s->getBoolean( "return" ) );

            s->invoke( "return this.x == 18;" , 0, &o );
            ASSERT_EQUALS( false , s->getBoolean( "return" ) );

            s->invoke( "function(){ return this.x == 17; }" , 0, &o );
            ASSERT_EQUALS( true , s->getBoolean( "return" ) );

            s->invoke( "function(){ return this.x == 18; }" , 0, &o );
            ASSERT_EQUALS( false , s->getBoolean( "return" ) );

            s->invoke( "function (){ return this.x == 17; }" , 0, &o );
            ASSERT_EQUALS( true , s->getBoolean( "return" ) );

            s->invoke( "function z(){ return this.x == 18; }" , 0, &o );
            ASSERT_EQUALS( false , s->getBoolean( "return" ) );

            s->invoke( "function (){ this.x == 17; }" , 0, &o );
            ASSERT_EQUALS( false , s->getBoolean( "return" ) );

            s->invoke( "function z(){ this.x == 18; }" , 0, &o );
            ASSERT_EQUALS( false , s->getBoolean( "return" ) );

            s->invoke( "x = 5; for( ; x <10; x++){ a = 1; }" , 0, &o );
            ASSERT_EQUALS( 10 , s->getNumber( "x" ) );

            delete s;
        }
    };

    class ObjectDecoding {
    public:
        void run() {
            Scope * s = globalScriptEngine->newScope();

            s->invoke( "z = { num : 1 };" , 0, 0 );
            BSONObj out = s->getObject( "z" );
            ASSERT_EQUALS( 1 , out["num"].number() );
            ASSERT_EQUALS( 1 , out.nFields() );

            s->invoke( "z = { x : 'eliot' };" , 0, 0 );
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
        void run() {
#ifdef MOZJS
            Scope * s = globalScriptEngine->newScope();

            s->localConnect( "blah" );

            s->invoke( "z = { _id : new ObjectId() , a : 123 };" , 0, 0 );
            BSONObj out = s->getObject( "z" );
            ASSERT_EQUALS( 123 , out["a"].number() );
            ASSERT_EQUALS( jstOID , out["_id"].type() );

            OID save = out["_id"].__oid();

            s->setObject( "a" , out );

            s->invoke( "y = { _id : a._id , a : 124 };" , 0, 0 );
            out = s->getObject( "y" );
            ASSERT_EQUALS( 124 , out["a"].number() );
            ASSERT_EQUALS( jstOID , out["_id"].type() );
            ASSERT_EQUALS( out["_id"].__oid().str() , save.str() );

            s->invoke( "y = { _id : new ObjectId( a._id ) , a : 125 };" , 0, 0 );
            out = s->getObject( "y" );
            ASSERT_EQUALS( 125 , out["a"].number() );
            ASSERT_EQUALS( jstOID , out["_id"].type() );
            ASSERT_EQUALS( out["_id"].__oid().str() , save.str() );

            delete s;
#endif
        }
    };

    class SetImplicit {
    public:
        void run() {
            Scope *s = globalScriptEngine->newScope();

            BSONObj o = BSON( "foo" << "bar" );
            s->setObject( "a.b", o );
            ASSERT( s->getObject( "a" ).isEmpty() );

            BSONObj o2 = BSONObj();
            s->setObject( "a", o2 );
            s->setObject( "a.b", o );
            ASSERT( s->getObject( "a" ).isEmpty() );

            o2 = fromjson( "{b:{}}" );
            s->setObject( "a", o2 );
            s->setObject( "a.b", o );
            ASSERT( !s->getObject( "a" ).isEmpty() );
        }
    };

    class ObjectModReadonlyTests {
    public:
        void run() {
            Scope * s = globalScriptEngine->newScope();

            BSONObj o = BSON( "x" << 17 << "y" << "eliot" << "z" << "sara" << "zz" << BSONObj() );
            s->setObject( "blah" , o , true );

            s->invoke( "blah.y = 'e'", 0, 0 );
            BSONObj out = s->getObject( "blah" );
            ASSERT( strlen( out["y"].valuestr() ) > 1 );

            s->invoke( "blah.a = 19;" , 0, 0 );
            out = s->getObject( "blah" );
            ASSERT( out["a"].eoo() );

            s->invoke( "blah.zz.a = 19;" , 0, 0 );
            out = s->getObject( "blah" );
            ASSERT( out["zz"].embeddedObject()["a"].eoo() );

            s->setObject( "blah.zz", BSON( "a" << 19 ) );
            out = s->getObject( "blah" );
            ASSERT( out["zz"].embeddedObject()["a"].eoo() );

            s->invoke( "delete blah['x']" , 0, 0 );
            out = s->getObject( "blah" );
            ASSERT( !out["x"].eoo() );

            // read-only object itself can be overwritten
            s->invoke( "blah = {}", 0, 0 );
            out = s->getObject( "blah" );
            ASSERT( out.isEmpty() );

            // test array - can't implement this in v8
//            o = fromjson( "{a:[1,2,3]}" );
//            s->setObject( "blah", o, true );
//            out = s->getObject( "blah" );
//            s->invoke( "blah.a[ 0 ] = 4;", BSONObj() );
//            s->invoke( "delete blah['a'][ 2 ];", BSONObj() );
//            out = s->getObject( "blah" );
//            ASSERT_EQUALS( 1.0, out[ "a" ].embeddedObject()[ 0 ].number() );
//            ASSERT_EQUALS( 3.0, out[ "a" ].embeddedObject()[ 2 ].number() );

            delete s;
        }
    };

    class OtherJSTypes {
    public:
        void run() {
            Scope * s = globalScriptEngine->newScope();

            {
                // date
                BSONObj o;
                {
                    BSONObjBuilder b;
                    b.appendDate( "d" , 123456789 );
                    o = b.obj();
                }
                s->setObject( "x" , o );

                s->invoke( "return x.d.getTime() != 12;" , 0, 0 );
                ASSERT_EQUALS( true, s->getBoolean( "return" ) );

                s->invoke( "z = x.d.getTime();" , 0, 0 );
                ASSERT_EQUALS( 123456789 , s->getNumber( "z" ) );

                s->invoke( "z = { z : x.d }" , 0, 0 );
                BSONObj out = s->getObject( "z" );
                ASSERT( out["z"].type() == Date );
            }

            {
                // regex
                BSONObj o;
                {
                    BSONObjBuilder b;
                    b.appendRegex( "r" , "^a" , "i" );
                    o = b.obj();
                }
                s->setObject( "x" , o );

                s->invoke( "z = x.r.test( 'b' );" , 0, 0 );
                ASSERT_EQUALS( false , s->getBoolean( "z" ) );

                s->invoke( "z = x.r.test( 'a' );" , 0, 0 );
                ASSERT_EQUALS( true , s->getBoolean( "z" ) );

                s->invoke( "z = x.r.test( 'ba' );" , 0, 0 );
                ASSERT_EQUALS( false , s->getBoolean( "z" ) );

                s->invoke( "z = { a : x.r };" , 0, 0 );

                BSONObj out = s->getObject("z");
                ASSERT_EQUALS( (string)"^a" , out["a"].regex() );
                ASSERT_EQUALS( (string)"i" , out["a"].regexFlags() );

            }

            // array
            {
                BSONObj o = fromjson( "{r:[1,2,3]}" );
                s->setObject( "x", o, false );
                BSONObj out = s->getObject( "x" );
                ASSERT_EQUALS( Array, out.firstElement().type() );

                s->setObject( "x", o, true );
                out = s->getObject( "x" );
                ASSERT_EQUALS( Array, out.firstElement().type() );
            }

            delete s;
        }
    };

    class SpecialDBTypes {
    public:
        void run() {
            Scope * s = globalScriptEngine->newScope();

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

            ASSERT( s->invoke( "y = { a : z.a , b : z.b , c : z.c , d: z.d }" , 0, 0 ) == 0 );

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
        void run() {
            Scope * s = globalScriptEngine->newScope();

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
            s->invoke( "return z" , 0, 0 );
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
            s->invoke( "return z" , 0, 0 );
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

            s->invokeSafe( "z.z = 5;" , 0, 0 );
            out = s->getObject( "z" );
            ASSERT_EQUALS( 5 , out["z"].number() );
            ASSERT_EQUALS( NumberDouble , out["a"].embeddedObjectUserCheck()["0"].type() );
            // Commenting so that v8 tests will work
//            ASSERT_EQUALS( NumberDouble , out["a"].embeddedObjectUserCheck()["1"].type() ); // TODO: this is technically bad, but here to make sure that i understand the behavior


            // Eliot says I don't have to worry about this case

//            // -- D --
//
//            o = fromjson( "{a:3.0,b:4.5}" );
//            ASSERT_EQUALS( NumberDouble , o["a"].type() );
//            ASSERT_EQUALS( NumberDouble , o["b"].type() );
//
//            s->setObject( "z" , o , false );
//            s->invoke( "return z" , BSONObj() );
//            out = s->getObject( "return" );
//            ASSERT_EQUALS( 3 , out["a"].number() );
//            ASSERT_EQUALS( 4.5 , out["b"].number() );
//
//            ASSERT_EQUALS( NumberDouble , out["b"].type() );
//            ASSERT_EQUALS( NumberDouble , out["a"].type() );
//

            delete s;
        }

    };

    class NumberLong {
    public:
        void run() {
            auto_ptr<Scope> s( globalScriptEngine->newScope() );
            s->localConnect( "blah" );
            BSONObjBuilder b;
            long long val = (long long)( 0xbabadeadbeefbaddULL );
            b.append( "a", val );
            BSONObj in = b.obj();
            s->setObject( "a", in );
            BSONObj out = s->getObject( "a" );
            ASSERT_EQUALS( mongo::NumberLong, out.firstElement().type() );

            ASSERT( s->exec( "b = {b:a.a}", "foo", false, true, false ) );
            out = s->getObject( "b" );
            ASSERT_EQUALS( mongo::NumberLong, out.firstElement().type() );
            if( val != out.firstElement().numberLong() ) {
                cout << val << endl;
                cout << out.firstElement().numberLong() << endl;
                cout << out.toString() << endl;
                ASSERT_EQUALS( val, out.firstElement().numberLong() );
            }

            ASSERT( s->exec( "c = {c:a.a.toString()}", "foo", false, true, false ) );
            out = s->getObject( "c" );
            stringstream ss;
            ss << "NumberLong(\"" << val << "\")";
            ASSERT_EQUALS( ss.str(), out.firstElement().valuestr() );

            ASSERT( s->exec( "d = {d:a.a.toNumber()}", "foo", false, true, false ) );
            out = s->getObject( "d" );
            ASSERT_EQUALS( NumberDouble, out.firstElement().type() );
            ASSERT_EQUALS( double( val ), out.firstElement().number() );

            ASSERT( s->exec( "e = {e:a.a.floatApprox}", "foo", false, true, false ) );
            out = s->getObject( "e" );
            ASSERT_EQUALS( NumberDouble, out.firstElement().type() );
            ASSERT_EQUALS( double( val ), out.firstElement().number() );

            ASSERT( s->exec( "f = {f:a.a.top}", "foo", false, true, false ) );
            out = s->getObject( "f" );
            ASSERT( NumberDouble == out.firstElement().type() || NumberInt == out.firstElement().type() );

            s->setObject( "z", BSON( "z" << (long long)( 4 ) ) );
            ASSERT( s->exec( "y = {y:z.z.top}", "foo", false, true, false ) );
            out = s->getObject( "y" );
            ASSERT_EQUALS( Undefined, out.firstElement().type() );

            ASSERT( s->exec( "x = {x:z.z.floatApprox}", "foo", false, true, false ) );
            out = s->getObject( "x" );
            ASSERT( NumberDouble == out.firstElement().type() || NumberInt == out.firstElement().type() );
            ASSERT_EQUALS( double( 4 ), out.firstElement().number() );

            ASSERT( s->exec( "w = {w:z.z}", "foo", false, true, false ) );
            out = s->getObject( "w" );
            ASSERT_EQUALS( mongo::NumberLong, out.firstElement().type() );
            ASSERT_EQUALS( 4, out.firstElement().numberLong() );

        }
    };

    class NumberLong2 {
    public:
        void run() {
            auto_ptr<Scope> s( globalScriptEngine->newScope() );
            s->localConnect( "blah" );

            BSONObj in;
            {
                BSONObjBuilder b;
                b.append( "a" , 5 );
                b.append( "b" , (long long)5 );
                b.append( "c" , (long long)pow( 2.0, 29 ) );
                b.append( "d" , (long long)pow( 2.0, 30 ) );
                b.append( "e" , (long long)pow( 2.0, 31 ) );
                b.append( "f" , (long long)pow( 2.0, 45 ) );
                in = b.obj();
            }
            s->setObject( "a" , in );

            ASSERT( s->exec( "x = tojson( a ); " ,"foo" , false , true , false ) );
            string outString = s->getString( "x" );

            ASSERT( s->exec( (string)"y = " + outString , "foo2" , false , true , false ) );
            BSONObj out = s->getObject( "y" );
            ASSERT_EQUALS( in , out );
        }
    };

    class NumberLongUnderLimit {
    public:
        void run() {
            auto_ptr<Scope> s( globalScriptEngine->newScope() );
            s->localConnect( "blah" );
            BSONObjBuilder b;
            // limit is 2^53
            long long val = (long long)( 9007199254740991ULL );
            b.append( "a", val );
            BSONObj in = b.obj();
            s->setObject( "a", in );
            BSONObj out = s->getObject( "a" );
            ASSERT_EQUALS( mongo::NumberLong, out.firstElement().type() );

            ASSERT( s->exec( "b = {b:a.a}", "foo", false, true, false ) );
            out = s->getObject( "b" );
            ASSERT_EQUALS( mongo::NumberLong, out.firstElement().type() );
            if( val != out.firstElement().numberLong() ) {
                cout << val << endl;
                cout << out.firstElement().numberLong() << endl;
                cout << out.toString() << endl;
                ASSERT_EQUALS( val, out.firstElement().numberLong() );
            }

            ASSERT( s->exec( "c = {c:a.a.toString()}", "foo", false, true, false ) );
            out = s->getObject( "c" );
            stringstream ss;
            ss << "NumberLong(\"" << val << "\")";
            ASSERT_EQUALS( ss.str(), out.firstElement().valuestr() );

            ASSERT( s->exec( "d = {d:a.a.toNumber()}", "foo", false, true, false ) );
            out = s->getObject( "d" );
            ASSERT_EQUALS( NumberDouble, out.firstElement().type() );
            ASSERT_EQUALS( double( val ), out.firstElement().number() );

            ASSERT( s->exec( "e = {e:a.a.floatApprox}", "foo", false, true, false ) );
            out = s->getObject( "e" );
            ASSERT_EQUALS( NumberDouble, out.firstElement().type() );
            ASSERT_EQUALS( double( val ), out.firstElement().number() );

            ASSERT( s->exec( "f = {f:a.a.top}", "foo", false, true, false ) );
            out = s->getObject( "f" );
            ASSERT( Undefined == out.firstElement().type() );
        }
    };

    class WeirdObjects {
    public:

        BSONObj build( int depth ) {
            BSONObjBuilder b;
            b.append( "0" , depth );
            if ( depth > 0 )
                b.appendArray( "1" , build( depth - 1 ) );
            return b.obj();
        }

        void run() {
            Scope * s = globalScriptEngine->newScope();

            s->localConnect( "blah" );

            for ( int i=5; i<100 ; i += 10 ) {
                s->setObject( "a" , build(i) , false );
                s->invokeSafe( "tojson( a )" , 0, 0 );

                s->setObject( "a" , build(5) , true );
                s->invokeSafe( "tojson( a )" , 0, 0 );
            }

            delete s;
        }
    };


    void dummy_function_to_force_dbeval_cpp_linking() {
        BSONObj cmd;
        BSONObjBuilder result;
        string errmsg;
        dbEval( "test", cmd, result, errmsg);
        verify(0);
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
                static string fail = string( "Assertion failure expected " ) + one.toString() + ", got " + two.toString();
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

    class InvalidUTF8Check {
    public:
        void run() {
            if( !globalScriptEngine->utf8Ok() )
                return;

            auto_ptr<Scope> s;
            s.reset( globalScriptEngine->newScope() );

            BSONObj b;
            {
                char crap[5];

                crap[0] = (char) 128;
                crap[1] = 17;
                crap[2] = (char) 128;
                crap[3] = 17;
                crap[4] = 0;

                BSONObjBuilder bb;
                bb.append( "x" , crap );
                b = bb.obj();
            }

            //cout << "ELIOT: " << b.jsonString() << endl;
            // its ok  if this is handled by js, just can't create a c++ exception
            s->invoke( "x=this.x.length;" , 0, &b );
        }
    };

    class CodeTests {
    public:
        void run() {
            Scope * s = globalScriptEngine->newScope();

            {
                BSONObjBuilder b;
                b.append( "a" , 1 );
                b.appendCode( "b" , "function(){ out.b = 11; }" );
                b.appendCodeWScope( "c" , "function(){ out.c = 12; }" , BSONObj() );
                b.appendCodeWScope( "d" , "function(){ out.d = 13 + bleh; }" , BSON( "bleh" << 5 ) );
                s->setObject( "foo" , b.obj() );
            }

            s->invokeSafe( "out = {}; out.a = foo.a; foo.b(); foo.c();" , 0, 0 );
            BSONObj out = s->getObject( "out" );

            ASSERT_EQUALS( 1 , out["a"].number() );
            ASSERT_EQUALS( 11 , out["b"].number() );
            ASSERT_EQUALS( 12 , out["c"].number() );

            // Guess we don't care about this
            //s->invokeSafe( "foo.d() " , BSONObj() );
            //out = s->getObject( "out" );
            //ASSERT_EQUALS( 18 , out["d"].number() );


            delete s;
        }
    };

    class DBRefTest {
    public:
        DBRefTest() {
            _a = "unittest.dbref.a";
            _b = "unittest.dbref.b";
            reset();
        }
        ~DBRefTest() {
            //reset();
        }

        void run() {

            client.insert( _a , BSON( "a" << "17" ) );

            {
                BSONObj fromA = client.findOne( _a , BSONObj() );
                verify( fromA.valid() );
                //cout << "Froma : " << fromA << endl;
                BSONObjBuilder b;
                b.append( "b" , 18 );
                b.appendDBRef( "c" , "dbref.a" , fromA["_id"].__oid() );
                client.insert( _b , b.obj() );
            }

            ASSERT( client.eval( "unittest" , "x = db.dbref.b.findOne(); assert.eq( 17 , x.c.fetch().a , 'ref working' );" ) );

            // BSON DBRef <=> JS DBPointer
            ASSERT( client.eval( "unittest", "x = db.dbref.b.findOne(); db.dbref.b.drop(); x.c = new DBPointer( x.c.ns, x.c.id ); db.dbref.b.insert( x );" ) );
            ASSERT_EQUALS( DBRef, client.findOne( "unittest.dbref.b", "" )[ "c" ].type() );

            // BSON Object <=> JS DBRef
            ASSERT( client.eval( "unittest", "x = db.dbref.b.findOne(); db.dbref.b.drop(); x.c = new DBRef( x.c.ns, x.c.id ); db.dbref.b.insert( x );" ) );
            ASSERT_EQUALS( Object, client.findOne( "unittest.dbref.b", "" )[ "c" ].type() );
            ASSERT_EQUALS( string( "dbref.a" ), client.findOne( "unittest.dbref.b", "" )[ "c" ].embeddedObject().getStringField( "$ref" ) );
        }

        void reset() {
            client.dropCollection( _a );
            client.dropCollection( _b );
        }

        const char * _a;
        const char * _b;
    };

    class InformalDBRef {
    public:
        void run() {
            client.insert( ns(), BSON( "i" << 1 ) );
            BSONObj obj = client.findOne( ns(), BSONObj() );
            client.remove( ns(), BSONObj() );
            client.insert( ns(), BSON( "r" << BSON( "$ref" << "jstests.informaldbref" << "$id" << obj["_id"].__oid() << "foo" << "bar" ) ) );
            obj = client.findOne( ns(), BSONObj() );
            ASSERT_EQUALS( "bar", obj[ "r" ].embeddedObject()[ "foo" ].str() );

            ASSERT( client.eval( "unittest", "x = db.jstests.informaldbref.findOne(); y = { r:x.r }; db.jstests.informaldbref.drop(); y.r[ \"a\" ] = \"b\"; db.jstests.informaldbref.save( y );" ) );
            obj = client.findOne( ns(), BSONObj() );
            ASSERT_EQUALS( "bar", obj[ "r" ].embeddedObject()[ "foo" ].str() );
            ASSERT_EQUALS( "b", obj[ "r" ].embeddedObject()[ "a" ].str() );
        }
    private:
        static const char *ns() { return "unittest.jstests.informaldbref"; }
    };

    class BinDataType {
    public:

        void pp( const char * s , BSONElement e ) {
            int len;
            const char * data = e.binData( len );
            cout << s << ":" << e.binDataType() << "\t" << len << endl;
            cout << "\t";
            for ( int i=0; i<len; i++ )
                cout << (int)(data[i]) << " ";
            cout << endl;
        }

        void run() {
            Scope * s = globalScriptEngine->newScope();
            s->localConnect( "asd" );
            const char * foo = "asdas\0asdasd";
            const char * base64 = "YXNkYXMAYXNkYXNk";

            BSONObj in;
            {
                BSONObjBuilder b;
                b.append( "a" , 7 );
                b.appendBinData( "b" , 12 , BinDataGeneral , foo );
                in = b.obj();
                s->setObject( "x" , in );
            }

            s->invokeSafe( "myb = x.b; print( myb ); printjson( myb );" , 0, 0 );
            s->invokeSafe( "y = { c : myb };" , 0, 0 );

            BSONObj out = s->getObject( "y" );
            ASSERT_EQUALS( BinData , out["c"].type() );
//            pp( "in " , in["b"] );
//            pp( "out" , out["c"] );
            ASSERT_EQUALS( 0 , in["b"].woCompare( out["c"] , false ) );

            // check that BinData js class is utilized
            s->invokeSafe( "q = x.b.toString();", 0, 0 );
            stringstream expected;
            expected << "BinData(" << BinDataGeneral << ",\"" << base64 << "\")";
            ASSERT_EQUALS( expected.str(), s->getString( "q" ) );

            stringstream scriptBuilder;
            scriptBuilder << "z = { c : new BinData( " << BinDataGeneral << ", \"" << base64 << "\" ) };";
            string script = scriptBuilder.str();
            s->invokeSafe( script.c_str(), 0, 0 );
            out = s->getObject( "z" );
//            pp( "out" , out["c"] );
            ASSERT_EQUALS( 0 , in["b"].woCompare( out["c"] , false ) );

            s->invokeSafe( "a = { f: new BinData( 128, \"\" ) };", 0, 0 );
            out = s->getObject( "a" );
            int len = -1;
            out[ "f" ].binData( len );
            ASSERT_EQUALS( 0, len );
            ASSERT_EQUALS( 128, out[ "f" ].binDataType() );

            delete s;
        }
    };

    class VarTests {
    public:
        void run() {
            Scope * s = globalScriptEngine->newScope();

            ASSERT( s->exec( "a = 5;" , "a" , false , true , false ) );
            ASSERT_EQUALS( 5 , s->getNumber("a" ) );

            ASSERT( s->exec( "var b = 6;" , "b" , false , true , false ) );
            ASSERT_EQUALS( 6 , s->getNumber("b" ) );
            delete s;
        }
    };

    class Speed1 {
    public:
        void run() {
            BSONObj start = BSON( "x" << 5.0 );
            BSONObj empty;

            auto_ptr<Scope> s;
            s.reset( globalScriptEngine->newScope() );

            ScriptingFunction f = s->createFunction( "return this.x + 6;" );

            Timer t;
            double n = 0;
            for ( ; n < 100000; n++ ) {
                s->invoke( f , &empty, &start );
                ASSERT_EQUALS( 11 , s->getNumber( "return" ) );
            }
            //cout << "speed1: " << ( n / t.millis() ) << " ops/ms" << endl;
        }
    };

    class ScopeOut {
    public:
        void run() {
            auto_ptr<Scope> s;
            s.reset( globalScriptEngine->newScope() );

            s->invokeSafe( "x = 5;" , 0, 0 );
            {
                BSONObjBuilder b;
                s->append( b , "z" , "x" );
                ASSERT_EQUALS( BSON( "z" << 5 ) , b.obj() );
            }

            s->invokeSafe( "x = function(){ return 17; }" , 0, 0 );
            BSONObj temp;
            {
                BSONObjBuilder b;
                s->append( b , "z" , "x" );
                temp = b.obj();
            }

            s->invokeSafe( "foo = this.z();" , 0, &temp );
            ASSERT_EQUALS( 17 , s->getNumber( "foo" ) );
        }
    };

    class RenameTest {
    public:
        void run() {
            auto_ptr<Scope> s;
            s.reset( globalScriptEngine->newScope() );

            s->setNumber( "x" , 5 );
            ASSERT_EQUALS( 5 , s->getNumber( "x" ) );
            ASSERT_EQUALS( Undefined , s->type( "y" ) );

            s->rename( "x" , "y" );
            ASSERT_EQUALS( 5 , s->getNumber( "y" ) );
            ASSERT_EQUALS( Undefined , s->type( "x" ) );

            s->rename( "y" , "x" );
            ASSERT_EQUALS( 5 , s->getNumber( "x" ) );
            ASSERT_EQUALS( Undefined , s->type( "y" ) );
        }
    };


    class All : public Suite {
    public:
        All() : Suite( "js" ) {
        }

        void setupTests() {
            add< Fundamental >();
            add< BasicScope >();
            add< ResetScope >();
            add< FalseTests >();
            add< SimpleFunctions >();
            add< ExecLogError >();
            add< InvokeLogError >();

            add< ObjectMapping >();
            add< ObjectDecoding >();
            add< JSOIDTests >();
            add< SetImplicit >();
            add< ObjectModReadonlyTests >();
            add< OtherJSTypes >();
            add< SpecialDBTypes >();
            add< TypeConservation >();
            add< NumberLong >();
            add< NumberLong2 >();
            add< RenameTest >();

            add< WeirdObjects >();
            add< CodeTests >();
            add< DBRefTest >();
            add< InformalDBRef >();
            add< BinDataType >();

            add< VarTests >();

            add< Speed1 >();

            add< InvalidUTF8Check >();
            add< Utf8Check >();
            add< LongUtf8String >();

            add< ScopeOut >();
        }
    } myall;

} // namespace JavaJSTests

