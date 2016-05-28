// jstests.cpp
//

/**
 *    Copyright (C) 2009-2014 MongoDB Inc.
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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include <iostream>
#include <limits>

#include "mongo/base/parse_number.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/json.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/platform/decimal128.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"

using std::cout;
using std::endl;
using std::string;
using std::stringstream;
using std::unique_ptr;
using std::vector;

namespace JSTests {

class BuiltinTests {
public:
    void run() {
        // Run any tests included with the scripting engine
        globalScriptEngine->runTest();
    }
};

class BasicScope {
public:
    void run() {
        unique_ptr<Scope> s;
        s.reset(globalScriptEngine->newScope());

        s->setNumber("x", 5);
        ASSERT(5 == s->getNumber("x"));

        s->setNumber("x", 1.67);
        ASSERT(1.67 == s->getNumber("x"));

        s->setString("s", "eliot was here");
        ASSERT("eliot was here" == s->getString("s"));

        s->setBoolean("b", true);
        ASSERT(s->getBoolean("b"));

        s->setBoolean("b", false);
        ASSERT(!s->getBoolean("b"));
    }
};

class ResetScope {
public:
    void run() {
        /* Currently reset does not clear data in v8 or spidermonkey scopes.  See SECURITY-10
        unique_ptr<Scope> s;
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
        // Test falsy javascript values
        unique_ptr<Scope> s;
        s.reset(globalScriptEngine->newScope());

        ASSERT(!s->getBoolean("notSet"));

        s->setString("emptyString", "");
        ASSERT(!s->getBoolean("emptyString"));

        s->setNumber("notANumberVal", std::numeric_limits<double>::quiet_NaN());
        ASSERT(!s->getBoolean("notANumberVal"));

        auto obj = BSONObjBuilder().appendNull("null").obj();
        s->setElement("nullVal", obj.getField("null"), obj);
        ASSERT(!s->getBoolean("nullVal"));

        s->setNumber("zeroVal", 0);
        ASSERT(!s->getBoolean("zeroVal"));
    }
};

class SimpleFunctions {
public:
    void run() {
        unique_ptr<Scope> s(globalScriptEngine->newScope());

        s->invoke("x=5;", 0, 0);
        ASSERT(5 == s->getNumber("x"));

        s->invoke("return 17;", 0, 0);
        ASSERT(17 == s->getNumber("__returnValue"));

        s->invoke("function(){ return 17; }", 0, 0);
        ASSERT(17 == s->getNumber("__returnValue"));

        s->setNumber("x", 1.76);
        s->invoke("return x == 1.76; ", 0, 0);
        ASSERT(s->getBoolean("__returnValue"));

        s->setNumber("x", 1.76);
        s->invoke("return x == 1.79; ", 0, 0);
        ASSERT(!s->getBoolean("__returnValue"));

        BSONObj obj = BSON("" << 11.0);
        s->invoke("function( z ){ return 5 + z; }", &obj, 0);
        ASSERT_EQUALS(16, s->getNumber("__returnValue"));
    }
};

/** Installs a tee for auditing log messages in the same thread. */
class LogRecordingScope {
public:
    LogRecordingScope()
        : _logged(false),
          _threadName(mongo::getThreadName()),
          _handle(mongo::logger::globalLogDomain()->attachAppender(
              mongo::logger::MessageLogDomain::AppenderAutoPtr(new Tee(this)))) {}
    ~LogRecordingScope() {
        mongo::logger::globalLogDomain()->detachAppender(_handle);
    }
    /** @return most recent log entry. */
    bool logged() const {
        return _logged;
    }

private:
    class Tee : public mongo::logger::MessageLogDomain::EventAppender {
    public:
        Tee(LogRecordingScope* scope) : _scope(scope) {}
        virtual ~Tee() {}
        virtual Status append(const logger::MessageEventEphemeral& event) {
            // Don't want to consider logging by background threads.
            if (mongo::getThreadName() == _scope->_threadName) {
                _scope->_logged = true;
            }
            return Status::OK();
        }

    private:
        LogRecordingScope* _scope;
    };
    bool _logged;
    const string _threadName;
    mongo::logger::MessageLogDomain::AppenderHandle _handle;
};

/** Error logging in Scope::exec(). */
class ExecLogError {
public:
    void run() {
        unique_ptr<Scope> scope(globalScriptEngine->newScope());

        // No error is logged when reportError == false.
        ASSERT(!scope->exec("notAFunction()", "foo", false, false, false));
        ASSERT(!_logger.logged());

        // No error is logged for a valid statement.
        ASSERT(scope->exec("validStatement = true", "foo", false, true, false));
        ASSERT(!_logger.logged());

        // An error is logged for an invalid statement when reportError == true.
        ASSERT(!scope->exec("notAFunction()", "foo", false, true, false));

        // Don't check if we're using SpiderMonkey. Our threading model breaks
        // this test
        // TODO: figure out a way to check for SpiderMonkey
        auto ivs = globalScriptEngine->getInterpreterVersionString();
        std::string prefix("MozJS");
        if (ivs.compare(0, prefix.length(), prefix) != 0) {
            ASSERT(_logger.logged());
        }
    }

private:
    LogRecordingScope _logger;
};

/** Error logging in Scope::invoke(). */
class InvokeLogError {
public:
    void run() {
        unique_ptr<Scope> scope(globalScriptEngine->newScope());

        // No error is logged for a valid statement.
        ASSERT_EQUALS(0, scope->invoke("validStatement = true", 0, 0));
        ASSERT(!_logger.logged());

        // An error is logged for an invalid statement.
        try {
            scope->invoke("notAFunction()", 0, 0);
        } catch (const DBException&) {
            // ignore the exception; just test that we logged something
        }

        // Don't check if we're using SpiderMonkey. Our threading model breaks
        // this test
        // TODO: figure out a way to check for SpiderMonkey
        auto ivs = globalScriptEngine->getInterpreterVersionString();
        std::string prefix("MozJS");
        if (ivs.compare(0, prefix.length(), prefix) != 0) {
            ASSERT(_logger.logged());
        }
    }

private:
    LogRecordingScope _logger;
};

class ObjectMapping {
public:
    void run() {
        unique_ptr<Scope> s(globalScriptEngine->newScope());

        BSONObj o = BSON("x" << 17.0 << "y"
                             << "eliot"
                             << "z"
                             << "sara");
        s->setObject("blah", o);

        s->invoke("return blah.x;", 0, 0);
        ASSERT_EQUALS(17, s->getNumber("__returnValue"));
        s->invoke("return blah.y;", 0, 0);
        ASSERT_EQUALS("eliot", s->getString("__returnValue"));

        s->invoke("return this.z;", 0, &o);
        ASSERT_EQUALS("sara", s->getString("__returnValue"));

        s->invoke("return this.z == 'sara';", 0, &o);
        ASSERT_EQUALS(true, s->getBoolean("__returnValue"));

        s->invoke("this.z == 'sara';", 0, &o);
        ASSERT_EQUALS(true, s->getBoolean("__returnValue"));

        s->invoke("this.z == 'asara';", 0, &o);
        ASSERT_EQUALS(false, s->getBoolean("__returnValue"));

        s->invoke("return this.x == 17;", 0, &o);
        ASSERT_EQUALS(true, s->getBoolean("__returnValue"));

        s->invoke("return this.x == 18;", 0, &o);
        ASSERT_EQUALS(false, s->getBoolean("__returnValue"));

        s->invoke("function(){ return this.x == 17; }", 0, &o);
        ASSERT_EQUALS(true, s->getBoolean("__returnValue"));

        s->invoke("function(){ return this.x == 18; }", 0, &o);
        ASSERT_EQUALS(false, s->getBoolean("__returnValue"));

        s->invoke("function (){ return this.x == 17; }", 0, &o);
        ASSERT_EQUALS(true, s->getBoolean("__returnValue"));

        s->invoke("function z(){ return this.x == 18; }", 0, &o);
        ASSERT_EQUALS(false, s->getBoolean("__returnValue"));

        s->invoke("function (){ this.x == 17; }", 0, &o);
        ASSERT_EQUALS(false, s->getBoolean("__returnValue"));

        s->invoke("function z(){ this.x == 18; }", 0, &o);
        ASSERT_EQUALS(false, s->getBoolean("__returnValue"));

        s->invoke("x = 5; for( ; x <10; x++){ a = 1; }", 0, &o);
        ASSERT_EQUALS(10, s->getNumber("x"));
    }
};

class ObjectDecoding {
public:
    void run() {
        unique_ptr<Scope> s(globalScriptEngine->newScope());

        s->invoke("z = { num : 1 };", 0, 0);
        BSONObj out = s->getObject("z");
        ASSERT_EQUALS(1, out["num"].number());
        ASSERT_EQUALS(1, out.nFields());

        s->invoke("z = { x : 'eliot' };", 0, 0);
        out = s->getObject("z");
        ASSERT_EQUALS((string) "eliot", out["x"].valuestr());
        ASSERT_EQUALS(1, out.nFields());

        BSONObj o = BSON("x" << 17);
        s->setObject("blah", o);
        out = s->getObject("blah");
        ASSERT_EQUALS(17, out["x"].number());
    }
};

