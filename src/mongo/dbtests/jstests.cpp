// jstests.cpp
//


/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include <iostream>
#include <limits>

#include "mongo/base/parse_number.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/hasher.h"
#include "mongo/db/json.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/platform/decimal128.h"
#include "mongo/scripting/engine.h"
#include "mongo/shell/shell_utils.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/future.h"
#include "mongo/util/log.h"
#include "mongo/util/time_support.h"
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
        getGlobalScriptEngine()->runTest();
    }
};

class BasicScope {
public:
    void run() {
        unique_ptr<Scope> s;
        s.reset(getGlobalScriptEngine()->newScope());

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
        s.reset( getGlobalScriptEngine()->newScope() );

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
        s.reset(getGlobalScriptEngine()->newScope());

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
        unique_ptr<Scope> s(getGlobalScriptEngine()->newScope());

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
          _threadName(mongo::getThreadName().toString()),
          _handle(mongo::logger::globalLogDomain()->attachAppender(std::make_unique<Tee>(this))) {}
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
        unique_ptr<Scope> scope(getGlobalScriptEngine()->newScope());

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
        auto ivs = getGlobalScriptEngine()->getInterpreterVersionString();
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
        unique_ptr<Scope> scope(getGlobalScriptEngine()->newScope());

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
        auto ivs = getGlobalScriptEngine()->getInterpreterVersionString();
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
        unique_ptr<Scope> s(getGlobalScriptEngine()->newScope());

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
        unique_ptr<Scope> s(getGlobalScriptEngine()->newScope());

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
        unique_ptr<Scope> s(getGlobalScriptEngine()->newScope());

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
        unique_ptr<Scope> s(getGlobalScriptEngine()->newScope());

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
        unique_ptr<Scope> s(getGlobalScriptEngine()->newScope());

        BSONObj o = BSON("x" << 17 << "y"
                             << "eliot"
                             << "z"
                             << "sara"
                             << "zz"
                             << BSONObj());
        s->setObject("blah", o, true);

        BSONObj out;

        ASSERT_THROWS(s->invoke("blah.y = 'e'", 0, 0), mongo::AssertionException);
        ASSERT_THROWS(s->invoke("blah.a = 19;", 0, 0), mongo::AssertionException);
        ASSERT_THROWS(s->invoke("blah.zz.a = 19;", 0, 0), mongo::AssertionException);
        ASSERT_THROWS(s->invoke("blah.zz = { a : 19 };", 0, 0), mongo::AssertionException);
        ASSERT_THROWS(s->invoke("delete blah['x']", 0, 0), mongo::AssertionException);

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
        unique_ptr<Scope> s(getGlobalScriptEngine()->newScope());

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
        unique_ptr<Scope> s(getGlobalScriptEngine()->newScope());

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
        unique_ptr<Scope> s(getGlobalScriptEngine()->newScope());

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
        unique_ptr<Scope> s(getGlobalScriptEngine()->newScope());
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
        ASSERT_EQUALS(NumberDouble, out.firstElement().type());

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
        unique_ptr<Scope> s(getGlobalScriptEngine()->newScope());

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
        ASSERT_BSONOBJ_EQ(in, out);
    }
};

class NumberLongUnderLimit {
public:
    void run() {
        unique_ptr<Scope> s(getGlobalScriptEngine()->newScope());

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
        unique_ptr<Scope> s(getGlobalScriptEngine()->newScope());
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
        unique_ptr<Scope> s(getGlobalScriptEngine()->newScope());
        ASSERT(s->exec("a = 5;", "a", false, true, false));
        ASSERT_TRUE(Decimal128(5).isEqual(s->getNumberDecimal("a")));
    }
};

class NumberDecimalBigObject {
public:
    void run() {
        unique_ptr<Scope> s(getGlobalScriptEngine()->newScope());

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
        ASSERT_BSONOBJ_EQ(in, out);
    }
};

