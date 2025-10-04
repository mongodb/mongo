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

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/json.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/hasher.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/platform/decimal128.h"
#include "mongo/scripting/engine.h"
#include "mongo/shell/shell_utils.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace JSTests {

using ScopeFactory = Scope* (ScriptEngine::*)();

template <ScopeFactory scopeFactory>
class BuiltinTests {
public:
    void run() {
        // Run any tests included with the scripting engine
        getGlobalScriptEngine()->runTest();
    }
};

template <ScopeFactory scopeFactory>
class BasicScope {
public:
    void run() {
        std::unique_ptr<Scope> s;
        s.reset((getGlobalScriptEngine()->*scopeFactory)());

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

template <ScopeFactory scopeFactory>
class ResetScope {
public:
    void run() {
        /* Currently reset does not clear data in v8 or spidermonkey scopes.  See SECURITY-10
       std::unique_ptr<Scope> s;
        s.reset( (getGlobalScriptEngine()->*scopeFactory)() );

        s->setBoolean( "x" , true );
        ASSERT( s->getBoolean( "x" ) );

        s->reset();
        ASSERT( !s->getBoolean( "x" ) );
        */
    }
};

template <ScopeFactory scopeFactory>
class FalseTests {
public:
    void run() {
        // Test falsy javascript values
        std::unique_ptr<Scope> s;
        s.reset((getGlobalScriptEngine()->*scopeFactory)());

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

template <ScopeFactory scopeFactory>
class SimpleFunctions {
public:
    void run() {
        std::unique_ptr<Scope> s((getGlobalScriptEngine()->*scopeFactory)());

        s->invoke("x=5;", nullptr, nullptr);
        ASSERT(5 == s->getNumber("x"));

        s->invoke("return 17;", nullptr, nullptr);
        ASSERT(17 == s->getNumber("__returnValue"));

        s->invoke("function(){ return 18; }", nullptr, nullptr);
        ASSERT(18 == s->getNumber("__returnValue"));

        s->setNumber("x", 1.76);
        s->invoke("return x == 1.76; ", nullptr, nullptr);
        ASSERT(s->getBoolean("__returnValue"));

        s->setNumber("x", 1.76);
        s->invoke("return x == 1.79; ", nullptr, nullptr);
        ASSERT(!s->getBoolean("__returnValue"));

        BSONObj obj = BSON("" << 11.0);
        s->invoke("function( z ){ return 5 + z; }", &obj, nullptr);
        ASSERT_EQUALS(16, s->getNumber("__returnValue"));
    }
};

template <ScopeFactory scopeFactory>
class ObjectMapping {
public:
    void run() {
        std::unique_ptr<Scope> s((getGlobalScriptEngine()->*scopeFactory)());

        BSONObj o = BSON("x" << 17.0 << "y"
                             << "eliot"
                             << "z"
                             << "sara");
        s->setObject("blah", o);

        s->invoke("return blah.x;", nullptr, nullptr);
        ASSERT_EQUALS(17, s->getNumber("__returnValue"));
        s->invoke("return blah.y;", nullptr, nullptr);
        ASSERT_EQUALS("eliot", s->getString("__returnValue"));

        s->invoke("return this.z;", nullptr, &o);
        ASSERT_EQUALS("sara", s->getString("__returnValue"));

        s->invoke("return this.z == 'sara';", nullptr, &o);
        ASSERT_EQUALS(true, s->getBoolean("__returnValue"));

        s->invoke("this.z == 'sara';", nullptr, &o);
        ASSERT_EQUALS(true, s->getBoolean("__returnValue"));

        s->invoke("this.z == 'asara';", nullptr, &o);
        ASSERT_EQUALS(false, s->getBoolean("__returnValue"));

        s->invoke("return this.x == 17;", nullptr, &o);
        ASSERT_EQUALS(true, s->getBoolean("__returnValue"));

        s->invoke("return this.x == 18;", nullptr, &o);
        ASSERT_EQUALS(false, s->getBoolean("__returnValue"));

        s->invoke("function(){ return this.x == 17; }", nullptr, &o);
        ASSERT_EQUALS(true, s->getBoolean("__returnValue"));

        s->invoke("function(){ return this.x == 18; }", nullptr, &o);
        ASSERT_EQUALS(false, s->getBoolean("__returnValue"));

        s->invoke("function (){ return this.x == 17; }", nullptr, &o);
        ASSERT_EQUALS(true, s->getBoolean("__returnValue"));

        s->invoke("function z(){ return this.x == 18; }", nullptr, &o);
        ASSERT_EQUALS(false, s->getBoolean("__returnValue"));

        s->invoke("function (){ this.x == 17; }", nullptr, &o);
        ASSERT_EQUALS(false, s->getBoolean("__returnValue"));

        s->invoke("function z(){ this.x == 18; }", nullptr, &o);
        ASSERT_EQUALS(false, s->getBoolean("__returnValue"));

        s->invoke("x = 5; for( ; x <10; x++){ a = 1; }", nullptr, &o);
        ASSERT_EQUALS(10, s->getNumber("x"));
    }
};

template <ScopeFactory scopeFactory>
class ObjectDecoding {
public:
    void run() {
        std::unique_ptr<Scope> s((getGlobalScriptEngine()->*scopeFactory)());

        s->invoke("z = { num : 1 };", nullptr, nullptr);
        BSONObj out = s->getObject("z");
        ASSERT_EQUALS(1, out["num"].number());
        ASSERT_EQUALS(1, out.nFields());

        s->invoke("z = { x : 'eliot' };", nullptr, nullptr);
        out = s->getObject("z");
        ASSERT_EQUALS("eliot", out["x"].str());
        ASSERT_EQUALS(1, out.nFields());

        BSONObj o = BSON("x" << 17);
        s->setObject("blah", o);
        out = s->getObject("blah");
        ASSERT_EQUALS(17, out["x"].number());
    }
};

template <ScopeFactory scopeFactory>
class JSOIDTests {
public:
    void run() {
#ifdef MOZJS
        std::unique_ptr<Scope> s((getGlobalScriptEngine()->*scopeFactory)());

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

template <ScopeFactory scopeFactory>
class SetImplicit {
public:
    void run() {
        std::unique_ptr<Scope> s((getGlobalScriptEngine()->*scopeFactory)());

        BSONObj o = BSON("foo" << "bar");
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

template <ScopeFactory scopeFactory>
class ObjectModReadonlyTests {
public:
    void run() {
        std::unique_ptr<Scope> s((getGlobalScriptEngine()->*scopeFactory)());

        BSONObj o = BSON("x" << 17 << "y"
                             << "eliot"
                             << "z"
                             << "sara"
                             << "zz" << BSONObj());
        s->setObject("blah", o, true);

        BSONObj out;

        ASSERT_THROWS(s->invoke("blah.y = 'e'", nullptr, nullptr), mongo::AssertionException);
        ASSERT_THROWS(s->invoke("blah.a = 19;", nullptr, nullptr), mongo::AssertionException);
        ASSERT_THROWS(s->invoke("blah.zz.a = 19;", nullptr, nullptr), mongo::AssertionException);
        ASSERT_THROWS(s->invoke("blah.zz = { a : 19 };", nullptr, nullptr),
                      mongo::AssertionException);
        ASSERT_THROWS(s->invoke("delete blah['x']", nullptr, nullptr), mongo::AssertionException);

        // read-only object itself can be overwritten
        s->invoke("blah = {}", nullptr, nullptr);
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

template <ScopeFactory scopeFactory>
class OtherJSTypes {
public:
    void run() {
        std::unique_ptr<Scope> s((getGlobalScriptEngine()->*scopeFactory)());

        {
            // date
            BSONObj o;
            {
                BSONObjBuilder b;
                b.appendDate("d", Date_t::fromMillisSinceEpoch(123456789));
                o = b.obj();
            }
            s->setObject("x", o);

            s->invoke("return x.d.getTime() != 12;", nullptr, nullptr);
            ASSERT_EQUALS(true, s->getBoolean("__returnValue"));

            s->invoke("z = x.d.getTime();", nullptr, nullptr);
            ASSERT_EQUALS(123456789, s->getNumber("z"));

            s->invoke("z = { z : x.d }", nullptr, nullptr);
            BSONObj out = s->getObject("z");
            ASSERT(out["z"].type() == BSONType::date);
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

            s->invoke("z = x.r.test( 'b' );", nullptr, nullptr);
            ASSERT_EQUALS(false, s->getBoolean("z"));

            s->invoke("z = x.r.test( 'a' );", nullptr, nullptr);
            ASSERT_EQUALS(true, s->getBoolean("z"));

            s->invoke("z = x.r.test( 'ba' );", nullptr, nullptr);
            ASSERT_EQUALS(false, s->getBoolean("z"));

            s->invoke("z = { a : x.r };", nullptr, nullptr);

            BSONObj out = s->getObject("z");
            ASSERT_EQUALS((std::string) "^a", out["a"].regex());
            ASSERT_EQUALS((std::string) "i", out["a"].regexFlags());

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
                "    assert(threw);"  // NOLINT
                "}";
            ASSERT_EQUALS(s->invoke(code, &invalidRegex, nullptr), 0);
        }

        // array
        {
            BSONObj o = fromjson("{r:[1,2,3]}");
            s->setObject("x", o, false);
            BSONObj out = s->getObject("x");
            ASSERT_EQUALS(BSONType::array, out.firstElement().type());

            s->setObject("x", o, true);
            out = s->getObject("x");
            ASSERT_EQUALS(BSONType::array, out.firstElement().type());
        }

        // symbol
        {
            // test mutable object with symbol type
            BSONObjBuilder builder;
            builder.appendSymbol("sym", "value");
            BSONObj in = builder.done();
            s->setObject("x", in, false);
            BSONObj out = s->getObject("x");
            ASSERT_EQUALS(BSONType::symbol, out.firstElement().type());

            // readonly
            s->setObject("x", in, true);
            out = s->getObject("x");
            ASSERT_EQUALS(BSONType::symbol, out.firstElement().type());
        }
    }
};

template <ScopeFactory scopeFactory>
class SpecialDBTypes {
public:
    void run() {
        std::unique_ptr<Scope> s((getGlobalScriptEngine()->*scopeFactory)());

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

        ASSERT(s->invoke("y = { a : z.a , b : z.b , c : z.c , d: z.d }", nullptr, nullptr) == 0);

        BSONObj out = s->getObject("y");
        ASSERT_EQUALS(BSONType::timestamp, out["a"].type());
        ASSERT_EQUALS(BSONType::minKey, out["b"].type());
        ASSERT_EQUALS(BSONType::maxKey, out["c"].type());
        ASSERT_EQUALS(BSONType::timestamp, out["d"].type());

        ASSERT_EQUALS(9876U, out["d"].timestampInc());
        ASSERT_EQUALS(Date_t::fromMillisSinceEpoch(1234000), out["d"].timestampTime());
        ASSERT_EQUALS(Date_t::fromMillisSinceEpoch(123456789), out["a"].date());
    }
};

template <ScopeFactory scopeFactory>
class TypeConservation {
public:
    void run() {
        std::unique_ptr<Scope> s((getGlobalScriptEngine()->*scopeFactory)());

        //  --  A  --

        BSONObj o;
        {
            BSONObjBuilder b;
            b.append("a", (int)5);
            b.append("b", 5.6);
            o = b.obj();
        }
        ASSERT_EQUALS(BSONType::numberInt, o["a"].type());
        ASSERT_EQUALS(BSONType::numberDouble, o["b"].type());

        s->setObject("z", o);
        s->invoke("return z", nullptr, nullptr);
        BSONObj out = s->getObject("__returnValue");
        ASSERT_EQUALS(5, out["a"].number());
        ASSERT_EQUALS(5.6, out["b"].number());

        ASSERT_EQUALS(BSONType::numberDouble, out["b"].type());
        ASSERT_EQUALS(BSONType::numberInt, out["a"].type());

        //  --  B  --

        {
            BSONObjBuilder b;
            b.append("a", (int)5);
            b.append("b", 5.6);
            o = b.obj();
        }

        s->setObject("z", o, false);
        s->invoke("return z", nullptr, nullptr);
        out = s->getObject("__returnValue");
        ASSERT_EQUALS(5, out["a"].number());
        ASSERT_EQUALS(5.6, out["b"].number());

        ASSERT_EQUALS(BSONType::numberDouble, out["b"].type());
        ASSERT_EQUALS(BSONType::numberInt, out["a"].type());


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

        ASSERT_EQUALS(BSONType::numberDouble, o["a"].embeddedObjectUserCheck()["0"].type());
        ASSERT_EQUALS(BSONType::numberInt, o["a"].embeddedObjectUserCheck()["1"].type());

        s->setObject("z", o, false);
        out = s->getObject("z");

        ASSERT_EQUALS(BSONType::numberDouble, out["a"].embeddedObjectUserCheck()["0"].type());
        ASSERT_EQUALS(BSONType::numberInt, out["a"].embeddedObjectUserCheck()["1"].type());

        s->invokeSafe("z.z = 5;", nullptr, nullptr);
        out = s->getObject("z");
        ASSERT_EQUALS(5, out["z"].number());
        ASSERT_EQUALS(BSONType::numberDouble, out["a"].embeddedObjectUserCheck()["0"].type());
        // Commenting so that v8 tests will work
        // TODO: this is technically bad, but here to make sure that i understand the behavior
        // ASSERT_EQUALS(BSONType::numberDouble , out["a"].embeddedObjectUserCheck()["1"].type() );


        // Eliot says I don't have to worry about this case

        //            // -- D --
        //
        //            o = fromjson( "{a:3.0,b:4.5}" );
        //            ASSERT_EQUALS(BSONType::numberDouble , o["a"].type() );
        //            ASSERT_EQUALS(BSONType::numberDouble , o["b"].type() );
        //
        //            s->setObject( "z" , o , false );
        //            s->invoke( "return z" , BSONObj() );
        //            out = s->getObject( "__returnValue" );
        //            ASSERT_EQUALS( 3 , out["a"].number() );
        //            ASSERT_EQUALS( 4.5 , out["b"].number() );
        //
        //            ASSERT_EQUALS(BSONType::numberDouble , out["b"].type() );
        //            ASSERT_EQUALS(BSONType::numberDouble , out["a"].type() );
        //
    }
};

template <ScopeFactory scopeFactory>
class NumberLong {
public:
    void run() {
        std::unique_ptr<Scope> s((getGlobalScriptEngine()->*scopeFactory)());
        BSONObjBuilder b;
        long long val = (long long)(0xbabadeadbeefbaddULL);
        b.append("a", val);
        BSONObj in = b.obj();
        s->setObject("a", in);
        BSONObj out = s->getObject("a");
        ASSERT_EQUALS(mongo::BSONType::numberLong, out.firstElement().type());

        ASSERT(s->exec("b = {b:a.a}", "foo", false, true, false));
        out = s->getObject("b");
        ASSERT_EQUALS(mongo::BSONType::numberLong, out.firstElement().type());
        if (val != out.firstElement().numberLong()) {
            std::cout << val << std::endl;
            std::cout << out.firstElement().numberLong() << std::endl;
            std::cout << out.toString() << std::endl;
            ASSERT_EQUALS(val, out.firstElement().numberLong());
        }

        ASSERT(s->exec("c = {c:a.a.toString()}", "foo", false, true, false));
        out = s->getObject("c");
        std::stringstream ss;
        ss << "NumberLong(\"" << val << "\")";
        ASSERT_EQUALS(ss.str(), out.firstElement().str());

        ASSERT(s->exec("d = {d:a.a.toNumber()}", "foo", false, true, false));
        out = s->getObject("d");
        ASSERT_EQUALS(BSONType::numberDouble, out.firstElement().type());
        ASSERT_EQUALS(double(val), out.firstElement().number());

        ASSERT(s->exec("e = {e:a.a.floatApprox}", "foo", false, true, false));
        out = s->getObject("e");
        ASSERT_EQUALS(BSONType::numberDouble, out.firstElement().type());
        ASSERT_EQUALS(double(val), out.firstElement().number());

        ASSERT(s->exec("f = {f:a.a.top}", "foo", false, true, false));
        out = s->getObject("f");
        ASSERT(BSONType::numberDouble == out.firstElement().type() ||
               BSONType::numberInt == out.firstElement().type());

        s->setObject("z", BSON("z" << (long long)(4)));
        ASSERT(s->exec("y = {y:z.z.top}", "foo", false, true, false));
        out = s->getObject("y");
        ASSERT_EQUALS(BSONType::numberDouble, out.firstElement().type());

        ASSERT(s->exec("x = {x:z.z.floatApprox}", "foo", false, true, false));
        out = s->getObject("x");
        ASSERT(BSONType::numberDouble == out.firstElement().type() ||
               BSONType::numberInt == out.firstElement().type());
        ASSERT_EQUALS(double(4), out.firstElement().number());

        ASSERT(s->exec("w = {w:z.z}", "foo", false, true, false));
        out = s->getObject("w");
        ASSERT_EQUALS(mongo::BSONType::numberLong, out.firstElement().type());
        ASSERT_EQUALS(4, out.firstElement().numberLong());
    }
};

template <ScopeFactory scopeFactory>
class NumberLong2 {
public:
    void run() {
        std::unique_ptr<Scope> s((getGlobalScriptEngine()->*scopeFactory)());

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
        std::string outString = s->getString("x");

        ASSERT(s->exec((std::string) "y = " + outString, "foo2", false, true, false));
        BSONObj out = s->getObject("y");
        ASSERT_BSONOBJ_EQ(in, out);
    }
};

template <ScopeFactory scopeFactory>
class NumberLongUnderLimit {
public:
    void run() {
        std::unique_ptr<Scope> s((getGlobalScriptEngine()->*scopeFactory)());

        BSONObjBuilder b;
        // limit is 2^53
        long long val = (long long)(9007199254740991ULL);
        b.append("a", val);
        BSONObj in = b.obj();
        s->setObject("a", in);
        BSONObj out = s->getObject("a");
        ASSERT_EQUALS(mongo::BSONType::numberLong, out.firstElement().type());

        ASSERT(s->exec("b = {b:a.a}", "foo", false, true, false));
        out = s->getObject("b");
        ASSERT_EQUALS(mongo::BSONType::numberLong, out.firstElement().type());
        if (val != out.firstElement().numberLong()) {
            std::cout << val << std::endl;
            std::cout << out.firstElement().numberLong() << std::endl;
            std::cout << out.toString() << std::endl;
            ASSERT_EQUALS(val, out.firstElement().numberLong());
        }

        ASSERT(s->exec("c = {c:a.a.toString()}", "foo", false, true, false));
        out = s->getObject("c");
        std::stringstream ss;
        ss << "NumberLong(\"" << val << "\")";
        ASSERT_EQUALS(ss.str(), out.firstElement().str());

        ASSERT(s->exec("d = {d:a.a.toNumber()}", "foo", false, true, false));
        out = s->getObject("d");
        ASSERT_EQUALS(BSONType::numberDouble, out.firstElement().type());
        ASSERT_EQUALS(double(val), out.firstElement().number());

        ASSERT(s->exec("e = {e:a.a.floatApprox}", "foo", false, true, false));
        out = s->getObject("e");
        ASSERT_EQUALS(BSONType::numberDouble, out.firstElement().type());
        ASSERT_EQUALS(double(val), out.firstElement().number());

        ASSERT(s->exec("f = {f:a.a.top}", "foo", false, true, false));
        out = s->getObject("f");
        ASSERT(BSONType::undefined == out.firstElement().type());
    }
};

template <ScopeFactory scopeFactory>
class NumberDecimal {
public:
    void run() {
        std::unique_ptr<Scope> s((getGlobalScriptEngine()->*scopeFactory)());
        BSONObjBuilder b;
        Decimal128 val = Decimal128("2.010");
        b.append("a", val);
        BSONObj in = b.obj();
        s->setObject("a", in);

        // Test the scope object
        BSONObj out = s->getObject("a");
        ASSERT_EQUALS(mongo::BSONType::numberDecimal, out.firstElement().type());
        ASSERT_TRUE(val.isEqual(out.firstElement().numberDecimal()));

        ASSERT(s->exec("b = {b:a.a}", "foo", false, true, false));
        out = s->getObject("b");
        ASSERT_EQUALS(mongo::BSONType::numberDecimal, out.firstElement().type());
        ASSERT_TRUE(val.isEqual(out.firstElement().numberDecimal()));

        // Test that the appropriatestd::string output is generated
        ASSERT(s->exec("c = {c:a.a.toString()}", "foo", false, true, false));
        out = s->getObject("c");
        std::stringstream ss;
        ss << "NumberDecimal(\"" << val.toString() << "\")";
        ASSERT_EQUALS(ss.str(), out.firstElement().str());
    }
};

template <ScopeFactory scopeFactory>
class NumberDecimalGetFromScope {
public:
    void run() {
        std::unique_ptr<Scope> s((getGlobalScriptEngine()->*scopeFactory)());
        ASSERT(s->exec("a = 5;", "a", false, true, false));
        ASSERT_TRUE(Decimal128(5).isEqual(s->getNumberDecimal("a")));
    }
};

template <ScopeFactory scopeFactory>
class NumberDecimalBigObject {
public:
    void run() {
        std::unique_ptr<Scope> s((getGlobalScriptEngine()->*scopeFactory)());

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
        std::string outString = s->getString("x");

        ASSERT(s->exec((std::string) "y = " + outString, "foo2", false, true, false));
        BSONObj out = s->getObject("y");
        ASSERT_BSONOBJ_EQ(in, out);
    }
};

template <ScopeFactory scopeFactory>
class MaxTimestamp {
public:
    void run() {
        std::unique_ptr<Scope> s((getGlobalScriptEngine()->*scopeFactory)());

        // Timestamp 't' component can exceed max for int32_t.
        BSONObj in;
        {
            BSONObjBuilder b;
            b.bb().appendNum(static_cast<char>(BSONType::timestamp));
            b.bb().appendCStr("a");
            b.bb().appendNum(std::numeric_limits<unsigned long long>::max());

            in = b.obj();
        }
        s->setObject("a", in);

        ASSERT(s->exec("x = tojson( a ); ", "foo", false, true, false));
    }
};

template <ScopeFactory scopeFactory>
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
        std::unique_ptr<Scope> s((getGlobalScriptEngine()->*scopeFactory)());

        for (int i = 5; i < 100; i += 10) {
            s->setObject("a", build(i), false);
            s->invokeSafe("tojson( a )", nullptr, nullptr);

            s->setObject("a", build(5), true);
            s->invokeSafe("tojson( a )", nullptr, nullptr);
        }
    }
};

/**
 * Test exec() timeout value terminates execution (SERVER-8053)
 */
template <ScopeFactory scopeFactory>
class ExecTimeout {
public:
    void run() {
        std::unique_ptr<Scope> scope((getGlobalScriptEngine()->*scopeFactory)());

        // assert timeout occurred
        ASSERT(!scope->exec("var a = 1; while (true) { ; }", "ExecTimeout", false, true, false, 1));
    }
};

/**
 * Test exec() timeout value terminates execution (SERVER-8053)
 */
template <ScopeFactory scopeFactory>
class ExecNoTimeout {
public:
    void run() {
        std::unique_ptr<Scope> scope((getGlobalScriptEngine()->*scopeFactory)());

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
template <ScopeFactory scopeFactory>
class InvokeTimeout {
public:
    void run() {
        std::unique_ptr<Scope> scope((getGlobalScriptEngine()->*scopeFactory)());

        // scope timeout after 500ms
        bool caught = false;
        try {
            scope->invokeSafe(
                "function() {         "
                "    while (true) { } "
                "}                    ",
                nullptr,
                nullptr,
                1);
        } catch (const DBException&) {
            caught = true;
        }
        ASSERT(caught);
    }
};

template <ScopeFactory scopeFactory>
class SleepInterruption {
public:
    void run() {
        auto scopePF = makePromiseFuture<Scope*>();
        auto awakenedPF = makePromiseFuture<void>();
        auto safeToDestroyScopePF = makePromiseFuture<void>();

        // Spawn a thread which attempts to sleep indefinitely.
        stdx::thread thread([&] {
            std::unique_ptr<Scope> scope((getGlobalScriptEngine()->*scopeFactory)());
            scopePF.promise.emplaceValue(scope.get());
            awakenedPF.promise.setWith([&] {
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

            // The parent thread uses the 'scope' pointer to send the "kill" signal, making it
            // unsafe to destroy 'scope' until we have confirmation that the parent no longer needs
            // it. This wait protects against a use-after-free error that can occur if Scope::exec()
            // fails instead of executing its long sleep.
            safeToDestroyScopePF.future.wait();
        });

        // Wait until just before the sleep begins.
        auto scope = scopePF.future.get();

        // Attempt to wait until Javascript enters the sleep.
        // It's OK if we kill the function prematurely, before it begins sleeping. Either cause of
        // death will emit an error with the Interrupted code.
        sleepsecs(1);

        // Send the operation a kill signal.
        scope->kill();
        safeToDestroyScopePF.promise.setWith([&scope] { scope = nullptr; });

        // Wait for the error.
        auto result = awakenedPF.future.getNoThrow();
        ASSERT_EQ(ErrorCodes::Interrupted, result);

        thread.join();
    }
};

/**
 * Test invoke() timeout value does not terminate execution (SERVER-8053)
 */
template <ScopeFactory scopeFactory>
class InvokeNoTimeout {
public:
    void run() {
        std::unique_ptr<Scope> scope((getGlobalScriptEngine()->*scopeFactory)());

        // invoke completes before timeout
        scope->invokeSafe(
            "function() { "
            "  for (var i=0; i<1; i++) { ; } "
            "} ",
            nullptr,
            nullptr,
            5 * 60 * 1000);
    }
};


template <ScopeFactory scopeFactory>
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
            static std::string fail = std::string("Assertion failure expected ") + one.toString() +
                ", got " + two.toString();
            FAIL(fail.c_str());
        }
    }

    void reset() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        DBDirectClient client(&opCtx);

        client.dropCollection(nss());
    }

    static const char* ns() {
        return "unittest.jstests.utf8check";
    }

    static NamespaceString nss() {
        return NamespaceString::createNamespaceString_forTest(ns());
    }
};


template <ScopeFactory scopeFactory>
class BinDataType {
public:
    void pp(const char* s, BSONElement e) {
        int len;
        const char* data = e.binData(len);
        std::cout << s << ":" << e.binDataType() << "\t" << len << std::endl;
        std::cout << "\t";
        for (int i = 0; i < len; i++)
            std::cout << (int)(data[i]) << " ";
        std::cout << std::endl;
    }

    void run() {
        std::unique_ptr<Scope> s((getGlobalScriptEngine()->*scopeFactory)());

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

        s->invokeSafe("myb = x.b; print( myb ); printjson( myb );", nullptr, nullptr);
        s->invokeSafe("y = { c : myb };", nullptr, nullptr);

        BSONObj out = s->getObject("y");
        ASSERT_EQUALS(BSONType::binData, out["c"].type());
        //            pp( "in " , in["b"] );
        //            pp( "out" , out["c"] );
        ASSERT_EQUALS(0, in["b"].woCompare(out["c"], false));

        // check that BinData js class is utilized
        s->invokeSafe("q = x.b.toString();", nullptr, nullptr);
        std::stringstream expected;
        expected << "BinData(" << BinDataGeneral << ",\"" << base64 << "\")";
        ASSERT_EQUALS(expected.str(), s->getString("q"));

        std::stringstream scriptBuilder;
        scriptBuilder << "z = { c : new BinData( " << BinDataGeneral << ", \"" << base64
                      << "\" ) };";
        std::string script = scriptBuilder.str();
        s->invokeSafe(script.c_str(), nullptr, nullptr);
        out = s->getObject("z");
        //            pp( "out" , out["c"] );
        ASSERT_EQUALS(0, in["b"].woCompare(out["c"], false));

        s->invokeSafe("a = { f: new BinData( 128, \"\" ) };", nullptr, nullptr);
        out = s->getObject("a");
        int len = -1;
        out["f"].binData(len);
        ASSERT_EQUALS(0, len);
        ASSERT_EQUALS(128, out["f"].binDataType());
    }
};

template <ScopeFactory scopeFactory>
class VarTests {
public:
    void run() {
        std::unique_ptr<Scope> s((getGlobalScriptEngine()->*scopeFactory)());

        ASSERT(s->exec("a = 5;", "a", false, true, false));
        ASSERT_EQUALS(5, s->getNumber("a"));

        ASSERT(s->exec("var b = 6;", "b", false, true, false));
        ASSERT_EQUALS(6, s->getNumber("b"));
    }
};

template <ScopeFactory scopeFactory>
class Speed1 {
public:
    void run() {
        BSONObj start = BSON("x" << 5.0);
        BSONObj empty;

        std::unique_ptr<Scope> s;
        s.reset((getGlobalScriptEngine()->*scopeFactory)());

        ScriptingFunction f = s->createFunction("return this.x + 6;");

        Timer t;
        double n = 0;
        for (; n < 10000; n++) {
            s->invoke(f, &empty, &start);
            ASSERT_EQUALS(11, s->getNumber("__returnValue"));
        }
        // std::cout << "speed1: " << ( n / t.millis() ) << " ops/ms" << std::endl;
    }
};

template <ScopeFactory scopeFactory>
class ScopeOut {
public:
    void run() {
        std::unique_ptr<Scope> s;
        s.reset((getGlobalScriptEngine()->*scopeFactory)());

        s->invokeSafe("x = 5;", nullptr, nullptr);
        {
            BSONObjBuilder b;
            s->append(b, "z", "x");
            ASSERT_BSONOBJ_EQ(BSON("z" << 5), b.obj());
        }

        s->invokeSafe("x = function(){ return 17; }", nullptr, nullptr);
        BSONObj temp;
        {
            BSONObjBuilder b;
            s->append(b, "z", "x");
            temp = b.obj();
        }

        s->invokeSafe("foo = this.z();", nullptr, &temp);
        ASSERT_EQUALS(17, s->getNumber("foo"));
    }
};

template <ScopeFactory scopeFactory>
class RenameTest {
public:
    void run() {
        std::unique_ptr<Scope> s;
        s.reset((getGlobalScriptEngine()->*scopeFactory)());

        s->setNumber("x", 5);
        ASSERT_EQUALS(5, s->getNumber("x"));
        ASSERT_EQUALS(stdx::to_underlying(BSONType::undefined), s->type("y"));

        s->rename("x", "y");
        ASSERT_EQUALS(5, s->getNumber("y"));
        ASSERT_EQUALS(stdx::to_underlying(BSONType::undefined), s->type("x"));

        s->rename("y", "x");
        ASSERT_EQUALS(5, s->getNumber("x"));
        ASSERT_EQUALS(stdx::to_underlying(BSONType::undefined), s->type("y"));
    }
};

/**
 * This tests a bug discovered in SERVER-24054, where certain interesting nan patterns crash
 * spidermonkey by looking like non-double type puns.  This verifies that we put that particular
 * interesting nan in and that we still get a nan out.
 */
template <ScopeFactory scopeFactory>
class NovelNaN {
public:
    void run() {
        uint8_t bits[] = {
            16,
            0,
            0,
            0,
            0x01,
            'a',
            '\0',
            0x61,
            0x79,
            0xfe,
            0xff,
            0xff,
            0xff,
            0xff,
            0xff,
            0,
        };
        std::unique_ptr<Scope> s((getGlobalScriptEngine()->*scopeFactory)());

        s->setObject("val", BSONObj(reinterpret_cast<char*>(bits)).getOwned());

        s->invoke("val[\"a\"];", nullptr, nullptr);
        ASSERT_TRUE(std::isnan(s->getNumber("__returnValue")));
    }
};

template <ScopeFactory scopeFactory>
class NoReturnSpecified {
public:
    void run() {
        std::unique_ptr<Scope> s((getGlobalScriptEngine()->*scopeFactory)());

        s->invoke("x=5;", nullptr, nullptr);
        ASSERT_EQUALS(5, s->getNumber("__returnValue"));

        s->invoke("x='test'", nullptr, nullptr);
        ASSERT_EQUALS("test", s->getString("__returnValue"));

        s->invoke("x='return'", nullptr, nullptr);
        ASSERT_EQUALS("return", s->getString("__returnValue"));

        s->invoke("return 'return'", nullptr, nullptr);
        ASSERT_EQUALS("return", s->getString("__returnValue"));

        s->invoke("x = ' return '", nullptr, nullptr);
        ASSERT_EQUALS(" return ", s->getString("__returnValue"));

        s->invoke("x = \" return \"", nullptr, nullptr);
        ASSERT_EQUALS(" return ", s->getString("__returnValue"));

        s->invoke("x = \"' return '\"", nullptr, nullptr);
        ASSERT_EQUALS("' return '", s->getString("__returnValue"));

        s->invoke("x = '\" return \"'", nullptr, nullptr);
        ASSERT_EQUALS("\" return \"", s->getString("__returnValue"));

        s->invoke(";return 5", nullptr, nullptr);
        ASSERT_EQUALS(5, s->getNumber("__returnValue"));

        s->invoke("String('return')", nullptr, nullptr);
        ASSERT_EQUALS("return", s->getString("__returnValue"));

        s->invoke("String(' return ')", nullptr, nullptr);
        ASSERT_EQUALS(" return ", s->getString("__returnValue"));

        s->invoke("String(\"'return\")", nullptr, nullptr);
        ASSERT_EQUALS("'return", s->getString("__returnValue"));

        s->invoke("String('\"return')", nullptr, nullptr);
        ASSERT_EQUALS("\"return", s->getString("__returnValue"));
    }
};

template <ScopeFactory scopeFactory>
class RecursiveInvoke {
public:
    static BSONObj callback(const BSONObj& args, void* data) {
        auto scope = static_cast<Scope*>(data);

        scope->invoke("x = 10;", nullptr, nullptr);

        return BSONObj();
    }

    void run() {
        std::unique_ptr<Scope> s((getGlobalScriptEngine()->*scopeFactory)());

        s->injectNative("foo", callback, s.get());
        s->invoke("var x = 1; foo();", nullptr, nullptr);
        ASSERT_EQUALS(s->getNumberInt("x"), 10);
    }
};

template <ScopeFactory scopeFactory>
class ErrorCodeFromInvoke {
public:
    void run() {
        std::unique_ptr<Scope> s((getGlobalScriptEngine()->*scopeFactory)());

        {
            bool threwException = false;
            try {
                s->invoke("\"use strict\"; x = 10;", nullptr, nullptr);
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
                s->invoke("UUID(1,2,3,4,5);", nullptr, nullptr);
            } catch (...) {
                threwException = true;

                auto status = exceptionToStatus();

                ASSERT_EQUALS(status.code(), ErrorCodes::BadValue);
            }

            ASSERT(threwException);
        }
    }
};

template <ScopeFactory scopeFactory>
class ErrorWithSidecarFromInvoke {
public:
    void run() {
        auto sidecarThrowingFunc = [](const BSONObj& args, void* data) -> BSONObj {
            uassertStatusOK(Status(ErrorExtraInfoExample(123), "foo"));
            return {};
        };

        std::unique_ptr<Scope> s((getGlobalScriptEngine()->*scopeFactory)());

        s->injectNative("foo", sidecarThrowingFunc);

        ASSERT_THROWS_WITH_CHECK(
            s->invoke("try { foo(); } catch (e) { throw e; } throw new Error(\"bar\");",
                      nullptr,
                      nullptr),
            ExceptionFor<ErrorCodes::ForTestingErrorExtraInfo>,
            [](const auto& ex) { ASSERT_EQ(ex->data, 123); });
    }
};

template <ScopeFactory scopeFactory>
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
            std::unique_ptr<Scope> s((getGlobalScriptEngine()->*scopeFactory)());
            s->setObject("unowned", unowned, true);
            s->setObject("owned", owned, true);
        }

        // After we set the flag, we should only be able to set owned
        {
            std::unique_ptr<Scope> s((getGlobalScriptEngine()->*scopeFactory)());
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

template <ScopeFactory scopeFactory>
class ConvertShardKeyToHashed {
public:
    void check(std::shared_ptr<Scope> s, const mongo::BSONObj& o) {
        s->setObject("o", o, true);
        s->invoke("return convertShardKeyToHashed(o);", nullptr, nullptr);
        const auto scopeShardKey = s->getNumber("__returnValue");

        // Wrapping to form a proper element
        const auto wrapO = BSON("" << o);
        const auto e = wrapO[""];
        const auto trueShardKey =
            mongo::BSONElementHasher::hash64(e, mongo::BSONElementHasher::DEFAULT_HASH_SEED);

        ASSERT_EQUALS(scopeShardKey, trueShardKey);
    }

    void checkNoArgs(std::shared_ptr<Scope> s) {
        s->invoke("return convertShardKeyToHashed();", nullptr, nullptr);
    }

    void checkWithExtraArg(std::shared_ptr<Scope> s, const mongo::BSONObj& o, int seed) {
        s->setObject("o", o, true);
        s->invoke("return convertShardKeyToHashed(o, 1);", nullptr, nullptr);
    }

    void run() {
        std::shared_ptr<Scope> s((getGlobalScriptEngine()->*scopeFactory)());
        shell_utils::installShellUtils(*s);

        // Check a few elementary objects
        check(s, BSON("" << 1));
        check(s, BSON("" << 10.0));
        check(s, BSON("" << "Shardy"));
        check(s, BSON("" << BSON_ARRAY(1 << 2 << 3)));
        check(s, BSON("" << mongo::BSONType::null));
        check(s, BSON("" << mongo::BSONObj()));
        check(s,
              BSON("A" << 1 << "B"
                       << "Shardy"));

        ASSERT_THROWS(checkNoArgs(s), mongo::DBException);
        ASSERT_THROWS(checkWithExtraArg(s, BSON("" << 10.0), 0), mongo::DBException);
    }
};

/**
 * A basic async test to make sure that async works and doesn't break
 */
template <ScopeFactory scopeFactory>
class BasicAsyncJS {
public:
    void run() {
        std::unique_ptr<Scope> scope((getGlobalScriptEngine()->*scopeFactory)());

        scope->setNumber("x", 0);
        /* The async code will get run after the return, so
         * 0 should be returned. Immediately after the return is
         * evaluated the function within the then() will be executed,
         * setting x to 28. */
        scope->invoke(
            "let f = async function() {  return 28; };"
            "f().then(function(y){ x = y; });"
            "return x;",
            nullptr,
            nullptr);
        ASSERT(0 == scope->getNumber("__returnValue"));
        /* When we return x the second time the value has been updated
         * by the async function */
        scope->invoke("return x;", nullptr, nullptr);
        ASSERT(28 == scope->getNumber("__returnValue"));
    }
};

class All : public unittest::OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("js") {}

    template <ScopeFactory scopeFactory>
    void setupTestsWithScopeFactory() {
        add<BuiltinTests<scopeFactory>>();
        add<BasicScope<scopeFactory>>();
        add<ResetScope<scopeFactory>>();
        add<FalseTests<scopeFactory>>();
        add<SimpleFunctions<scopeFactory>>();
        add<ExecTimeout<scopeFactory>>();
        add<ExecNoTimeout<scopeFactory>>();
        add<InvokeTimeout<scopeFactory>>();
        add<SleepInterruption<scopeFactory>>();
        add<InvokeNoTimeout<scopeFactory>>();

        add<ObjectMapping<scopeFactory>>();
        add<ObjectDecoding<scopeFactory>>();
        add<JSOIDTests<scopeFactory>>();
        add<SetImplicit<scopeFactory>>();
        add<ObjectModReadonlyTests<scopeFactory>>();
        add<OtherJSTypes<scopeFactory>>();
        add<SpecialDBTypes<scopeFactory>>();
        add<TypeConservation<scopeFactory>>();
        add<NumberLong<scopeFactory>>();
        add<NumberLong2<scopeFactory>>();

        add<NumberDecimal<scopeFactory>>();
        add<NumberDecimalGetFromScope<scopeFactory>>();
        add<NumberDecimalBigObject<scopeFactory>>();

        add<MaxTimestamp<scopeFactory>>();
        add<RenameTest<scopeFactory>>();

        add<WeirdObjects<scopeFactory>>();
        add<BinDataType<scopeFactory>>();

        add<VarTests<scopeFactory>>();
        add<Speed1<scopeFactory>>();
        add<Utf8Check<scopeFactory>>();
        add<ScopeOut<scopeFactory>>();
        add<NovelNaN<scopeFactory>>();
        add<NoReturnSpecified<scopeFactory>>();

        add<RecursiveInvoke<scopeFactory>>();
        add<ErrorCodeFromInvoke<scopeFactory>>();
        add<ErrorWithSidecarFromInvoke<scopeFactory>>();
        add<RequiresOwnedObjects<scopeFactory>>();
        add<ConvertShardKeyToHashed<scopeFactory>>();

        add<BasicAsyncJS<scopeFactory>>();
    }

    void setupTests() override {
        setupTestsWithScopeFactory<&ScriptEngine::newScope>();
        setupTestsWithScopeFactory<&ScriptEngine::newScopeForCurrentThread>();
    }
};

unittest::OldStyleSuiteInitializer<All> myall;

}  // namespace JSTests
}  // namespace mongo