class JSOIDTests {
public:
    void run() {
#ifdef MOZJS
        unique_ptr<Scope> s(globalScriptEngine->newScope());

        s->localConnect("blah");

        s->invoke("z = { _id : new ObjectId() , a : 123 };", 0, 0);
        BSONObj out = s->getObject("z");
        ASSERT_EQUALS(123, out["a"].number());
        ASSERT_EQUALS(jstOID, out["_id"].type());

        OID save = out["_id"].__oid();

        s->setObject("a", out);

        s->invoke("y = { _id : a._id , a : 124 };", 0, 0);
        out = s->getObject("y");
        ASSERT_EQUALS(124, out["a"].number());
        ASSERT_EQUALS(jstOID, out["_id"].type());
        ASSERT_EQUALS(out["_id"].__oid().str(), save.str());

        s->invoke("y = { _id : new ObjectId( a._id ) , a : 125 };", 0, 0);
        out = s->getObject("y");
        ASSERT_EQUALS(125, out["a"].number());
        ASSERT_EQUALS(jstOID, out["_id"].type());
        ASSERT_EQUALS(out["_id"].__oid().str(), save.str());
#endif
    }
};

class SetImplicit {
public:
    void run() {
        unique_ptr<Scope> s(globalScriptEngine->newScope());

        BSONObj o = BSON("foo"
                         << "bar");
        s->setObject("a.b", o);
        ASSERT(s->getObject("a").isEmpty());

        BSONObj o2 = BSONObj();
        s->setObject("a", o2);
        s->setObject("a.b", o);
        ASSERT(s->getObject("a").isEmpty());

        o2 = fromjson("{b:{}}");
        s->setObject("a", o2);
        s->setObject("a.b", o);
        ASSERT(!s->getObject("a").isEmpty());
    }
};

class ObjectModReadonlyTests {
public:
    void run() {
        unique_ptr<Scope> s(globalScriptEngine->newScope());

        BSONObj o = BSON("x" << 17 << "y"
                             << "eliot"
                             << "z"
                             << "sara"
                             << "zz"
                             << BSONObj());
        s->setObject("blah", o, true);

        BSONObj out;

        ASSERT_THROWS(s->invoke("blah.y = 'e'", 0, 0), mongo::UserException);
        ASSERT_THROWS(s->invoke("blah.a = 19;", 0, 0), mongo::UserException);
        ASSERT_THROWS(s->invoke("blah.zz.a = 19;", 0, 0), mongo::UserException);
        ASSERT_THROWS(s->invoke("blah.zz = { a : 19 };", 0, 0), mongo::UserException);
        ASSERT_THROWS(s->invoke("delete blah['x']", 0, 0), mongo::UserException);

        // read-only object itself can be overwritten
        s->invoke("blah = {}", 0, 0);
        out = s->getObject("blah");
        ASSERT(out.isEmpty());

        // test array - can't implement this in v8
        //            o = fromjson( "{a:[1,2,3]}" );
        //            s->setObject( "blah", o, true );
        //            out = s->getObject( "blah" );
        //            s->invoke( "blah.a[ 0 ] = 4;", BSONObj() );
        //            s->invoke( "delete blah['a'][ 2 ];", BSONObj() );
        //            out = s->getObject( "blah" );
        //            ASSERT_EQUALS( 1.0, out[ "a" ].embeddedObject()[ 0 ].number() );
        //            ASSERT_EQUALS( 3.0, out[ "a" ].embeddedObject()[ 2 ].number() );
    }
};

class OtherJSTypes {
public:
    void run() {
        unique_ptr<Scope> s(globalScriptEngine->newScope());

        {
            // date
            BSONObj o;
            {
                BSONObjBuilder b;
                b.appendDate("d", Date_t::fromMillisSinceEpoch(123456789));
                o = b.obj();
            }
            s->setObject("x", o);

            s->invoke("return x.d.getTime() != 12;", 0, 0);
            ASSERT_EQUALS(true, s->getBoolean("__returnValue"));

            s->invoke("z = x.d.getTime();", 0, 0);
            ASSERT_EQUALS(123456789, s->getNumber("z"));

            s->invoke("z = { z : x.d }", 0, 0);
            BSONObj out = s->getObject("z");
            ASSERT(out["z"].type() == Date);
        }

        {
            // regex
            BSONObj o;
            {
                BSONObjBuilder b;
                b.appendRegex("r", "^a", "i");
                o = b.obj();
            }
            s->setObject("x", o);

            s->invoke("z = x.r.test( 'b' );", 0, 0);
            ASSERT_EQUALS(false, s->getBoolean("z"));

            s->invoke("z = x.r.test( 'a' );", 0, 0);
            ASSERT_EQUALS(true, s->getBoolean("z"));

            s->invoke("z = x.r.test( 'ba' );", 0, 0);
            ASSERT_EQUALS(false, s->getBoolean("z"));

            s->invoke("z = { a : x.r };", 0, 0);

            BSONObj out = s->getObject("z");
            ASSERT_EQUALS((string) "^a", out["a"].regex());
            ASSERT_EQUALS((string) "i", out["a"].regexFlags());

            // This regex used to cause a segfault because x isn't a valid flag for a js RegExp.
            // Now it throws a JS exception.
            BSONObj invalidRegex = BSON_ARRAY(BSON("regex" << BSONRegEx("asdf", "x")));
            const char* code =
                "function (obj) {"
                "    var threw = false;"
                "    try {"
                "        obj.regex;"  // should throw
                "    } catch(e) {"
                "         threw = true;"
                "    }"
                "    assert(threw);"
                "}";
            ASSERT_EQUALS(s->invoke(code, &invalidRegex, NULL), 0);
        }

        // array
        {
            BSONObj o = fromjson("{r:[1,2,3]}");
            s->setObject("x", o, false);
            BSONObj out = s->getObject("x");
            ASSERT_EQUALS(Array, out.firstElement().type());

            s->setObject("x", o, true);
            out = s->getObject("x");
            ASSERT_EQUALS(Array, out.firstElement().type());
        }

        // symbol
        {
            // test mutable object with symbol type
            BSONObjBuilder builder;
            builder.appendSymbol("sym", "value");
            BSONObj in = builder.done();
            s->setObject("x", in, false);
            BSONObj out = s->getObject("x");
            ASSERT_EQUALS(Symbol, out.firstElement().type());

            // readonly
            s->setObject("x", in, true);
            out = s->getObject("x");
            ASSERT_EQUALS(Symbol, out.firstElement().type());
        }
    }
};

class SpecialDBTypes {
public:
    void run() {
        unique_ptr<Scope> s(globalScriptEngine->newScope());

        BSONObjBuilder b;
        b.appendTimestamp("a", 123456789);
        b.appendMinKey("b");
        b.appendMaxKey("c");
        b.append("d", Timestamp(1234, 9876));


        {
            BSONObj t = b.done();
            ASSERT_EQUALS(Date_t::fromMillisSinceEpoch(1234000), t["d"].timestampTime());
            ASSERT_EQUALS(9876U, t["d"].timestampInc());
        }

        s->setObject("z", b.obj());

        ASSERT(s->invoke("y = { a : z.a , b : z.b , c : z.c , d: z.d }", 0, 0) == 0);

        BSONObj out = s->getObject("y");
        ASSERT_EQUALS(bsonTimestamp, out["a"].type());
        ASSERT_EQUALS(MinKey, out["b"].type());
        ASSERT_EQUALS(MaxKey, out["c"].type());
        ASSERT_EQUALS(bsonTimestamp, out["d"].type());

        ASSERT_EQUALS(9876U, out["d"].timestampInc());
        ASSERT_EQUALS(Date_t::fromMillisSinceEpoch(1234000), out["d"].timestampTime());
        ASSERT_EQUALS(Date_t::fromMillisSinceEpoch(123456789), out["a"].date());
    }
};