class MaxTimestamp {
public:
    void run() {
        unique_ptr<Scope> s(getGlobalScriptEngine()->newScope());

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
        unique_ptr<Scope> s(getGlobalScriptEngine()->newScope());

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
        unique_ptr<Scope> scope(getGlobalScriptEngine()->newScope());

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
        unique_ptr<Scope> scope(getGlobalScriptEngine()->newScope());

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
        unique_ptr<Scope> scope(getGlobalScriptEngine()->newScope());

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

class SleepInterruption {
public:
    void run() {
        std::shared_ptr<Scope> scope(getGlobalScriptEngine()->newScope());

        auto sleep = makePromiseFuture<void>();
        auto awakened = makePromiseFuture<void>();

        // Spawn a thread which attempts to sleep indefinitely.
        stdx::thread([
            preSleep = std::move(sleep.promise),
            onAwake = std::move(awakened.promise),
            scope
        ]() mutable {
            preSleep.emplaceValue();
            onAwake.setWith([&] {
                scope->exec(
                    ""
                    "  try {"
                    "    sleep(99999999999);"
                    "  } finally {"
                    "    throw \"FAILURE\";"
                    "  }"
                    "",
                    "test",
                    false,
                    false,
                    true);
            });
        }).detach();

        // Wait until just before the sleep begins.
        sleep.future.get();

        // Attempt to wait until Javascript enters the sleep.
        // It's OK if we kill the function prematurely, before it begins sleeping. Either cause of
        // death will emit an error with the Interrupted code.
        sleepsecs(1);

        // Send the operation a kill signal.
        scope->kill();

        // Wait for the error.
        auto result = awakened.future.getNoThrow();
        ASSERT_EQ(ErrorCodes::Interrupted, result);
    }
};

/**
 * Test invoke() timeout value does not terminate execution (SERVER-8053)
 */
class InvokeNoTimeout {
public:
    void run() {
        unique_ptr<Scope> scope(getGlobalScriptEngine()->newScope());

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
        ASSERT(getGlobalScriptEngine()->utf8Ok());
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
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        DBDirectClient client(&opCtx);

        client.dropCollection(ns());
    }

    static const char* ns() {
        return "unittest.jstests.utf8check";
    }
};


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
        unique_ptr<Scope> s(getGlobalScriptEngine()->newScope());

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
        unique_ptr<Scope> s(getGlobalScriptEngine()->newScope());

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
        s.reset(getGlobalScriptEngine()->newScope());

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
        s.reset(getGlobalScriptEngine()->newScope());

        s->invokeSafe("x = 5;", 0, 0);
        {
            BSONObjBuilder b;
            s->append(b, "z", "x");
            ASSERT_BSONOBJ_EQ(BSON("z" << 5), b.obj());
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
        s.reset(getGlobalScriptEngine()->newScope());

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
        unique_ptr<Scope> s(getGlobalScriptEngine()->newScope());

        s->setObject("val", BSONObj(reinterpret_cast<char*>(bits)).getOwned());

        s->invoke("val[\"a\"];", 0, 0);
        ASSERT_TRUE(std::isnan(s->getNumber("__returnValue")));
    }
};

class NoReturnSpecified {
public:
    void run() {
        unique_ptr<Scope> s(getGlobalScriptEngine()->newScope());

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
        unique_ptr<Scope> s(getGlobalScriptEngine()->newScope());

        s->injectNative("foo", callback, s.get());
        s->invoke("var x = 1; foo();", 0, 0);
        ASSERT_EQUALS(s->getNumberInt("x"), 10);
    }
};

class ErrorCodeFromInvoke {
public:
    void run() {
        unique_ptr<Scope> s(getGlobalScriptEngine()->newScope());

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

class ErrorWithSidecarFromInvoke {
public:
    void run() {
        auto sidecarThrowingFunc = [](const BSONObj& args, void* data) -> BSONObj {
            uassertStatusOK(Status(ErrorExtraInfoExample(123), "foo"));
            return {};
        };

        unique_ptr<Scope> s(getGlobalScriptEngine()->newScope());

        s->injectNative("foo", sidecarThrowingFunc);

        ASSERT_THROWS_WITH_CHECK(
            s->invoke("try { foo(); } catch (e) { throw e; } throw new Error(\"bar\");", 0, 0),
            ExceptionFor<ErrorCodes::ForTestingErrorExtraInfo>,
            [](const auto& ex) { ASSERT_EQ(ex->data, 123); });
    }
};

class RequiresOwnedObjects {
public:
    void run() {
        char buf[] = {5, 0, 0, 0, 0};
        BSONObj unowned(buf);
        BSONObj owned = unowned.getOwned();

        ASSERT(!unowned.isOwned());
        ASSERT(owned.isOwned());

        // Ensure that by default we can bind owned and unowned
        {
            unique_ptr<Scope> s(getGlobalScriptEngine()->newScope());
            s->setObject("unowned", unowned, true);
            s->setObject("owned", owned, true);
        }

        // After we set the flag, we should only be able to set owned
        {
            unique_ptr<Scope> s(getGlobalScriptEngine()->newScope());
            s->requireOwnedObjects();
            s->setObject("owned", owned, true);

            bool threwException = false;
            try {
                s->setObject("unowned", unowned, true);
            } catch (...) {
                threwException = true;

                auto status = exceptionToStatus();

                ASSERT_EQUALS(status.code(), ErrorCodes::BadValue);
            }

            ASSERT(threwException);

            // after resetting, we can set unowned's again
            s->reset();
            s->setObject("unowned", unowned, true);
        }
    }
};

class ConvertShardKeyToHashed {
public:
    void check(shared_ptr<Scope> s, const mongo::BSONObj& o) {
        s->setObject("o", o, true);
        s->invoke("return convertShardKeyToHashed(o);", 0, 0);
        const auto scopeShardKey = s->getNumber("__returnValue");

        // Wrapping to form a proper element
        const auto wrapO = BSON("" << o);
        const auto e = wrapO[""];
        const auto trueShardKey =
            mongo::BSONElementHasher::hash64(e, mongo::BSONElementHasher::DEFAULT_HASH_SEED);

        ASSERT_EQUALS(scopeShardKey, trueShardKey);
    }

    void checkWithSeed(shared_ptr<Scope> s, const mongo::BSONObj& o, int seed) {
        s->setObject("o", o, true);
        s->setNumber("seed", seed);
        s->invoke("return convertShardKeyToHashed(o, seed);", 0, 0);
        const auto scopeShardKey = s->getNumber("__returnValue");

        // Wrapping to form a proper element
        const auto wrapO = BSON("" << o);
        const auto e = wrapO[""];
        const auto trueShardKey = mongo::BSONElementHasher::hash64(e, seed);

        ASSERT_EQUALS(scopeShardKey, trueShardKey);
    }

    void checkNoArgs(shared_ptr<Scope> s) {
        s->invoke("return convertShardKeyToHashed();", 0, 0);
    }

    void checkWithExtraArg(shared_ptr<Scope> s, const mongo::BSONObj& o, int seed) {
        s->setObject("o", o, true);
        s->setNumber("seed", seed);
        s->invoke("return convertShardKeyToHashed(o, seed, 1);", 0, 0);
    }

    void checkWithBadSeed(shared_ptr<Scope> s, const mongo::BSONObj& o) {
        s->setObject("o", o, true);
        s->setString("seed", "sunflower");
        s->invoke("return convertShardKeyToHashed(o, seed);", 0, 0);
    }

    void run() {
        shared_ptr<Scope> s(getGlobalScriptEngine()->newScope());
        shell_utils::installShellUtils(*s);

        // Check a few elementary objects
        check(s, BSON("" << 1));
        check(s, BSON("" << 10.0));
        check(s,
              BSON(""
                   << "Shardy"));
        check(s, BSON("" << BSON_ARRAY(1 << 2 << 3)));
        check(s, BSON("" << mongo::jstNULL));
        check(s, BSON("" << mongo::BSONObj()));
        check(s,
              BSON("A" << 1 << "B"
                       << "Shardy"));

        // Check a few different seeds
        checkWithSeed(s,
                      BSON(""
                           << "Shardy"),
                      mongo::BSONElementHasher::DEFAULT_HASH_SEED);
        checkWithSeed(s,
                      BSON(""
                           << "Shardy"),
                      0);
        checkWithSeed(s,
                      BSON(""
                           << "Shardy"),
                      -1);

        ASSERT_THROWS(checkNoArgs(s), mongo::DBException);
        ASSERT_THROWS(checkWithExtraArg(s, BSON("" << 10.0), 0), mongo::DBException);
        ASSERT_THROWS(checkWithBadSeed(s, BSON("" << 1)), mongo::DBException);
    }
};

class All : public Suite {
public:
    All() : Suite("js") {}

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
        add<SleepInterruption>();
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
        add<BinDataType>();

        add<VarTests>();
        add<Speed1>();
        add<Utf8Check>();
        add<ScopeOut>();
        add<NovelNaN>();
        add<NoReturnSpecified>();

        add<RecursiveInvoke>();
        add<ErrorCodeFromInvoke>();
        add<ErrorWithSidecarFromInvoke>();
        add<RequiresOwnedObjects>();
        add<ConvertShardKeyToHashed>();
    }
};

SuiteInstance<All> myall;

}  // namespace JSTests