class TypeConservation {
public:
    void run() {
        unique_ptr<Scope> s(globalScriptEngine->newScope());

        //  --  A  --

        BSONObj o;
        {
            BSONObjBuilder b;
            b.append("a", (int)5);
            b.append("b", 5.6);
            o = b.obj();
        }
        ASSERT_EQUALS(NumberInt, o["a"].type());
        ASSERT_EQUALS(NumberDouble, o["b"].type());

        s->setObject("z", o);
        s->invoke("return z", 0, 0);
        BSONObj out = s->getObject("__returnValue");
        ASSERT_EQUALS(5, out["a"].number());
        ASSERT_EQUALS(5.6, out["b"].number());

        ASSERT_EQUALS(NumberDouble, out["b"].type());
        ASSERT_EQUALS(NumberInt, out["a"].type());

        //  --  B  --

        {
            BSONObjBuilder b;
            b.append("a", (int)5);
            b.append("b", 5.6);
            o = b.obj();
        }

        s->setObject("z", o, false);
        s->invoke("return z", 0, 0);
        out = s->getObject("__returnValue");
        ASSERT_EQUALS(5, out["a"].number());
        ASSERT_EQUALS(5.6, out["b"].number());

        ASSERT_EQUALS(NumberDouble, out["b"].type());
        ASSERT_EQUALS(NumberInt, out["a"].type());


        //  -- C --

        {
            BSONObjBuilder b;

            {
                BSONObjBuilder c;
                c.append("0", 5.5);
                c.append("1", 6);
                b.appendArray("a", c.obj());
            }

            o = b.obj();
        }

        ASSERT_EQUALS(NumberDouble, o["a"].embeddedObjectUserCheck()["0"].type());
        ASSERT_EQUALS(NumberInt, o["a"].embeddedObjectUserCheck()["1"].type());

        s->setObject("z", o, false);
        out = s->getObject("z");

        ASSERT_EQUALS(NumberDouble, out["a"].embeddedObjectUserCheck()["0"].type());
        ASSERT_EQUALS(NumberInt, out["a"].embeddedObjectUserCheck()["1"].type());

        s->invokeSafe("z.z = 5;", 0, 0);
        out = s->getObject("z");
        ASSERT_EQUALS(5, out["z"].number());
        ASSERT_EQUALS(NumberDouble, out["a"].embeddedObjectUserCheck()["0"].type());
        // Commenting so that v8 tests will work
        // TODO: this is technically bad, but here to make sure that i understand the behavior
        // ASSERT_EQUALS( NumberDouble , out["a"].embeddedObjectUserCheck()["1"].type() );


        // Eliot says I don't have to worry about this case

        //            // -- D --
        //
        //            o = fromjson( "{a:3.0,b:4.5}" );
        //            ASSERT_EQUALS( NumberDouble , o["a"].type() );
        //            ASSERT_EQUALS( NumberDouble , o["b"].type() );
        //
        //            s->setObject( "z" , o , false );
        //            s->invoke( "return z" , BSONObj() );
        //            out = s->getObject( "__returnValue" );
        //            ASSERT_EQUALS( 3 , out["a"].number() );
        //            ASSERT_EQUALS( 4.5 , out["b"].number() );
        //
        //            ASSERT_EQUALS( NumberDouble , out["b"].type() );
        //            ASSERT_EQUALS( NumberDouble , out["a"].type() );
        //
    }
};

class NumberLong {
public:
    void run() {
        unique_ptr<Scope> s(globalScriptEngine->newScope());
        BSONObjBuilder b;
        long long val = (long long)(0xbabadeadbeefbaddULL);
        b.append("a", val);
        BSONObj in = b.obj();
        s->setObject("a", in);
        BSONObj out = s->getObject("a");
        ASSERT_EQUALS(mongo::NumberLong, out.firstElement().type());

        ASSERT(s->exec("b = {b:a.a}", "foo", false, true, false));
        out = s->getObject("b");
        ASSERT_EQUALS(mongo::NumberLong, out.firstElement().type());
        if (val != out.firstElement().numberLong()) {
            cout << val << endl;
            cout << out.firstElement().numberLong() << endl;
            cout << out.toString() << endl;
            ASSERT_EQUALS(val, out.firstElement().numberLong());
        }

        ASSERT(s->exec("c = {c:a.a.toString()}", "foo", false, true, false));
        out = s->getObject("c");
        stringstream ss;
        ss << "NumberLong(\"" << val << "\")";
        ASSERT_EQUALS(ss.str(), out.firstElement().valuestr());

        ASSERT(s->exec("d = {d:a.a.toNumber()}", "foo", false, true, false));
        out = s->getObject("d");
        ASSERT_EQUALS(NumberDouble, out.firstElement().type());
        ASSERT_EQUALS(double(val), out.firstElement().number());

        ASSERT(s->exec("e = {e:a.a.floatApprox}", "foo", false, true, false));
        out = s->getObject("e");
        ASSERT_EQUALS(NumberDouble, out.firstElement().type());
        ASSERT_EQUALS(double(val), out.firstElement().number());

        ASSERT(s->exec("f = {f:a.a.top}", "foo", false, true, false));
        out = s->getObject("f");
        ASSERT(NumberDouble == out.firstElement().type() || NumberInt == out.firstElement().type());

        s->setObject("z", BSON("z" << (long long)(4)));
        ASSERT(s->exec("y = {y:z.z.top}", "foo", false, true, false));
        out = s->getObject("y");
        ASSERT_EQUALS(Undefined, out.firstElement().type());

        ASSERT(s->exec("x = {x:z.z.floatApprox}", "foo", false, true, false));
        out = s->getObject("x");
        ASSERT(NumberDouble == out.firstElement().type() || NumberInt == out.firstElement().type());
        ASSERT_EQUALS(double(4), out.firstElement().number());

        ASSERT(s->exec("w = {w:z.z}", "foo", false, true, false));
        out = s->getObject("w");
        ASSERT_EQUALS(mongo::NumberLong, out.firstElement().type());
        ASSERT_EQUALS(4, out.firstElement().numberLong());
    }
};

class NumberLong2 {
public:
    void run() {
        unique_ptr<Scope> s(globalScriptEngine->newScope());

        BSONObj in;
        {
            BSONObjBuilder b;
            b.append("a", 5);
            b.append("b", (long long)5);
            b.append("c", (long long)pow(2.0, 29));
            b.append("d", (long long)pow(2.0, 30));
            b.append("e", (long long)pow(2.0, 31));
            b.append("f", (long long)pow(2.0, 45));
            in = b.obj();
        }
        s->setObject("a", in);

        ASSERT(s->exec("x = tojson( a ); ", "foo", false, true, false));
        string outString = s->getString("x");

        ASSERT(s->exec((string) "y = " + outString, "foo2", false, true, false));
        BSONObj out = s->getObject("y");
        ASSERT_EQUALS(in, out);
    }
};

class NumberLongUnderLimit {
public:
    void run() {
        unique_ptr<Scope> s(globalScriptEngine->newScope());

        BSONObjBuilder b;
        // limit is 2^53
        long long val = (long long)(9007199254740991ULL);
        b.append("a", val);
        BSONObj in = b.obj();
        s->setObject("a", in);
        BSONObj out = s->getObject("a");
        ASSERT_EQUALS(mongo::NumberLong, out.firstElement().type());

        ASSERT(s->exec("b = {b:a.a}", "foo", false, true, false));
        out = s->getObject("b");
        ASSERT_EQUALS(mongo::NumberLong, out.firstElement().type());
        if (val != out.firstElement().numberLong()) {
            cout << val << endl;
            cout << out.firstElement().numberLong() << endl;
            cout << out.toString() << endl;
            ASSERT_EQUALS(val, out.firstElement().numberLong());
        }

        ASSERT(s->exec("c = {c:a.a.toString()}", "foo", false, true, false));
        out = s->getObject("c");
        stringstream ss;
        ss << "NumberLong(\"" << val << "\")";
        ASSERT_EQUALS(ss.str(), out.firstElement().valuestr());

        ASSERT(s->exec("d = {d:a.a.toNumber()}", "foo", false, true, false));
        out = s->getObject("d");
        ASSERT_EQUALS(NumberDouble, out.firstElement().type());
        ASSERT_EQUALS(double(val), out.firstElement().number());

        ASSERT(s->exec("e = {e:a.a.floatApprox}", "foo", false, true, false));
        out = s->getObject("e");
        ASSERT_EQUALS(NumberDouble, out.firstElement().type());
        ASSERT_EQUALS(double(val), out.firstElement().number());

        ASSERT(s->exec("f = {f:a.a.top}", "foo", false, true, false));
        out = s->getObject("f");
        ASSERT(Undefined == out.firstElement().type());
    }
};

class NumberDecimal {
public:
    void run() {
        unique_ptr<Scope> s(globalScriptEngine->newScope());
        BSONObjBuilder b;
        Decimal128 val = Decimal128("2.010");
        b.append("a", val);
        BSONObj in = b.obj();
        s->setObject("a", in);

        // Test the scope object
        BSONObj out = s->getObject("a");
        ASSERT_EQUALS(mongo::NumberDecimal, out.firstElement().type());
        ASSERT_TRUE(val.isEqual(out.firstElement().numberDecimal()));

        ASSERT(s->exec("b = {b:a.a}", "foo", false, true, false));
        out = s->getObject("b");
        ASSERT_EQUALS(mongo::NumberDecimal, out.firstElement().type());
        ASSERT_TRUE(val.isEqual(out.firstElement().numberDecimal()));

        // Test that the appropriate string output is generated
        ASSERT(s->exec("c = {c:a.a.toString()}", "foo", false, true, false));
        out = s->getObject("c");
        stringstream ss;
        ss << "NumberDecimal(\"" << val.toString() << "\")";
        ASSERT_EQUALS(ss.str(), out.firstElement().valuestr());
    }
};

class NumberDecimalGetFromScope {
public:
    void run() {
        unique_ptr<Scope> s(globalScriptEngine->newScope());
        ASSERT(s->exec("a = 5;", "a", false, true, false));
        ASSERT_TRUE(Decimal128(5).isEqual(s->getNumberDecimal("a")));
    }
};

class NumberDecimalBigObject {
public:
    void run() {
        unique_ptr<Scope> s(globalScriptEngine->newScope());

        BSONObj in;
        {
            BSONObjBuilder b;
            b.append("a", 5);
            b.append("b", Decimal128("1.5E-3000"));
            b.append("c", Decimal128("1.5E-1"));
            b.append("d", Decimal128("1.5E3000"));
            b.append("e", Decimal128("Infinity"));
            b.append("f", Decimal128("NaN"));
            in = b.obj();
        }
        s->setObject("a", in);

        ASSERT(s->exec("x = tojson( a ); ", "foo", false, true, false));
        string outString = s->getString("x");

        ASSERT(s->exec((string) "y = " + outString, "foo2", false, true, false));
        BSONObj out = s->getObject("y");
        ASSERT_EQUALS(in, out);
    }
};

class MaxTimestamp {
public:
    void run() {
        unique_ptr<Scope> s(globalScriptEngine->newScope());

        // Timestamp 't' component can exceed max for int32_t.
        BSONObj in;
        {
            BSONObjBuilder b;
            b.bb().appendNum(static_cast<char>(bsonTimestamp));
            b.bb().appendStr("a");
            b.bb().appendNum(std::numeric_limits<unsigned long long>::max());

            in = b.obj();
        }
        s->setObject("a", in);

        ASSERT(s->exec("x = tojson( a ); ", "foo", false, true, false));
    }
};

class WeirdObjects {
public:
    BSONObj build(int depth) {
        BSONObjBuilder b;
        b.append("0", depth);
        if (depth > 0)
            b.appendArray("1", build(depth - 1));
        return b.obj();
    }

    void run() {
        unique_ptr<Scope> s(globalScriptEngine->newScope());

        for (int i = 5; i < 100; i += 10) {
            s->setObject("a", build(i), false);
            s->invokeSafe("tojson( a )", 0, 0);

            s->setObject("a", build(5), true);
            s->invokeSafe("tojson( a )", 0, 0);
        }
    }
};

/**
 * Test exec() timeout value terminates execution (SERVER-8053)
 */
class ExecTimeout {
public:
    void run() {
        unique_ptr<Scope> scope(globalScriptEngine->newScope());

        // assert timeout occurred
        ASSERT(!scope->exec("var a = 1; while (true) { ; }", "ExecTimeout", false, true, false, 1));
    }
};

/**
 * Test exec() timeout value terminates execution (SERVER-8053)
 */
class ExecNoTimeout {
public:
    void run() {
        unique_ptr<Scope> scope(globalScriptEngine->newScope());

        // assert no timeout occurred
        ASSERT(scope->exec("var a = function() { return 1; }",
                           "ExecNoTimeout",
                           false,
                           true,
                           false,
                           5 * 60 * 1000));
    }
};

/**
 * Test invoke() timeout value terminates execution (SERVER-8053)
 */
class InvokeTimeout {
public:
    void run() {
        unique_ptr<Scope> scope(globalScriptEngine->newScope());

        // scope timeout after 500ms
        bool caught = false;
        try {
            scope->invokeSafe(
                "function() {         "
                "    while (true) { } "
                "}                    ",
                0,
                0,
                1);
        } catch (const DBException&) {
            caught = true;
        }
        ASSERT(caught);
    }
};

/**
 * Test invoke() timeout value does not terminate execution (SERVER-8053)
 */
class InvokeNoTimeout {
public:
    void run() {
        unique_ptr<Scope> scope(globalScriptEngine->newScope());

        // invoke completes before timeout
        scope->invokeSafe(
            "function() { "
            "  for (var i=0; i<1; i++) { ; } "
            "} ",
            0,
            0,
            5 * 60 * 1000);
    }
};


class Utf8Check {
public:
    Utf8Check() {
        reset();
    }
    ~Utf8Check() {
        reset();
    }
    void run() {
        if (!globalScriptEngine->utf8Ok()) {
            mongo::unittest::log() << "warning: utf8 not supported" << endl;
            return;
        }
        string utf8ObjSpec = "{'_id':'\\u0001\\u007f\\u07ff\\uffff'}";
        BSONObj utf8Obj = fromjson(utf8ObjSpec);

        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        DBDirectClient client(&txn);

        client.insert(ns(), utf8Obj);
        client.eval("unittest",
                    "v = db.jstests.utf8check.findOne(); db.jstests.utf8check.remove( {} ); "
                    "db.jstests.utf8check.insert( v );");
        check(utf8Obj, client.findOne(ns(), BSONObj()));
    }

private:
    void check(const BSONObj& one, const BSONObj& two) {
        if (one.woCompare(two) != 0) {
            static string fail =
                string("Assertion failure expected ") + one.toString() + ", got " + two.toString();
            FAIL(fail.c_str());
        }
    }

    void reset() {
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        DBDirectClient client(&txn);

        client.dropCollection(ns());
    }

    static const char* ns() {
        return "unittest.jstests.utf8check";
    }
};

class LongUtf8String {
public:
    LongUtf8String() {
        reset();
    }
    ~LongUtf8String() {
        reset();
    }
    void run() {
        if (!globalScriptEngine->utf8Ok())
            return;

        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        DBDirectClient client(&txn);

        client.eval("unittest",
                    "db.jstests.longutf8string.save( {_id:'\\uffff\\uffff\\uffff\\uffff'} )");
    }

private:
    void reset() {
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        DBDirectClient client(&txn);

        client.dropCollection(ns());
    }

    static const char* ns() {
        return "unittest.jstests.longutf8string";
    }
};

class InvalidUTF8Check {
public:
    void run() {
        if (!globalScriptEngine->utf8Ok())
            return;

        unique_ptr<Scope> s;
        s.reset(globalScriptEngine->newScope());

        BSONObj b;
        {
            char crap[5];

            crap[0] = (char)128;
            crap[1] = 17;
            crap[2] = (char)128;
            crap[3] = 17;
            crap[4] = 0;

            BSONObjBuilder bb;
            bb.append("x", crap);
            b = bb.obj();
        }

        // cout << "ELIOT: " << b.jsonString() << endl;
        // its ok  if this is handled by js, just can't create a c++ exception
        s->invoke("x=this.x.length;", 0, &b);
    }
};

class CodeTests {
public:
    void run() {
        unique_ptr<Scope> s(globalScriptEngine->newScope());

        {
            BSONObjBuilder b;
            b.append("a", 1);
            b.appendCode("b", "function(){ out.b = 11; }");
            b.appendCodeWScope("c", "function(){ out.c = 12; }", BSONObj());
            b.appendCodeWScope("d", "function(){ out.d = 13 + bleh; }", BSON("bleh" << 5));
            s->setObject("foo", b.obj());
        }

        s->invokeSafe("out = {}; out.a = foo.a; foo.b(); foo.c();", 0, 0);
        BSONObj out = s->getObject("out");

        ASSERT_EQUALS(1, out["a"].number());
        ASSERT_EQUALS(11, out["b"].number());
        ASSERT_EQUALS(12, out["c"].number());

        // Guess we don't care about this
        // s->invokeSafe( "foo.d() " , BSONObj() );
        // out = s->getObject( "out" );
        // ASSERT_EQUALS( 18 , out["d"].number() );
    }
};

namespace RoundTripTests {

// Inherit from this class to test round tripping of JSON objects
class TestRoundTrip {
public:
    virtual ~TestRoundTrip() {}
    void run() {
        // Insert in Javascript -> Find using DBDirectClient

        // Drop the collection
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        DBDirectClient client(&txn);

        client.dropCollection("unittest.testroundtrip");

        // Insert in Javascript
        stringstream jsInsert;
        jsInsert << "db.testroundtrip.insert(" << jsonIn() << ")";
        ASSERT_TRUE(client.eval("unittest", jsInsert.str()));

        // Find using DBDirectClient
        BSONObj excludeIdProjection = BSON("_id" << 0);
        BSONObj directFind = client.findOne("unittest.testroundtrip", "", &excludeIdProjection);
        bsonEquals(bson(), directFind);


        // Insert using DBDirectClient -> Find in Javascript

        // Drop the collection
        client.dropCollection("unittest.testroundtrip");

        // Insert using DBDirectClient
        client.insert("unittest.testroundtrip", bson());

        // Find in Javascript
        stringstream jsFind;
        jsFind << "dbref = db.testroundtrip.findOne( { } , { _id : 0 } )\n"
               << "assert.eq(dbref, " << jsonOut() << ")";
        ASSERT_TRUE(client.eval("unittest", jsFind.str()));
    }

protected:
    // Methods that must be defined by child classes
    virtual BSONObj bson() const = 0;
    virtual string json() const = 0;

    // This can be overriden if a different meaning of equality besides woCompare is needed
    virtual void bsonEquals(const BSONObj& expected, const BSONObj& actual) {
        if (expected.woCompare(actual)) {
            ::mongo::log() << "want:" << expected.jsonString() << " size: " << expected.objsize()
                           << endl;
            ::mongo::log() << "got :" << actual.jsonString() << " size: " << actual.objsize()
                           << endl;
            ::mongo::log() << expected.hexDump() << endl;
            ::mongo::log() << actual.hexDump() << endl;
        }
        ASSERT(!expected.woCompare(actual));
    }

    // This can be overriden if the JSON representation is altered on the round trip
    virtual string jsonIn() const {
        return json();
    }
    virtual string jsonOut() const {
        return json();
    }
};

class DBRefTest : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        OID o;
        memset(&o, 0, 12);
        BSONObjBuilder subBuilder(b.subobjStart("a"));
        subBuilder.append("$ref", "ns");
        subBuilder.append("$id", o);
        subBuilder.done();
        return b.obj();
    }
    virtual string json() const {
        return "{ \"a\" : DBRef( \"ns\", ObjectId( \"000000000000000000000000\" ) ) }";
    }

    // A "fetch" function is added to the DBRef object when it is inserted using the
    // constructor, so we need to compare the fields individually
    virtual void bsonEquals(const BSONObj& expected, const BSONObj& actual) {
        ASSERT_EQUALS(expected["a"].type(), actual["a"].type());
        ASSERT_EQUALS(expected["a"]["$id"].OID(), actual["a"]["$id"].OID());
        ASSERT_EQUALS(expected["a"]["$ref"].String(), actual["a"]["$ref"].String());
    }
};

class DBPointerTest : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        OID o;
        memset(&o, 0, 12);
        b.appendDBRef("a", "ns", o);
        return b.obj();
    }
    virtual string json() const {
        return "{ \"a\" : DBPointer( \"ns\", ObjectId( \"000000000000000000000000\" ) ) }";
    }
};

class InformalDBRefTest : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        BSONObjBuilder subBuilder(b.subobjStart("a"));
        subBuilder.append("$ref", "ns");
        subBuilder.append("$id", "000000000000000000000000");
        subBuilder.done();
        return b.obj();
    }

    // Don't need to return anything because we are overriding both jsonOut and jsonIn
    virtual string json() const {
        return "";
    }

    // Need to override these because the JSON doesn't actually round trip.
    // An object with "$ref" and "$id" fields is handled specially and different on the way out.
    virtual string jsonOut() const {
        return "{ \"a\" : DBRef( \"ns\", \"000000000000000000000000\" ) }";
    }
    virtual string jsonIn() const {
        stringstream ss;
        ss << "{ \"a\" : { \"$ref\" : \"ns\" , "
           << "\"$id\" : \"000000000000000000000000\" } }";
        return ss.str();
    }
};

class InformalDBRefOIDTest : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        OID o;
        memset(&o, 0, 12);
        BSONObjBuilder subBuilder(b.subobjStart("a"));
        subBuilder.append("$ref", "ns");
        subBuilder.append("$id", o);
        subBuilder.done();
        return b.obj();
    }

    // Don't need to return anything because we are overriding both jsonOut and jsonIn
    virtual string json() const {
        return "";
    }

    // Need to override these because the JSON doesn't actually round trip.
    // An object with "$ref" and "$id" fields is handled specially and different on the way out.
    virtual string jsonOut() const {
        return "{ \"a\" : DBRef( \"ns\", ObjectId( \"000000000000000000000000\" ) ) }";
    }
    virtual string jsonIn() const {
        stringstream ss;
        ss << "{ \"a\" : { \"$ref\" : \"ns\" , "
           << "\"$id\" : ObjectId( \"000000000000000000000000\" ) } }";
        return ss.str();
    }
};

class InformalDBRefExtraFieldTest : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        OID o;
        memset(&o, 0, 12);
        BSONObjBuilder subBuilder(b.subobjStart("a"));
        subBuilder.append("$ref", "ns");
        subBuilder.append("$id", o);
        subBuilder.append("otherfield", "value");
        subBuilder.done();
        return b.obj();
    }

    // Don't need to return anything because we are overriding both jsonOut and jsonIn
    virtual string json() const {
        return "";
    }

    // Need to override these because the JSON doesn't actually round trip.
    // An object with "$ref" and "$id" fields is handled specially and different on the way out.
    virtual string jsonOut() const {
        return "{ \"a\" : DBRef( \"ns\", ObjectId( \"000000000000000000000000\" ) ) }";
    }
    virtual string jsonIn() const {
        stringstream ss;
        ss << "{ \"a\" : { \"$ref\" : \"ns\" , "
           << "\"$id\" : ObjectId( \"000000000000000000000000\" ) , "
           << "\"otherfield\" : \"value\" } }";
        return ss.str();
    }
};

class Empty : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        return b.obj();
    }
    virtual string json() const {
        return "{}";
    }
};

class EmptyWithSpace : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        return b.obj();
    }
    virtual string json() const {
        return "{ }";
    }
};

class SingleString : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        b.append("a", "b");
        return b.obj();
    }
    virtual string json() const {
        return "{ \"a\" : \"b\" }";
    }
};

class EmptyStrings : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        b.append("", "");
        return b.obj();
    }
    virtual string json() const {
        return "{ \"\" : \"\" }";
    }
};

class SingleNumber : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        b.append("a", 1);
        return b.obj();
    }
    virtual string json() const {
        return "{ \"a\" : 1 }";
    }
};

class RealNumber : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        double d;
        ASSERT_OK(parseNumberFromString("0.7", &d));
        b.append("a", d);
        return b.obj();
    }
    virtual string json() const {
        return "{ \"a\" : 0.7 }";
    }
};

class FancyNumber : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        double d;
        ASSERT_OK(parseNumberFromString("-4.4433e-2", &d));
        b.append("a", d);
        return b.obj();
    }
    virtual string json() const {
        return "{ \"a\" : -4.4433e-2 }";
    }
};

class TwoElements : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        b.append("a", 1);
        b.append("b", "foo");
        return b.obj();
    }
    virtual string json() const {
        return "{ \"a\" : 1, \"b\" : \"foo\" }";
    }
};

class Subobject : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        b.append("a", 1);
        BSONObjBuilder c;
        c.append("z", b.done());
        return c.obj();
    }
    virtual string json() const {
        return "{ \"z\" : { \"a\" : 1 } }";
    }
};

class DeeplyNestedObject : public TestRoundTrip {
    virtual string buildJson(int depth) const {
        if (depth == 0) {
            return "{\"0\":true}";
        } else {
            std::stringstream ss;
            ss << "{\"" << depth << "\":" << buildJson(depth - 1) << "}";
            depth--;
            return ss.str();
        }
    }
    virtual BSONObj buildBson(int depth) const {
        BSONObjBuilder builder;
        if (depth == 0) {
            builder.append("0", true);
            return builder.obj();
        } else {
            std::stringstream ss;
            ss << depth;
            depth--;
            builder.append(ss.str(), buildBson(depth));
            return builder.obj();
        }
    }
    virtual BSONObj bson() const {
        return buildBson(35);
    }
    virtual string json() const {
        return buildJson(35);
    }
};

class ArrayEmpty : public TestRoundTrip {
    virtual BSONObj bson() const {
        vector<int> arr;
        BSONObjBuilder b;
        b.append("a", arr);
        return b.obj();
    }
    virtual string json() const {
        return "{ \"a\" : [] }";
    }
};

class Array : public TestRoundTrip {
    virtual BSONObj bson() const {
        vector<int> arr;
        arr.push_back(1);
        arr.push_back(2);
        arr.push_back(3);
        BSONObjBuilder b;
        b.append("a", arr);
        return b.obj();
    }
    virtual string json() const {
        return "{ \"a\" : [ 1, 2, 3 ] }";
    }
};

class True : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        b.appendBool("a", true);
        return b.obj();
    }
    virtual string json() const {
        return "{ \"a\" : true }";
    }
};

class False : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        b.appendBool("a", false);
        return b.obj();
    }
    virtual string json() const {
        return "{ \"a\" : false }";
    }
};

class Null : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        b.appendNull("a");
        return b.obj();
    }
    virtual string json() const {
        return "{ \"a\" : null }";
    }
};

class Undefined : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        b.appendUndefined("a");
        return b.obj();
    }

    virtual string json() const {
        return "{ \"a\" : undefined }";
    }
};

class EscapedCharacters : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        b.append("a", "\" \\ / \b \f \n \r \t \v");
        return b.obj();
    }
    virtual string json() const {
        return "{ \"a\" : \"\\\" \\\\ \\/ \\b \\f \\n \\r \\t \\v\" }";
    }
};

class NonEscapedCharacters : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        b.append("a", "% { a z $ # '  ");
        return b.obj();
    }
    virtual string json() const {
        return "{ \"a\" : \"\\% \\{ \\a \\z \\$ \\# \\' \\ \" }";
    }
};

class AllowedControlCharacter : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        b.append("a", "\x7f");
        return b.obj();
    }
    virtual string json() const {
        return "{ \"a\" : \"\x7f\" }";
    }
};

class NumbersInFieldName : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        b.append("b1", "b");
        return b.obj();
    }
    virtual string json() const {
        return "{ b1 : \"b\" }";
    }
};

class EscapeFieldName : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        b.append("\n", "b");
        return b.obj();
    }
    virtual string json() const {
        return "{ \"\\n\" : \"b\" }";
    }
};

class EscapedUnicodeToUtf8 : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        unsigned char u[7];
        u[0] = 0xe0 | 0x0a;
        u[1] = 0x80;
        u[2] = 0x80;
        u[3] = 0xe0 | 0x0a;
        u[4] = 0x80;
        u[5] = 0x80;
        u[6] = 0;
        b.append("a", (char*)u);
        BSONObj built = b.obj();
        ASSERT_EQUALS(string((char*)u), built.firstElement().valuestr());
        return built;
    }
    virtual string json() const {
        return "{ \"a\" : \"\\ua000\\uA000\" }";
    }
};

class Utf8AllOnes : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        unsigned char u[8];
        u[0] = 0x01;

        u[1] = 0x7f;

        u[2] = 0xdf;
        u[3] = 0xbf;

        u[4] = 0xef;
        u[5] = 0xbf;
        u[6] = 0xbf;

        u[7] = 0;

        b.append("a", (char*)u);
        return b.obj();
    }
    virtual string json() const {
        return "{ \"a\" : \"\\u0001\\u007f\\u07ff\\uffff\" }";
    }
};

class Utf8FirstByteOnes : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        unsigned char u[6];
        u[0] = 0xdc;
        u[1] = 0x80;

        u[2] = 0xef;
        u[3] = 0xbc;
        u[4] = 0x80;

        u[5] = 0;

        b.append("a", (char*)u);
        return b.obj();
    }
    virtual string json() const {
        return "{ \"a\" : \"\\u0700\\uff00\" }";
    }
};

class BinData : public TestRoundTrip {
    virtual BSONObj bson() const {
        char z[3];
        z[0] = 'a';
        z[1] = 'b';
        z[2] = 'c';
        BSONObjBuilder b;
        b.appendBinData("a", 3, BinDataGeneral, z);
        return b.obj();
    }
    virtual string json() const {
        return "{ \"a\" : BinData( 0 , \"YWJj\" ) }";
    }
};

class BinDataPaddedSingle : public TestRoundTrip {
    virtual BSONObj bson() const {
        char z[2];
        z[0] = 'a';
        z[1] = 'b';
        BSONObjBuilder b;
        b.appendBinData("a", 2, BinDataGeneral, z);
        return b.obj();
    }
    virtual string json() const {
        return "{ \"a\" : BinData( 0 , \"YWI=\" ) }";
    }
};

class BinDataPaddedDouble : public TestRoundTrip {
    virtual BSONObj bson() const {
        char z[1];
        z[0] = 'a';
        BSONObjBuilder b;
        b.appendBinData("a", 1, BinDataGeneral, z);
        return b.obj();
    }
    virtual string json() const {
        return "{ \"a\" : BinData( 0 , \"YQ==\" ) }";
    }
};

class BinDataAllChars : public TestRoundTrip {
    virtual BSONObj bson() const {
        unsigned char z[] = {0x00, 0x10, 0x83, 0x10, 0x51, 0x87, 0x20, 0x92, 0x8B, 0x30,
                             0xD3, 0x8F, 0x41, 0x14, 0x93, 0x51, 0x55, 0x97, 0x61, 0x96,
                             0x9B, 0x71, 0xD7, 0x9F, 0x82, 0x18, 0xA3, 0x92, 0x59, 0xA7,
                             0xA2, 0x9A, 0xAB, 0xB2, 0xDB, 0xAF, 0xC3, 0x1C, 0xB3, 0xD3,
                             0x5D, 0xB7, 0xE3, 0x9E, 0xBB, 0xF3, 0xDF, 0xBF};
        BSONObjBuilder b;
        b.appendBinData("a", 48, BinDataGeneral, z);
        return b.obj();
    }
    virtual string json() const {
        stringstream ss;
        ss << "{ \"a\" : BinData( 0 , \"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
           << "abcdefghijklmnopqrstuvwxyz0123456789+/\" ) }";
        return ss.str();
    }
};

class Date : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        b.appendDate("a", Date_t());
        return b.obj();
    }
    virtual string json() const {
        return "{ \"a\" : new Date( 0 ) }";
    }
};

class DateNonzero : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        b.appendDate("a", Date_t::fromMillisSinceEpoch(100));
        return b.obj();
    }
    virtual string json() const {
        return "{ \"a\" : new Date( 100 ) }";
    }
};

class DateNegative : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        b.appendDate("a", Date_t::fromMillisSinceEpoch(-1));
        return b.obj();
    }
    virtual string json() const {
        return "{ \"a\" : new Date( -1 ) }";
    }
};

class JSTimestamp : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        b.append("a", Timestamp(20, 5));
        return b.obj();
    }
    virtual string json() const {
        return "{ \"a\" : Timestamp( 20, 5 ) }";
    }
};

class TimestampMax : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        b.appendMaxForType("a", mongo::bsonTimestamp);
        BSONObj o = b.obj();
        return o;
    }
    virtual string json() const {
        Timestamp opTime = Timestamp::max();
        stringstream ss;
        ss << "{ \"a\" : Timestamp( " << opTime.getSecs() << ", " << opTime.getInc() << " ) }";
        return ss.str();
    }
};

class Regex : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        b.appendRegex("a", "b", "");
        return b.obj();
    }
    virtual string json() const {
        return "{ \"a\" : /b/ }";
    }
};

class RegexWithQuotes : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        b.appendRegex("a", "\"", "");
        return b.obj();
    }
    virtual string json() const {
        return "{ \"a\" : /\"/ }";
    }
};

class UnquotedFieldName : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        b.append("a_b", 1);
        return b.obj();
    }
    virtual string json() const {
        return "{ a_b : 1 }";
    }
};

class SingleQuotes : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        b.append("ab'c\"", "bb\b '\"");
        return b.obj();
    }
    virtual string json() const {
        return "{ 'ab\\'c\"' : 'bb\\b \\'\"' }";
    }
};

class ObjectId : public TestRoundTrip {
    virtual BSONObj bson() const {
        OID id;
        id.init("deadbeeff00ddeadbeeff00d");
        BSONObjBuilder b;
        b.appendOID("foo", &id);
        return b.obj();
    }
    virtual string json() const {
        return "{ \"foo\": ObjectId( \"deadbeeff00ddeadbeeff00d\" ) }";
    }
};

class NumberLong : public TestRoundTrip {
public:
    virtual BSONObj bson() const {
        return BSON("long" << 4611686018427387904ll);  // 2**62
    }
    virtual string json() const {
        return "{ \"long\": NumberLong(4611686018427387904) }";
    }
};

class NumberInt : public TestRoundTrip {
public:
    virtual BSONObj bson() const {
        return BSON("int" << static_cast<int>(100));
    }
    virtual string json() const {
        return "{ \"int\": NumberInt(100) }";
    }
};

class Number : public TestRoundTrip {
public:
    virtual BSONObj bson() const {
        return BSON("double" << 3.14);
    }
    virtual string json() const {
        return "{ \"double\": Number(3.14) }";
    }
};

class NumberDecimal : public TestRoundTrip {
public:
    virtual BSONObj bson() const {
        return BSON("decimal" << Decimal128("2.010"));
    }
    virtual string json() const {
        return "{ \"decimal\": NumberDecimal(\"+2.010\") }";
    }
};

class NumberDecimalNegative : public TestRoundTrip {
public:
    virtual BSONObj bson() const {
        return BSON("decimal" << Decimal128("-4.018"));
    }
    virtual string json() const {
        return "{ \"decimal\": NumberDecimal(\"-4018E-3\") }";
    }
};

class NumberDecimalMax : public TestRoundTrip {
public:
    virtual BSONObj bson() const {
        return BSON("decimal" << Decimal128("+9.999999999999999999999999999999999E6144"));
    }
    virtual string json() const {
        return "{ \"decimal\": NumberDecimal(\"+9999999999999999999999999999999999E6111\") }";
    }
};

class NumberDecimalMin : public TestRoundTrip {
public:
    virtual BSONObj bson() const {
        return BSON("decimal" << Decimal128("0.000000000000000000000000000000001E-6143"));
    }
    virtual string json() const {
        return "{ \"decimal\": NumberDecimal(\"+1E-6176\") }";
    }
};

class NumberDecimalPositiveZero : public TestRoundTrip {
public:
    virtual BSONObj bson() const {
        return BSON("decimal" << Decimal128("0"));
    }
    virtual string json() const {
        return "{ \"decimal\": NumberDecimal(\"0\") }";
    }
};

class NumberDecimalNegativeZero : public TestRoundTrip {
public:
    virtual BSONObj bson() const {
        return BSON("decimal" << Decimal128("-0"));
    }
    virtual string json() const {
        return "{ \"decimal\": NumberDecimal(\"-0\") }";
    }
};

class NumberDecimalPositiveNaN : public TestRoundTrip {
public:
    virtual BSONObj bson() const {
        return BSON("decimal" << Decimal128("NaN"));
    }
    virtual string json() const {
        return "{ \"decimal\": NumberDecimal(\"NaN\") }";
    }
};

class NumberDecimalNegativeNaN : public TestRoundTrip {
public:
    virtual BSONObj bson() const {
        return BSON("decimal" << Decimal128("-NaN"));
    }
    virtual string json() const {
        return "{ \"decimal\": NumberDecimal(\"-NaN\") }";
    }
};

class NumberDecimalPositiveInfinity : public TestRoundTrip {
public:
    virtual BSONObj bson() const {
        return BSON("decimal" << Decimal128("1E999999"));
    }
    virtual string json() const {
        return "{ \"decimal\": NumberDecimal(\"+Inf\") }";
    }
};

class NumberDecimalNegativeInfinity : public TestRoundTrip {
public:
    virtual BSONObj bson() const {
        return BSON("decimal" << Decimal128("-1E999999"));
    }
    virtual string json() const {
        return "{ \"decimal\": NumberDecimal(\"-Inf\") }";
    }
};

class NumberDecimalPrecision : public TestRoundTrip {
public:
    virtual BSONObj bson() const {
        return BSON("decimal" << Decimal128("5.00"));
    }
    virtual string json() const {
        return "{ \"decimal\": NumberDecimal(\"+500E-2\") }";
    }
};

class UUID : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        unsigned char z[] = {0xAB,
                             0xCD,
                             0xEF,
                             0xAB,
                             0xCD,
                             0xEF,
                             0xAB,
                             0xCD,
                             0xEF,
                             0xAB,
                             0xCD,
                             0xEF,
                             0x00,
                             0x00,
                             0x00,
                             0x00};
        b.appendBinData("a", 16, bdtUUID, z);
        return b.obj();
    }

    // Don't need to return anything because we are overriding both jsonOut and jsonIn
    virtual string json() const {
        return "";
    }

    // The UUID constructor corresponds to a special BinData type
    virtual string jsonIn() const {
        return "{ \"a\" : UUID(\"abcdefabcdefabcdefabcdef00000000\") }";
    }
    virtual string jsonOut() const {
        return "{ \"a\" : BinData(3,\"q83vq83vq83vq83vAAAAAA==\") }";
    }
};

class HexData : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        unsigned char z[] = {0xAB,
                             0xCD,
                             0xEF,
                             0xAB,
                             0xCD,
                             0xEF,
                             0xAB,
                             0xCD,
                             0xEF,
                             0xAB,
                             0xCD,
                             0xEF,
                             0x00,
                             0x00,
                             0x00,
                             0x00};
        b.appendBinData("a", 16, BinDataGeneral, z);
        return b.obj();
    }

    // Don't need to return anything because we are overriding both jsonOut and jsonIn
    virtual string json() const {
        return "";
    }

    // The HexData constructor creates a BinData type from a hex string
    virtual string jsonIn() const {
        return "{ \"a\" : HexData(0,\"abcdefabcdefabcdefabcdef00000000\") }";
    }
    virtual string jsonOut() const {
        return "{ \"a\" : BinData(0,\"q83vq83vq83vq83vAAAAAA==\") }";
    }
};

class MD5 : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        unsigned char z[] = {0xAB,
                             0xCD,
                             0xEF,
                             0xAB,
                             0xCD,
                             0xEF,
                             0xAB,
                             0xCD,
                             0xEF,
                             0xAB,
                             0xCD,
                             0xEF,
                             0x00,
                             0x00,
                             0x00,
                             0x00};
        b.appendBinData("a", 16, MD5Type, z);
        return b.obj();
    }

    // Don't need to return anything because we are overriding both jsonOut and jsonIn
    virtual string json() const {
        return "";
    }

    // The HexData constructor creates a BinData type from a hex string
    virtual string jsonIn() const {
        return "{ \"a\" : MD5(\"abcdefabcdefabcdefabcdef00000000\") }";
    }
    virtual string jsonOut() const {
        return "{ \"a\" : BinData(5,\"q83vq83vq83vq83vAAAAAA==\") }";
    }
};

class NullString : public TestRoundTrip {
    virtual BSONObj bson() const {
        BSONObjBuilder b;
        b.append("x", "a\0b", 4);
        return b.obj();
    }
    virtual string json() const {
        return "{ \"x\" : \"a\\u0000b\" }";
    }
};

}  // namespace RoundTripTests

class BinDataType {
public:
    void pp(const char* s, BSONElement e) {
        int len;
        const char* data = e.binData(len);
        cout << s << ":" << e.binDataType() << "\t" << len << endl;
        cout << "\t";
        for (int i = 0; i < len; i++)
            cout << (int)(data[i]) << " ";
        cout << endl;
    }

    void run() {
        unique_ptr<Scope> s(globalScriptEngine->newScope());

        const char* foo = "asdas\0asdasd";
        const char* base64 = "YXNkYXMAYXNkYXNk";

        BSONObj in;
        {
            BSONObjBuilder b;
            b.append("a", 7);
            b.appendBinData("b", 12, BinDataGeneral, foo);
            in = b.obj();
            s->setObject("x", in);
        }

        s->invokeSafe("myb = x.b; print( myb ); printjson( myb );", 0, 0);
        s->invokeSafe("y = { c : myb };", 0, 0);

        BSONObj out = s->getObject("y");
        ASSERT_EQUALS(BinData, out["c"].type());
        //            pp( "in " , in["b"] );
        //            pp( "out" , out["c"] );
        ASSERT_EQUALS(0, in["b"].woCompare(out["c"], false));

        // check that BinData js class is utilized
        s->invokeSafe("q = x.b.toString();", 0, 0);
        stringstream expected;
        expected << "BinData(" << BinDataGeneral << ",\"" << base64 << "\")";
        ASSERT_EQUALS(expected.str(), s->getString("q"));

        stringstream scriptBuilder;
        scriptBuilder << "z = { c : new BinData( " << BinDataGeneral << ", \"" << base64
                      << "\" ) };";
        string script = scriptBuilder.str();
        s->invokeSafe(script.c_str(), 0, 0);
        out = s->getObject("z");
        //            pp( "out" , out["c"] );
        ASSERT_EQUALS(0, in["b"].woCompare(out["c"], false));

        s->invokeSafe("a = { f: new BinData( 128, \"\" ) };", 0, 0);
        out = s->getObject("a");
        int len = -1;
        out["f"].binData(len);
        ASSERT_EQUALS(0, len);
        ASSERT_EQUALS(128, out["f"].binDataType());
    }
};

class VarTests {
public:
    void run() {
        unique_ptr<Scope> s(globalScriptEngine->newScope());

        ASSERT(s->exec("a = 5;", "a", false, true, false));
        ASSERT_EQUALS(5, s->getNumber("a"));

        ASSERT(s->exec("var b = 6;", "b", false, true, false));
        ASSERT_EQUALS(6, s->getNumber("b"));
    }
};

class Speed1 {
public:
    void run() {
        BSONObj start = BSON("x" << 5.0);
        BSONObj empty;

        unique_ptr<Scope> s;
        s.reset(globalScriptEngine->newScope());

        ScriptingFunction f = s->createFunction("return this.x + 6;");

        Timer t;
        double n = 0;
        for (; n < 10000; n++) {
            s->invoke(f, &empty, &start);
            ASSERT_EQUALS(11, s->getNumber("__returnValue"));
        }
        // cout << "speed1: " << ( n / t.millis() ) << " ops/ms" << endl;
    }
};

class ScopeOut {
public:
    void run() {
        unique_ptr<Scope> s;
        s.reset(globalScriptEngine->newScope());

        s->invokeSafe("x = 5;", 0, 0);
        {
            BSONObjBuilder b;
            s->append(b, "z", "x");
            ASSERT_EQUALS(BSON("z" << 5), b.obj());
        }

        s->invokeSafe("x = function(){ return 17; }", 0, 0);
        BSONObj temp;
        {
            BSONObjBuilder b;
            s->append(b, "z", "x");
            temp = b.obj();
        }

        s->invokeSafe("foo = this.z();", 0, &temp);
        ASSERT_EQUALS(17, s->getNumber("foo"));
    }
};

class RenameTest {
public:
    void run() {
        unique_ptr<Scope> s;
        s.reset(globalScriptEngine->newScope());

        s->setNumber("x", 5);
        ASSERT_EQUALS(5, s->getNumber("x"));
        ASSERT_EQUALS(Undefined, s->type("y"));

        s->rename("x", "y");
        ASSERT_EQUALS(5, s->getNumber("y"));
        ASSERT_EQUALS(Undefined, s->type("x"));

        s->rename("y", "x");
        ASSERT_EQUALS(5, s->getNumber("x"));
        ASSERT_EQUALS(Undefined, s->type("y"));
    }
};


class InvalidStoredJS {
public:
    void run() {
        BSONObjBuilder query;
        query.append("_id", "invalidstoredjs1");

        BSONObjBuilder update;
        update.append("_id", "invalidstoredjs1");
        update.appendCode("value",
                          "function () { db.test.find().forEach(function(obj) { continue; }); }");

        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        DBDirectClient client(&txn);
        client.update("test.system.js", query.obj(), update.obj(), true /* upsert */);

        unique_ptr<Scope> s(globalScriptEngine->newScope());
        client.eval("test", "invalidstoredjs1()");

        BSONObj info;
        BSONElement ret;
        ASSERT(client.eval("test", "return 5 + 12", info, ret));
        ASSERT_EQUALS(17, ret.number());
    }
};

/**
 * This tests a bug discovered in SERVER-24054, where certain interesting nan patterns crash
 * spidermonkey by looking like non-double type puns.  This verifies that we put that particular
 * interesting nan in and that we still get a nan out.
 */
class NovelNaN {
public:
    void run() {
        uint8_t bits[] = {
            16, 0, 0, 0, 0x01, 'a', '\0', 0x61, 0x79, 0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0,
        };
        unique_ptr<Scope> s(globalScriptEngine->newScope());

        s->setObject("val", BSONObj(reinterpret_cast<char*>(bits)).getOwned());

        s->invoke("val[\"a\"];", 0, 0);
        ASSERT_TRUE(std::isnan(s->getNumber("__returnValue")));
    }
};

class NoReturnSpecified {
public:
    void run() {
        unique_ptr<Scope> s(globalScriptEngine->newScope());

        s->invoke("x=5;", 0, 0);
        ASSERT_EQUALS(5, s->getNumber("__returnValue"));

        s->invoke("x='test'", 0, 0);
        ASSERT_EQUALS("test", s->getString("__returnValue"));

        s->invoke("x='return'", 0, 0);
        ASSERT_EQUALS("return", s->getString("__returnValue"));

        s->invoke("return 'return'", 0, 0);
        ASSERT_EQUALS("return", s->getString("__returnValue"));

        s->invoke("x = ' return '", 0, 0);
        ASSERT_EQUALS(" return ", s->getString("__returnValue"));

        s->invoke("x = \" return \"", 0, 0);
        ASSERT_EQUALS(" return ", s->getString("__returnValue"));

        s->invoke("x = \"' return '\"", 0, 0);
        ASSERT_EQUALS("' return '", s->getString("__returnValue"));

        s->invoke("x = '\" return \"'", 0, 0);
        ASSERT_EQUALS("\" return \"", s->getString("__returnValue"));

        s->invoke(";return 5", 0, 0);
        ASSERT_EQUALS(5, s->getNumber("__returnValue"));

        s->invoke("String('return')", 0, 0);
        ASSERT_EQUALS("return", s->getString("__returnValue"));

        s->invoke("String(' return ')", 0, 0);
        ASSERT_EQUALS(" return ", s->getString("__returnValue"));

        s->invoke("String(\"'return\")", 0, 0);
        ASSERT_EQUALS("'return", s->getString("__returnValue"));

        s->invoke("String('\"return')", 0, 0);
        ASSERT_EQUALS("\"return", s->getString("__returnValue"));
    }
};

class RecursiveInvoke {
public:
    static BSONObj callback(const BSONObj& args, void* data) {
        auto scope = static_cast<Scope*>(data);

        scope->invoke("x = 10;", 0, 0);

        return BSONObj();
    }

    void run() {
        unique_ptr<Scope> s(globalScriptEngine->newScope());

        s->injectNative("foo", callback, s.get());
        s->invoke("var x = 1; foo();", 0, 0);
        ASSERT_EQUALS(s->getNumberInt("x"), 10);
    }
};

class ErrorCodeFromInvoke {
public:
    void run() {
        unique_ptr<Scope> s(globalScriptEngine->newScope());

        {
            bool threwException = false;
            try {
                s->invoke("\"use strict\"; x = 10;", 0, 0);
            } catch (...) {
                threwException = true;

                auto status = exceptionToStatus();

                ASSERT_EQUALS(status.code(), ErrorCodes::JSInterpreterFailure);
            }

            ASSERT(threwException);
        }

        {
            bool threwException = false;
            try {
                s->invoke("UUID(1,2,3,4,5);", 0, 0);
            } catch (...) {
                threwException = true;

                auto status = exceptionToStatus();

                ASSERT_EQUALS(status.code(), ErrorCodes::BadValue);
            }

            ASSERT(threwException);
        }
    }
};

class All : public Suite {
public:
    All() : Suite("js") {
        // Initialize the Javascript interpreter
        ScriptEngine::setup();
    }

    void setupTests() {
        add<BuiltinTests>();
        add<BasicScope>();
        add<ResetScope>();
        add<FalseTests>();
        add<SimpleFunctions>();
        add<ExecLogError>();
        add<InvokeLogError>();
        add<ExecTimeout>();
        add<ExecNoTimeout>();
        add<InvokeTimeout>();
        add<InvokeNoTimeout>();

        add<ObjectMapping>();
        add<ObjectDecoding>();
        add<JSOIDTests>();
        add<SetImplicit>();
        add<ObjectModReadonlyTests>();
        add<OtherJSTypes>();
        add<SpecialDBTypes>();
        add<TypeConservation>();
        add<NumberLong>();
        add<NumberLong2>();

        add<NumberDecimal>();
        add<NumberDecimalGetFromScope>();
        add<NumberDecimalBigObject>();

        add<MaxTimestamp>();
        add<RenameTest>();

        add<WeirdObjects>();
        add<CodeTests>();
        add<BinDataType>();

        add<VarTests>();

        add<Speed1>();

        add<InvalidUTF8Check>();
        add<Utf8Check>();
        add<LongUtf8String>();

        add<ScopeOut>();
        add<InvalidStoredJS>();

        add<NovelNaN>();

        add<NoReturnSpecified>();

        add<RecursiveInvoke>();
        add<ErrorCodeFromInvoke>();

        add<RoundTripTests::DBRefTest>();
        add<RoundTripTests::DBPointerTest>();
        add<RoundTripTests::InformalDBRefTest>();
        add<RoundTripTests::InformalDBRefOIDTest>();
        add<RoundTripTests::InformalDBRefExtraFieldTest>();
        add<RoundTripTests::Empty>();
        add<RoundTripTests::EmptyWithSpace>();
        add<RoundTripTests::SingleString>();
        add<RoundTripTests::EmptyStrings>();
        add<RoundTripTests::SingleNumber>();
        add<RoundTripTests::RealNumber>();
        add<RoundTripTests::FancyNumber>();
        add<RoundTripTests::TwoElements>();
        add<RoundTripTests::Subobject>();
        add<RoundTripTests::DeeplyNestedObject>();
        add<RoundTripTests::ArrayEmpty>();
        add<RoundTripTests::Array>();
        add<RoundTripTests::True>();
        add<RoundTripTests::False>();
        add<RoundTripTests::Null>();
        add<RoundTripTests::Undefined>();
        add<RoundTripTests::EscapedCharacters>();
        add<RoundTripTests::NonEscapedCharacters>();
        add<RoundTripTests::AllowedControlCharacter>();
        add<RoundTripTests::NumbersInFieldName>();
        add<RoundTripTests::EscapeFieldName>();
        add<RoundTripTests::EscapedUnicodeToUtf8>();
        add<RoundTripTests::Utf8AllOnes>();
        add<RoundTripTests::Utf8FirstByteOnes>();
        add<RoundTripTests::BinData>();
        add<RoundTripTests::BinDataPaddedSingle>();
        add<RoundTripTests::BinDataPaddedDouble>();
        add<RoundTripTests::BinDataAllChars>();
        add<RoundTripTests::Date>();
        add<RoundTripTests::DateNonzero>();
        add<RoundTripTests::DateNegative>();
        add<RoundTripTests::JSTimestamp>();
        add<RoundTripTests::TimestampMax>();
        add<RoundTripTests::Regex>();
        add<RoundTripTests::RegexWithQuotes>();
        add<RoundTripTests::UnquotedFieldName>();
        add<RoundTripTests::SingleQuotes>();
        add<RoundTripTests::ObjectId>();
        add<RoundTripTests::NumberLong>();
        add<RoundTripTests::NumberInt>();
        add<RoundTripTests::Number>();

        add<RoundTripTests::NumberDecimal>();
        add<RoundTripTests::NumberDecimalNegative>();
        add<RoundTripTests::NumberDecimalMax>();
        add<RoundTripTests::NumberDecimalMin>();
        add<RoundTripTests::NumberDecimalPositiveZero>();
        add<RoundTripTests::NumberDecimalNegativeZero>();
        add<RoundTripTests::NumberDecimalPositiveNaN>();
        add<RoundTripTests::NumberDecimalNegativeNaN>();
        add<RoundTripTests::NumberDecimalPositiveInfinity>();
        add<RoundTripTests::NumberDecimalNegativeInfinity>();
        add<RoundTripTests::NumberDecimalPrecision>();

        add<RoundTripTests::UUID>();
        add<RoundTripTests::HexData>();
        add<RoundTripTests::MD5>();
        add<RoundTripTests::NullString>();
    }
};

SuiteInstance<All> myall;

}  // namespace JavaJSTests
