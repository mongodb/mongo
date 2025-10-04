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

/**
 * Tests for jsobj.{h,cpp} code
 */


#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_comparator_interface_base.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonelement_comparator_interface.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobj_comparator.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/dotted_path/dotted_path_support.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/query/bson/bson_helper.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/logv2/log.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/allocator.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/embedded_builder.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <sys/types.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {

namespace dps = ::mongo::bson;

namespace {

enum FieldCompareResult {
    LEFT_SUBFIELD = -2,
    LEFT_BEFORE = -1,
    SAME = 0,
    RIGHT_BEFORE = 1,
    RIGHT_SUBFIELD = 2
};

}  // namespace

typedef std::map<std::string, BSONElement> BSONMap;
BSONMap bson2map(const BSONObj& obj) {
    BSONMap m;
    BSONObjIterator it(obj);
    while (it.more()) {
        BSONElement e = it.next();
        m[e.fieldName()] = e;
    }
    return m;
}

void dotted2nested(BSONObjBuilder& b, const BSONObj& obj) {
    // use map to sort fields
    BSONMap sorted = bson2map(obj);
    EmbeddedBuilder eb(&b);
    for (BSONMap::const_iterator it = sorted.begin(); it != sorted.end(); ++it) {
        eb.appendAs(it->second, it->first);
    }
    eb.done();
}

// {a.b:1} -> {a: {b:1}}
BSONObj dotted2nested(const BSONObj& obj) {
    BSONObjBuilder b;
    dotted2nested(b, obj);
    return b.obj();
}

// {a: {b:1}} -> {a.b:1}
void nested2dotted(BSONObjBuilder& b, const BSONObj& obj, const std::string& base = "") {
    BSONObjIterator it(obj);
    while (it.more()) {
        BSONElement e = it.next();
        if (e.type() == BSONType::object) {
            std::string newbase = base + e.fieldName() + ".";
            nested2dotted(b, e.embeddedObject(), newbase);
        } else {
            std::string newbase = base + e.fieldName();
            b.appendAs(e, newbase);
        }
    }
}

BSONObj nested2dotted(const BSONObj& obj) {
    BSONObjBuilder b;
    nested2dotted(b, obj);
    return b.obj();
}

FieldCompareResult compareDottedFieldNames(const std::string& l,
                                           const std::string& r,
                                           const str::LexNumCmp& cmp) {
    static int maxLoops = 1024 * 1024;

    size_t lstart = 0;
    size_t rstart = 0;

    for (int i = 0; i < maxLoops; i++) {
        size_t a = l.find('.', lstart);
        size_t b = r.find('.', rstart);

        size_t lend = a == std::string::npos ? l.size() : a;
        size_t rend = b == std::string::npos ? r.size() : b;

        const std::string& c = l.substr(lstart, lend - lstart);
        const std::string& d = r.substr(rstart, rend - rstart);

        int x = cmp.cmp(c.c_str(), d.c_str());

        if (x < 0)
            return LEFT_BEFORE;
        if (x > 0)
            return RIGHT_BEFORE;

        lstart = lend + 1;
        rstart = rend + 1;

        if (lstart >= l.size()) {
            if (rstart >= r.size())
                return SAME;
            return RIGHT_SUBFIELD;
        }
        if (rstart >= r.size())
            return LEFT_SUBFIELD;
    }

    LOGV2(22493,
          "compareDottedFieldNames ERROR  l: {l} r: {r}  TOO MANY LOOPS",
          "l"_attr = l,
          "r"_attr = r);
    MONGO_verify(0);
    return SAME;  // will never get here
}

namespace JsobjTests {

class BufBuilderBasic {
public:
    void run() {
        {
            BufBuilder b(0);
            b.appendCStr("foo");
            ASSERT_EQUALS(4, b.len());
            ASSERT(strcmp("foo", b.buf()) == 0);
        }
        {
            mongo::StackBufBuilder b;
            b.appendCStr("foo");
            ASSERT_EQUALS(4, b.len());
            ASSERT(strcmp("foo", b.buf()) == 0);
        }
    }
};

class BufBuilderReallocLimit {
public:
    void run() {
        BufBuilder b;
        unsigned int written = 0;
        try {
            for (; written <= mongo::BufferMaxSize + 1; ++written)
                // (re)alloc past the buffer limit
                b.appendCStr("a");
        } catch (const AssertionException&) {
        }
        // assert half of max buffer size was allocated before exception is thrown
        ASSERT(written == mongo::BufferMaxSize / 2);
    }
};

class BSONElementBasic {
public:
    void run() {
        ASSERT_EQUALS(1, BSONElement().size());

        BSONObj x;
        ASSERT_EQUALS(1, x.firstElement().size());
    }
};

namespace BSONObjTests {
class Create {
public:
    void run() {
        BSONObj b;
        ASSERT_EQUALS(0, b.nFields());
    }
};

class Base {
protected:
    static BSONObj basic(const char* name, int val) {
        BSONObjBuilder b;
        b.append(name, val);
        return b.obj();
    }
    static BSONObj basic(const char* name, std::vector<int> val) {
        BSONObjBuilder b;
        b.append(name, val);
        return b.obj();
    }
    template <class T>
    static BSONObj basic(const char* name, T val) {
        BSONObjBuilder b;
        b.append(name, val);
        return b.obj();
    }
};

class WoCompareBasic : public Base {
public:
    void run() {
        ASSERT(basic("a", 1).woCompare(basic("a", 1)) == 0);
        ASSERT(basic("a", 2).woCompare(basic("a", 1)) > 0);
        ASSERT(basic("a", 1).woCompare(basic("a", 2)) < 0);
        // field name comparison
        ASSERT(basic("a", 1).woCompare(basic("b", 1)) < 0);
    }
};

class IsPrefixOf : public Base {
public:
    void run() {
        SimpleBSONElementComparator eltCmp;
        {
            BSONObj k = BSON("x" << 1);
            ASSERT(!k.isPrefixOf(BSON("a" << 1), eltCmp));
            ASSERT(k.isPrefixOf(BSON("x" << 1), eltCmp));
            ASSERT(k.isPrefixOf(BSON("x" << 1 << "a" << 1), eltCmp));
            ASSERT(!k.isPrefixOf(BSON("a" << 1 << "x" << 1), eltCmp));
        }
        {
            BSONObj k = BSON("x" << 1 << "y" << 1);
            ASSERT(!k.isPrefixOf(BSON("x" << 1), eltCmp));
            ASSERT(!k.isPrefixOf(BSON("x" << 1 << "z" << 1), eltCmp));
            ASSERT(k.isPrefixOf(BSON("x" << 1 << "y" << 1), eltCmp));
            ASSERT(k.isPrefixOf(BSON("x" << 1 << "y" << 1 << "z" << 1), eltCmp));
        }
        {
            BSONObj k = BSON("x" << 1);
            ASSERT(!k.isPrefixOf(BSON("x" << "hi"), eltCmp));
            ASSERT(k.isPrefixOf(BSON("x" << 1 << "a"
                                         << "hi"),
                                eltCmp));
        }
        {
            BSONObj k = BSON("x" << 1);
            MONGO_verify(k.isFieldNamePrefixOf(BSON("x" << "hi")));
            MONGO_verify(!k.isFieldNamePrefixOf(BSON("a" << 1)));
        }
    }
};

class NumericCompareBasic : public Base {
public:
    void run() {
        ASSERT(basic("a", 1).woCompare(basic("a", 1.0)) == 0);
    }
};

class WoCompareEmbeddedObject : public Base {
public:
    void run() {
        ASSERT(basic("a", basic("b", 1)).woCompare(basic("a", basic("b", 1.0))) == 0);
        ASSERT(basic("a", basic("b", 1)).woCompare(basic("a", basic("b", 2))) < 0);
    }
};

class WoCompareEmbeddedArray : public Base {
public:
    void run() {
        std::vector<int> i;
        i.push_back(1);
        i.push_back(2);
        std::vector<double> d;
        d.push_back(1);
        d.push_back(2);
        ASSERT(basic("a", i).woCompare(basic("a", d)) == 0);
        std::vector<int> j;
        j.push_back(1);
        j.push_back(3);
        ASSERT(basic("a", i).woCompare(basic("a", j)) < 0);
    }
};

class WoCompareOrdered : public Base {
public:
    void run() {
        ASSERT(basic("a", 1).woCompare(basic("a", 1), basic("a", 1)) == 0);
        ASSERT(basic("a", 2).woCompare(basic("a", 1), basic("a", 1)) > 0);
        ASSERT(basic("a", 1).woCompare(basic("a", 2), basic("a", 1)) < 0);
        ASSERT(basic("a", 1).woCompare(basic("a", 1), basic("a", -1)) == 0);
        ASSERT(basic("a", 2).woCompare(basic("a", 1), basic("a", -1)) < 0);
        ASSERT(basic("a", 1).woCompare(basic("a", 2), basic("a", -1)) > 0);
    }
};

class WoCompareDifferentLength : public Base {
public:
    void run() {
        ASSERT(BSON("a" << 1).woCompare(BSON("a" << 1 << "b" << 1)) < 0);
        ASSERT(BSON("a" << 1 << "b" << 1).woCompare(BSON("a" << 1)) > 0);
    }
};

class MultiKeySortOrder : public Base {
public:
    void run() {
        ASSERT(BSON("x" << "a").woCompare(BSON("x" << "b")) < 0);
        ASSERT(BSON("x" << "b").woCompare(BSON("x" << "a")) > 0);

        ASSERT(BSON("x" << "a"
                        << "y"
                        << "a")
                   .woCompare(BSON("x" << "a"
                                       << "y"
                                       << "b")) < 0);
        ASSERT(BSON("x" << "a"
                        << "y"
                        << "a")
                   .woCompare(BSON("x" << "b"
                                       << "y"
                                       << "a")) < 0);
        ASSERT(BSON("x" << "a"
                        << "y"
                        << "a")
                   .woCompare(BSON("x" << "b")) < 0);

        ASSERT(BSON("x" << "c")
                   .woCompare(BSON("x" << "b"
                                       << "y"
                                       << "h")) > 0);
        ASSERT(BSON("x" << "b"
                        << "y"
                        << "b")
                   .woCompare(BSON("x" << "c")) < 0);

        BSONObj key = BSON("x" << 1 << "y" << 1);

        ASSERT(dps::compareObjectsAccordingToSort(BSON("x" << "c"),
                                                  BSON("x" << "b"
                                                           << "y"
                                                           << "h"),
                                                  key) > 0);
        ASSERT(BSON("x" << "b"
                        << "y"
                        << "b")
                   .woCompare(BSON("x" << "c"), key) < 0);

        key = BSON("" << 1 << "" << 1);

        ASSERT(dps::compareObjectsAccordingToSort(BSON("" << "c"),
                                                  BSON("" << "b"
                                                          << ""
                                                          << "h"),
                                                  key) > 0);
        ASSERT(BSON("" << "b"
                       << ""
                       << "b")
                   .woCompare(BSON("" << "c"), key) < 0);

        {
            BSONObjBuilder b;
            b.append("", "c");
            b.appendNull("");
            BSONObj o = b.obj();
            ASSERT(dps::compareObjectsAccordingToSort(o,
                                                      BSON("" << "b"
                                                              << ""
                                                              << "h"),
                                                      key) > 0);
            ASSERT(dps::compareObjectsAccordingToSort(BSON("" << "b"
                                                              << ""
                                                              << "h"),
                                                      o,
                                                      key) < 0);
        }

        ASSERT(BSON("" << "a").woCompare(BSON("" << "a"
                                                 << ""
                                                 << "c")) < 0);
        {
            BSONObjBuilder b;
            b.append("", "a");
            b.appendNull("");
            ASSERT(b.obj().woCompare(BSON("" << "a"
                                             << ""
                                             << "c")) < 0);  // SERVER-282
        }
    }
};

class Nan : public Base {
public:
    void run() {
        double inf = std::numeric_limits<double>::infinity();
        double nan = std::numeric_limits<double>::quiet_NaN();
        double nan2 = std::numeric_limits<double>::signaling_NaN();
        ASSERT(std::isnan(nan));
        ASSERT(std::isnan(nan2));
        ASSERT(!std::isnan(inf));

        ASSERT(BSON("a" << inf).woCompare(BSON("a" << inf)) == 0);
        ASSERT(BSON("a" << inf).woCompare(BSON("a" << 1)) > 0);
        ASSERT(BSON("a" << 1).woCompare(BSON("a" << inf)) < 0);

        ASSERT(BSON("a" << nan).woCompare(BSON("a" << nan)) == 0);
        ASSERT(BSON("a" << nan).woCompare(BSON("a" << 1)) < 0);

        ASSERT(BSON("a" << nan).woCompare(BSON("a" << 5000000000LL)) < 0);

        ASSERT(BSON("a" << 1).woCompare(BSON("a" << nan)) > 0);

        ASSERT(BSON("a" << nan2).woCompare(BSON("a" << nan2)) == 0);
        ASSERT(BSON("a" << nan2).woCompare(BSON("a" << 1)) < 0);
        ASSERT(BSON("a" << 1).woCompare(BSON("a" << nan2)) > 0);

        ASSERT(BSON("a" << inf).woCompare(BSON("a" << nan)) > 0);
        ASSERT(BSON("a" << inf).woCompare(BSON("a" << nan2)) > 0);
        ASSERT(BSON("a" << nan).woCompare(BSON("a" << nan2)) == 0);
    }
};

class AsTempObj {
public:
    void run() {
        {
            BSONObjBuilder bb;
            bb << "a" << 1;
            BSONObj tmp = bb.asTempObj();
            ASSERT(tmp.objsize() == 4 + (1 + 2 + 4) + 1);
            ASSERT(validateBSON(tmp).isOK());
            ASSERT(tmp.hasField("a"));
            ASSERT(!tmp.hasField("b"));
            ASSERT_BSONOBJ_EQ(tmp, BSON("a" << 1));

            bb << "b" << 2;
            BSONObj obj = bb.obj();
            ASSERT_EQUALS(obj.objsize(), 4 + (1 + 2 + 4) + (1 + 2 + 4) + 1);
            ASSERT(validateBSON(obj).isOK());
            ASSERT(obj.hasField("a"));
            ASSERT(obj.hasField("b"));
            ASSERT_BSONOBJ_EQ(obj, BSON("a" << 1 << "b" << 2));
        }
        {
            BSONObjBuilder bb;
            bb << "a" << GT << 1;
            BSONObj tmp = bb.asTempObj();
            ASSERT(tmp.objsize() == 4 + (1 + 2 + (4 + 1 + 4 + 4 + 1)) + 1);
            ASSERT(validateBSON(tmp).isOK());
            ASSERT(tmp.hasField("a"));
            ASSERT(!tmp.hasField("b"));
            ASSERT_BSONOBJ_EQ(tmp, BSON("a" << BSON("$gt" << 1)));

            bb << "b" << LT << 2;
            BSONObj obj = bb.obj();
            ASSERT(obj.objsize() ==
                   4 + (1 + 2 + (4 + 1 + 4 + 4 + 1)) + (1 + 2 + (4 + 1 + 4 + 4 + 1)) + 1);
            ASSERT(validateBSON(obj).isOK());
            ASSERT(obj.hasField("a"));
            ASSERT(obj.hasField("b"));
            ASSERT_BSONOBJ_EQ(obj, BSON("a" << BSON("$gt" << 1) << "b" << BSON("$lt" << 2)));
        }
        {
            BSONObjBuilder bb(32);
            bb << "a" << 1;
            BSONObj tmp = bb.asTempObj();
            ASSERT(tmp.objsize() == 4 + (1 + 2 + 4) + 1);
            ASSERT(validateBSON(tmp).isOK());
            ASSERT(tmp.hasField("a"));
            ASSERT(!tmp.hasField("b"));
            ASSERT_BSONOBJ_EQ(tmp, BSON("a" << 1));

            // force a realloc
            BSONArrayBuilder arr;
            for (int i = 0; i < 10000; i++) {
                arr << i;
            }
            bb << "b" << arr.arr();
            BSONObj obj = bb.obj();
            ASSERT(validateBSON(obj).isOK());
            ASSERT(obj.hasField("a"));
            ASSERT(obj.hasField("b"));
        }
    }
};

struct AppendNumber {
    void run() {
        BSONObjBuilder b;
        b.appendNumber("a", 5);
        b.appendNumber("b", 5.5);
        b.appendNumber("c", (1024LL * 1024 * 1024) - 1);
        b.appendNumber("d", (1024LL * 1024 * 1024 * 1024) - 1);
        b.appendNumber("e", 1024LL * 1024 * 1024 * 1024 * 1024 * 1024);
        b.appendNumber("f", mongo::Decimal128("1"));

        BSONObj o = b.obj();

        ASSERT(o["a"].type() == BSONType::numberInt);
        ASSERT(o["b"].type() == BSONType::numberDouble);
        ASSERT(o["c"].type() == BSONType::numberInt);
        ASSERT(o["d"].type() == BSONType::numberLong);
        ASSERT(o["e"].type() == BSONType::numberLong);
        ASSERT(o["f"].type() == BSONType::numberDecimal);
    }
};


class ToStringNumber {
public:
    void run() {
        BSONObjBuilder b;
        b.append("a", (int)4);
        b.append("b", (double)5);
        b.append("c", (long long)6);

        b.append("d", 123.456789123456789123456789123456789);
        b.append("e", 123456789.123456789123456789123456789);
        b.append("f", 1234567891234567891234.56789123456789);

        b.append("g", -123.456);

        b.append("h", 0.0);
        b.append("i", -0.0);

        BSONObj x = b.obj();

        ASSERT_EQUALS("4", x["a"].toString(false, true));
        ASSERT_EQUALS("5.0", x["b"].toString(false, true));
        ASSERT_EQUALS("6", x["c"].toString(false, true));

        ASSERT_EQUALS("123.4567891234568", x["d"].toString(false, true));
        ASSERT_EQUALS("123456789.1234568", x["e"].toString(false, true));
        // windows and *nix are different - TODO, work around for test or not bother?
        // ASSERT_EQUALS( "1.234567891234568e+21" , x["f"].toString( false , true ) );

        ASSERT_EQUALS("-123.456", x["g"].toString(false, true));

        ASSERT_EQUALS("0.0", x["h"].toString(false, true));
        ASSERT_EQUALS("-0.0", x["i"].toString(false, true));
    }
};

class NullString {
public:
    void run() {
        {
            BSONObjBuilder b;
            const char x[] = {'a', 0, 'b', 0};
            b.append("field", x, 4);
            b.append("z", true);
            BSONObj B = b.obj();
            // cout << B.toString() << endl;

            BSONObjBuilder a;
            const char xx[] = {'a', 0, 'c', 0};
            a.append("field", xx, 4);
            a.append("z", true);
            BSONObj A = a.obj();

            BSONObjBuilder c;
            const char xxx[] = {'a', 0, 'c', 0, 0};
            c.append("field", xxx, 5);
            c.append("z", true);
            BSONObj C = c.obj();

            // test that nulls are ok within bson std::strings
            ASSERT_BSONOBJ_NE(A, B);
            ASSERT_BSONOBJ_GT(A, B);

            ASSERT_BSONOBJ_NE(B, C);
            ASSERT_BSONOBJ_GT(C, B);

            // check iteration is ok
            ASSERT(B["z"].Bool() && A["z"].Bool() && C["z"].Bool());
        }

        BSONObjBuilder b;
        b.append("a", "a\0b", 4);
        std::string z("a\0b", 3);
        b.append("b", z);
        b.appendAs(b.asTempObj()["a"], "c");
        BSONObj o = b.obj();

        std::stringstream ss;
        ss << 'a' << '\0' << 'b';

        ASSERT_EQUALS(o["a"].valuestrsize(), 3 + 1);
        ASSERT_EQUALS(o["a"].str(), ss.str());

        ASSERT_EQUALS(o["b"].valuestrsize(), 3 + 1);
        ASSERT_EQUALS(o["b"].str(), ss.str());

        ASSERT_EQUALS(o["c"].valuestrsize(), 3 + 1);
        ASSERT_EQUALS(o["c"].str(), ss.str());
    }
};

class AppendAs {
public:
    void run() {
        BSONObjBuilder b;
        {
            BSONObj foo = BSON("foo" << 1);
            b.appendAs(foo.firstElement(), "bar");
        }
        ASSERT_BSONOBJ_EQ(BSON("bar" << 1), b.done());
    }
};

class ToStringRecursionDepth {
public:
    // create a nested BSON object with the specified recursion depth
    BSONObj recursiveBSON(int depth) {
        BSONObjBuilder b;
        if (depth == 0) {
            b << "name"
              << "Joe";
            return b.obj();
        }
        b.append("test", recursiveBSON(depth - 1));
        return b.obj();
    }

    void run() {
        BSONObj nestedBSON;
        StringBuilder s;
        std::string nestedBSONString;
        size_t found;

        // recursion depth one less than max allowed-- do not shorten the string
        nestedBSON = recursiveBSON(BSONObj::maxToStringRecursionDepth - 1);
        nestedBSON.toString(s, true, false);
        nestedBSONString = s.str();
        found = nestedBSONString.find("...");
        // did not find the "..." pattern
        ASSERT_EQUALS(found != std::string::npos, false);

        // recursion depth is equal to max allowed  -- do not shorten the string
        nestedBSON = recursiveBSON(BSONObj::maxToStringRecursionDepth);
        nestedBSON.toString(s, true, false);
        nestedBSONString = s.str();
        found = nestedBSONString.find("...");
        // did not find the "..." pattern
        ASSERT_EQUALS(found != std::string::npos, false);

        // recursion depth - one greater than max allowed -- shorten the string
        nestedBSON = recursiveBSON(BSONObj::maxToStringRecursionDepth + 1);
        nestedBSON.toString(s, false, false);
        nestedBSONString = s.str();
        found = nestedBSONString.find("...");
        // found the "..." pattern
        ASSERT_EQUALS(found != std::string::npos, true);

        /* recursion depth - one greater than max allowed but with full=true
         * should fail with an assertion
         */
        nestedBSON = recursiveBSON(BSONObj::maxToStringRecursionDepth + 1);
        ASSERT_THROWS(nestedBSON.toString(s, false, true), AssertionException);
    }
};

class StringWithNull {
public:
    void run() {
        const std::string input = std::string("a") + '\0' + 'b';
        ASSERT_EQUALS(input.size(), 3U);

        BSONObj obj = BSON("str" << input);
        const std::string output = obj.firstElement().String();
        ASSERT_EQUALS(str::escape(output), str::escape(input));  // for better failure output
        ASSERT_EQUALS(output, input);
    }
};

namespace Validation {

class Base {
public:
    virtual ~Base() {}
    void run() {
        ASSERT(validateBSON(valid()).isOK());
        ASSERT(!validateBSON(invalid()).isOK());
    }

protected:
    virtual BSONObj valid() const {
        return BSONObj();
    }
    virtual BSONObj invalid() const {
        return BSONObj();
    }
    static char get(const BSONObj& o, int i) {
        return o.objdata()[i];
    }
    static void set(BSONObj& o, int i, char c) {
        const_cast<char*>(o.objdata())[i] = c;
    }
};

class BadType : public Base {
    BSONObj valid() const override {
        return fromjson("{\"a\":1}");
    }
    BSONObj invalid() const override {
        BSONObj ret = valid();
        set(ret, 4, 50);
        return ret;
    }
};

class EooBeforeEnd : public Base {
    BSONObj valid() const override {
        return fromjson("{\"a\":1}");
    }
    BSONObj invalid() const override {
        BSONObj ret = valid();
        // (first byte of size)++
        set(ret, 0, get(ret, 0) + 1);
        // re-read size for BSONObj::details
        return ret.copy();
    }
};

class Undefined : public Base {
public:
    void run() {
        BSONObjBuilder b;
        b.appendNull("a");
        BSONObj o = b.done();
        set(o, 4, stdx::to_underlying(mongo::BSONType::undefined));
        ASSERT(validateBSON(o).isOK());
    }
};

class TotalSizeTooSmall : public Base {
    BSONObj valid() const override {
        return fromjson("{\"a\":1}");
    }
    BSONObj invalid() const override {
        BSONObj ret = valid();
        // (first byte of size)--
        set(ret, 0, get(ret, 0) - 1);
        // re-read size for BSONObj::details
        return ret.copy();
    }
};

class EooMissing : public Base {
    BSONObj valid() const override {
        return fromjson("{\"a\":1}");
    }
    BSONObj invalid() const override {
        BSONObj ret = valid();
        set(ret, ret.objsize() - 1, (char)0xff);
        // (first byte of size)--
        set(ret, 0, get(ret, 0) - 1);
        // re-read size for BSONObj::details
        return ret.copy();
    }
};

class WrongStringSize : public Base {
    BSONObj valid() const override {
        return fromjson("{\"a\":\"b\"}");
    }
    BSONObj invalid() const override {
        BSONObj ret = valid();
        auto val = ret.firstElement().valueStringData();
        ASSERT_EQUALS(val, "b"_sd);
        auto d = const_cast<char*>(val.data());
        ASSERT_EQUALS(d[1], 0);
        d[1] = 1;
        return ret.copy();
    }
};

class ZeroStringSize : public Base {
    BSONObj valid() const override {
        return fromjson("{\"a\":\"b\"}");
    }
    BSONObj invalid() const override {
        BSONObj ret = valid();
        set(ret, 7, 0);
        return ret;
    }
};

class NegativeStringSize : public Base {
    BSONObj valid() const override {
        return fromjson("{\"a\":\"b\"}");
    }
    BSONObj invalid() const override {
        BSONObj ret = valid();
        set(ret, 10, -100);
        return ret;
    }
};

class WrongSubobjectSize : public Base {
    BSONObj valid() const override {
        return fromjson("{\"a\":{\"b\":1}}");
    }
    BSONObj invalid() const override {
        BSONObj ret = valid();
        set(ret, 0, get(ret, 0) + 1);
        set(ret, 7, get(ret, 7) + 1);
        return ret.copy();
    }
};

class WrongDbrefNsSize : public Base {
    BSONObj valid() const override {
        return fromjson("{ \"a\": Dbref( \"b\", \"ffffffffffffffffffffffff\" ) }");
    }
    BSONObj invalid() const override {
        BSONObj ret = valid();
        set(ret, 0, get(ret, 0) + 1);
        set(ret, 7, get(ret, 7) + 1);
        return ret.copy();
    };
};

class NoFieldNameEnd : public Base {
    BSONObj valid() const override {
        return fromjson("{\"a\":1}");
    }
    BSONObj invalid() const override {
        BSONObj ret = valid();
        memset(const_cast<char*>(ret.objdata()) + 5, 0xff, ret.objsize() - 5);
        return ret;
    }
};

class BadRegex : public Base {
    BSONObj valid() const override {
        return fromjson("{\"a\":/c/i}");
    }
    BSONObj invalid() const override {
        BSONObj ret = valid();
        memset(const_cast<char*>(ret.objdata()) + 7, 0xff, ret.objsize() - 7);
        return ret;
    }
};

class BadRegexOptions : public Base {
    BSONObj valid() const override {
        return fromjson("{\"a\":/c/i}");
    }
    BSONObj invalid() const override {
        BSONObj ret = valid();
        memset(const_cast<char*>(ret.objdata()) + 9, 0xff, ret.objsize() - 9);
        return ret;
    }
};

class CodeWScopeBase : public Base {
    BSONObj valid() const override {
        BSONObjBuilder b;
        BSONObjBuilder scope;
        scope.append("a", "b");
        b.appendCodeWScope("c", "d", scope.done());
        return b.obj();
    }
    BSONObj invalid() const override {
        BSONObj ret = valid();
        modify(ret);
        return ret;
    }

protected:
    virtual void modify(BSONObj& o) const = 0;
};

class CodeWScopeSmallSize : public CodeWScopeBase {
    void modify(BSONObj& o) const override {
        set(o, 7, 7);
    }
};

class CodeWScopeZeroStrSize : public CodeWScopeBase {
    void modify(BSONObj& o) const override {
        set(o, 11, 0);
    }
};

class CodeWScopeSmallStrSize : public CodeWScopeBase {
    void modify(BSONObj& o) const override {
        set(o, 11, 1);
    }
};

class CodeWScopeNoSizeForObj : public CodeWScopeBase {
    void modify(BSONObj& o) const override {
        set(o, 7, 13);
    }
};

class CodeWScopeSmallObjSize : public CodeWScopeBase {
    void modify(BSONObj& o) const override {
        set(o, 17, 1);
    }
};

class CodeWScopeBadObject : public CodeWScopeBase {
    void modify(BSONObj& o) const override {
        set(o, 21, stdx::to_underlying(BSONType::jsTypeMax) + 1);
    }
};

class NoSize {
public:
    NoSize(BSONType type) : type_(type) {}
    void run() {
        const char data[] = {0x07, 0x00, 0x00, 0x00, char(type_), 'a', 0x00};
        BSONObj o(data);
        ASSERT(!validateBSON(o).isOK());
    }

private:
    BSONType type_;
};

}  // namespace Validation

}  // namespace BSONObjTests

namespace OIDTests {

class init1 {
public:
    void run() {
        OID a;
        OID b;

        a.init();
        b.init();

        ASSERT(a != b);
    }
};

class initParse1 {
public:
    void run() {
        OID a;
        OID b;

        a.init();
        b.init(a.toString());

        ASSERT(a == b);
    }
};

class append {
public:
    void run() {
        BSONObjBuilder b;
        b.appendOID("a", nullptr);
        b.appendOID("b", nullptr, false);
        b.appendOID("c", nullptr, true);
        BSONObj o = b.obj();
        ASSERT(o["a"].__oid().toString() == "000000000000000000000000");
        ASSERT(o["b"].__oid().toString() == "000000000000000000000000");
        ASSERT(o["c"].__oid().toString() != "000000000000000000000000");
    }
};

class increasing {
public:
    BSONObj g() {
        BSONObjBuilder b;
        b.appendOID("_id", nullptr, true);
        return b.obj();
    }
    void run() {
        BSONObj a = g();
        BSONObj b = g();

        ASSERT(a.woCompare(b) < 0);

        // yes, there is a 1/1000 chance this won't increase time(0)
        // and therefore inaccurately say the function is behaving
        // buf if its broken, it will fail 999/1000, so i think that's good enough
        sleepsecs(1);
        BSONObj c = g();
        ASSERT(a.woCompare(c) < 0);
    }
};

class ToDate {
public:
    void run() {
        OID oid;
        const Date_t base(Date_t::now());
        oid.init(base);

        ASSERT_EQUALS(base.toMillisSinceEpoch() / 1000, oid.asDateT().toMillisSinceEpoch() / 1000);
        ASSERT_EQUALS(base.toTimeT(), oid.asTimeT());
    }
};

class FromDate {
public:
    void run() {
        OID min, oid, max;
        oid.init();
        const Date_t now = oid.asDateT();
        min.init(now, false);
        max.init(now, true);

        ASSERT_EQUALS(oid.asDateT(), now);
        ASSERT_EQUALS(min.asDateT(), now);
        ASSERT_EQUALS(max.asDateT(), now);
        ASSERT_BSONOBJ_LT(BSON("" << min), BSON("" << oid));
        ASSERT_BSONOBJ_GT(BSON("" << max), BSON("" << oid));
    }
};

}  // namespace OIDTests


namespace ValueStreamTests {

class LabelBase {
public:
    virtual ~LabelBase() {}
    void run() {
        ASSERT(!expected().woCompare(actual()));
    }

protected:
    virtual BSONObj expected() = 0;
    virtual BSONObj actual() = 0;
};

class LabelBasic : public LabelBase {
    BSONObj expected() override {
        return BSON("a" << (BSON("$gt" << 1)));
    }
    BSONObj actual() override {
        return BSON("a" << GT << 1);
    }
};

class LabelShares : public LabelBase {
    BSONObj expected() override {
        return BSON("z" << "q"
                        << "a" << (BSON("$gt" << 1)) << "x"
                        << "p");
    }
    BSONObj actual() override {
        return BSON("z" << "q"
                        << "a" << GT << 1 << "x"
                        << "p");
    }
};

class LabelDouble : public LabelBase {
    BSONObj expected() override {
        return BSON("a" << (BSON("$gt" << 1 << "$lte"
                                       << "x")));
    }
    BSONObj actual() override {
        return BSON("a" << GT << 1 << LTE << "x");
    }
};

class LabelDoubleShares : public LabelBase {
    BSONObj expected() override {
        return BSON("z" << "q"
                        << "a"
                        << (BSON("$gt" << 1 << "$lte"
                                       << "x"))
                        << "x"
                        << "p");
    }
    BSONObj actual() override {
        return BSON("z" << "q"
                        << "a" << GT << 1 << LTE << "x"
                        << "x"
                        << "p");
    }
};

class LabelSize : public LabelBase {
    BSONObj expected() override {
        return BSON("a" << BSON("$size" << 4));
    }
    BSONObj actual() override {
        return BSON("a" << mongo::BSIZE << 4);
    }
};

class LabelMulti : public LabelBase {
    BSONObj expected() override {
        return BSON("z" << "q"
                        << "a"
                        << BSON("$gt" << 1 << "$lte"
                                      << "x")
                        << "b"
                        << BSON("$ne" << 1 << "$ne"
                                      << "f"
                                      << "$ne" << 22.3)
                        << "x"
                        << "p");
    }
    BSONObj actual() override {
        return BSON("z" << "q"
                        << "a" << GT << 1 << LTE << "x"
                        << "b" << NE << 1 << NE << "f" << NE << 22.3 << "x"
                        << "p");
    }
};
class LabelishOr : public LabelBase {
    BSONObj expected() override {
        return BSON("$or" << BSON_ARRAY(BSON("a" << BSON("$gt" << 1 << "$lte"
                                                               << "x"))
                                        << BSON("b" << BSON("$ne" << 1 << "$ne"
                                                                  << "f"
                                                                  << "$ne" << 22.3))
                                        << BSON("x" << "p")));
    }
    BSONObj actual() override {
        return BSON(OR(BSON("a" << GT << 1 << LTE << "x"),
                       BSON("b" << NE << 1 << NE << "f" << NE << 22.3),
                       BSON("x" << "p")));
    }
};

class ElementAppend {
public:
    void run() {
        BSONObj a = BSON("a" << 17);
        BSONObj b = BSON("b" << a["a"]);
        ASSERT_EQUALS(BSONType::numberInt, a["a"].type());
        ASSERT_EQUALS(BSONType::numberInt, b["b"].type());
        ASSERT_EQUALS(17, b["b"].number());

        // Append an element onto a label.
        ASSERT_BSONOBJ_EQ(BSON("b" << GT << a["a"]),  //
                          BSON("b" << BSON("$gt" << 17)));
    }
};

class AllTypes {
public:
    void run() {
        // These are listed in order of BSONType

        ASSERT_EQUALS(objTypeOf(MINKEY), BSONType::minKey);
        ASSERT_EQUALS(arrTypeOf(MINKEY), BSONType::minKey);

        // EOO not valid in middle of BSONObj

        ASSERT_EQUALS(objTypeOf(1.0), BSONType::numberDouble);
        ASSERT_EQUALS(arrTypeOf(1.0), BSONType::numberDouble);

        ASSERT_EQUALS(objTypeOf(""), BSONType::string);
        ASSERT_EQUALS(arrTypeOf(""), BSONType::string);
        ASSERT_EQUALS(objTypeOf(std::string()), BSONType::string);
        ASSERT_EQUALS(arrTypeOf(std::string()), BSONType::string);
        ASSERT_EQUALS(objTypeOf(StringData("")), BSONType::string);
        ASSERT_EQUALS(arrTypeOf(StringData("")), BSONType::string);

        ASSERT_EQUALS(objTypeOf(BSONObj()), BSONType::object);
        ASSERT_EQUALS(arrTypeOf(BSONObj()), BSONType::object);

        ASSERT_EQUALS(objTypeOf(BSONArray()), BSONType::array);
        ASSERT_EQUALS(arrTypeOf(BSONArray()), BSONType::array);

        ASSERT_EQUALS(objTypeOf(BSONBinData("", 0, BinDataGeneral)), BSONType::binData);
        ASSERT_EQUALS(arrTypeOf(BSONBinData("", 0, BinDataGeneral)), BSONType::binData);

        ASSERT_EQUALS(objTypeOf(BSONUndefined), BSONType::undefined);
        ASSERT_EQUALS(arrTypeOf(BSONUndefined), BSONType::undefined);

        ASSERT_EQUALS(objTypeOf(OID()), BSONType::oid);
        ASSERT_EQUALS(arrTypeOf(OID()), BSONType::oid);

        ASSERT_EQUALS(objTypeOf(true), BSONType::boolean);
        ASSERT_EQUALS(arrTypeOf(true), BSONType::boolean);

        ASSERT_EQUALS(objTypeOf(Date_t()), BSONType::date);
        ASSERT_EQUALS(arrTypeOf(Date_t()), BSONType::date);

        ASSERT_EQUALS(objTypeOf(BSONNULL), BSONType::null);
        ASSERT_EQUALS(arrTypeOf(BSONNULL), BSONType::null);

        ASSERT_EQUALS(objTypeOf(BSONRegEx("", "")), BSONType::regEx);
        ASSERT_EQUALS(arrTypeOf(BSONRegEx("", "")), BSONType::regEx);

        ASSERT_EQUALS(objTypeOf(BSONDBRef("", OID())), BSONType::dbRef);
        ASSERT_EQUALS(arrTypeOf(BSONDBRef("", OID())), BSONType::dbRef);

        ASSERT_EQUALS(objTypeOf(BSONCode("")), BSONType::code);
        ASSERT_EQUALS(arrTypeOf(BSONCode("")), BSONType::code);

        ASSERT_EQUALS(objTypeOf(BSONSymbol("")), BSONType::symbol);
        ASSERT_EQUALS(arrTypeOf(BSONSymbol("")), BSONType::symbol);

        ASSERT_EQUALS(objTypeOf(BSONCodeWScope("", BSONObj())), BSONType::codeWScope);
        ASSERT_EQUALS(arrTypeOf(BSONCodeWScope("", BSONObj())), BSONType::codeWScope);

        ASSERT_EQUALS(objTypeOf(1), BSONType::numberInt);
        ASSERT_EQUALS(arrTypeOf(1), BSONType::numberInt);

        ASSERT_EQUALS(objTypeOf(Timestamp()), BSONType::timestamp);
        ASSERT_EQUALS(arrTypeOf(Timestamp()), BSONType::timestamp);

        ASSERT_EQUALS(objTypeOf(1LL), BSONType::numberLong);
        ASSERT_EQUALS(arrTypeOf(1LL), BSONType::numberLong);

        ASSERT_EQUALS(objTypeOf(mongo::Decimal128("1")), BSONType::numberDecimal);
        ASSERT_EQUALS(arrTypeOf(mongo::Decimal128("1")), BSONType::numberDecimal);

        ASSERT_EQUALS(objTypeOf(MAXKEY), BSONType::maxKey);
        ASSERT_EQUALS(arrTypeOf(MAXKEY), BSONType::maxKey);
    }

    template <typename T>
    BSONType objTypeOf(const T& thing) {
        return BSON("" << thing).firstElement().type();
    }

    template <typename T>
    BSONType arrTypeOf(const T& thing) {
        return BSON_ARRAY(thing).firstElement().type();
    }
};
}  // namespace ValueStreamTests

class SubObjectBuilder {
public:
    void run() {
        BSONObjBuilder b1;
        b1.append("a", "bcd");
        BSONObjBuilder b2(b1.subobjStart("foo"));
        b2.append("ggg", 44.0);
        b2.done();
        b1.append("f", 10.0);
        BSONObj ret = b1.done();
        ASSERT(validateBSON(ret).isOK());
        ASSERT(ret.woCompare(fromjson("{a:'bcd',foo:{ggg:44},f:10}")) == 0);
    }
};

class DateBuilder {
public:
    void run() {
        BSONObj o = BSON("" << Date_t::fromMillisSinceEpoch(1234567890));
        ASSERT(o.firstElement().type() == BSONType::date);
        ASSERT(o.firstElement().date() == Date_t::fromMillisSinceEpoch(1234567890));
    }
};

class DateNowBuilder {
public:
    void run() {
        Date_t before = Date_t::now();
        BSONObj o = BSON("now" << DATENOW);
        Date_t after = Date_t::now();

        ASSERT(validateBSON(o).isOK());

        BSONElement e = o["now"];
        ASSERT(e.type() == BSONType::date);
        ASSERT(e.date() >= before);
        ASSERT(e.date() <= after);
    }
};

class TimeTBuilder {
public:
    void run() {
        Date_t aDate = Date_t::now();
        time_t aTime = aDate.toTimeT();
        BSONObjBuilder b;
        b.appendTimeT("now", aTime);
        BSONObj o = b.obj();

        ASSERT(validateBSON(o).isOK());

        BSONElement e = o["now"];
        ASSERT_EQUALS(BSONType::date, e.type());
        ASSERT_EQUALS(aTime, e.date().toTimeT());
    }
};

class MinMaxKeyBuilder {
public:
    void run() {
        BSONObj min = BSON("a" << MINKEY);
        BSONObj max = BSON("b" << MAXKEY);

        ASSERT(validateBSON(min).isOK());
        ASSERT(validateBSON(max).isOK());

        BSONElement minElement = min["a"];
        BSONElement maxElement = max["b"];
        ASSERT_EQ(minElement.type(), BSONType::minKey);
        ASSERT_EQ(maxElement.type(), BSONType::maxKey);
    }
};

class MinMaxElementTest {
public:
    BSONObj min(int t) {
        BSONObjBuilder b;
        b.appendMinForType("a", t);
        return b.obj();
    }

    BSONObj max(int t) {
        BSONObjBuilder b;
        b.appendMaxForType("a", t);
        return b.obj();
    }

    void run() {
        for (int t = 1; t < stdx::to_underlying(BSONType::jsTypeMax); t++) {
            std::stringstream ss;
            ss << "type: " << t;
            std::string s = ss.str();
            ASSERT(min(t).woCompare(max(t)) <= 0);
            ASSERT(max(t).woCompare(min(t)) >= 0);
            ASSERT(min(t).woCompare(min(t)) == 0);
            ASSERT(max(t).woCompare(max(t)) == 0);
        }
    }
};

class ComparatorTest {
public:
    BSONObj one(std::string s) {
        return BSON("x" << s);
    }
    BSONObj two(std::string x, std::string y) {
        BSONObjBuilder b;
        b.append("x", x);
        if (y.size())
            b.append("y", y);
        else
            b.appendNull("y");
        return b.obj();
    }

    void test(BSONObj order, BSONObj l, BSONObj r, bool wanted) {
        const StringDataComparator* stringComparator = nullptr;
        BSONObjComparator bsonCmp(
            order, BSONObjComparator::FieldNamesMode::kConsider, stringComparator);
        bool got = bsonCmp.makeLessThan()(l, r);
        if (got == wanted)
            return;
        std::cout << " order: " << order << " l: " << l << "r: " << r << " wanted: " << wanted
                  << " got: " << got << std::endl;
    }

    void lt(BSONObj order, BSONObj l, BSONObj r) {
        test(order, l, r, 1);
    }

    void run() {
        BSONObj s = BSON("x" << 1);
        BSONObj c = BSON("x" << 1 << "y" << 1);
        test(s, one("A"), one("B"), 1);
        test(s, one("B"), one("A"), 0);

        test(c, two("A", "A"), two("A", "B"), 1);
        test(c, two("A", "A"), two("B", "A"), 1);
        test(c, two("B", "A"), two("A", "B"), 0);

        lt(c, one("A"), two("A", "A"));
        lt(c, one("A"), one("B"));
        lt(c, two("A", ""), two("B", "A"));

        lt(c, two("B", "A"), two("C", "A"));
        lt(c, two("B", "A"), one("C"));
        lt(c, two("B", "A"), two("C", ""));
    }
};

class CompatBSON {
public:
#define JSONBSONTEST(j, s) ASSERT_EQUALS(fromjson(j).objsize(), s);
#define RAWBSONTEST(j, s) ASSERT_EQUALS(j.objsize(), s);

    void run() {
        JSONBSONTEST("{ 'x' : true }", 9);
        JSONBSONTEST("{ 'x' : null }", 8);
        JSONBSONTEST("{ 'x' : 5.2 }", 16);
        JSONBSONTEST("{ 'x' : 'eliot' }", 18);
        JSONBSONTEST("{ 'x' : 5.2 , 'y' : 'truth' , 'z' : 1.1 }", 40);
        JSONBSONTEST("{ 'a' : { 'b' : 1.1 } }", 24);
        JSONBSONTEST("{ 'x' : 5.2 , 'y' : { 'a' : 'eliot' , b : true } , 'z' : null }", 44);
        JSONBSONTEST("{ 'x' : 5.2 , 'y' : [ 'a' , 'eliot' , 'b' , true ] , 'z' : null }", 62);

        RAWBSONTEST(BSON("x" << 4), 12);
    }
};

class CompareDottedFieldNamesTest {
public:
    void t(FieldCompareResult res, const std::string& l, const std::string& r) {
        str::LexNumCmp cmp(true);
        ASSERT_EQUALS(res, compareDottedFieldNames(l, r, cmp));
        ASSERT_EQUALS(-1 * res, compareDottedFieldNames(r, l, cmp));
    }

    void run() {
        t(SAME, "x", "x");
        t(SAME, "x.a", "x.a");
        t(SAME, "x.4", "x.4");
        t(LEFT_BEFORE, "a", "b");
        t(RIGHT_BEFORE, "b", "a");
        t(LEFT_BEFORE, "x.04", "x.4");

        t(LEFT_SUBFIELD, "a.x", "a");
        t(LEFT_SUBFIELD, "a.4", "a");
    }
};

class CompareDottedArrayFieldNamesTest {
public:
    void t(FieldCompareResult res, const std::string& l, const std::string& r) {
        str::LexNumCmp cmp(false);  // Specify numeric comparison for array field names.
        ASSERT_EQUALS(res, compareDottedFieldNames(l, r, cmp));
        ASSERT_EQUALS(-1 * res, compareDottedFieldNames(r, l, cmp));
    }

    void run() {
        t(SAME, "0", "0");
        t(SAME, "1", "1");
        t(SAME, "0.1", "0.1");
        t(SAME, "0.a", "0.a");
        t(LEFT_BEFORE, "0", "1");
        t(LEFT_BEFORE, "2", "10");
        t(RIGHT_BEFORE, "1", "0");
        t(RIGHT_BEFORE, "10", "2");

        t(LEFT_SUBFIELD, "5.4", "5");
        t(LEFT_SUBFIELD, "5.x", "5");
    }
};

struct NestedDottedConversions {
    void t(const BSONObj& nest, const BSONObj& dot) {
        ASSERT_BSONOBJ_EQ(nested2dotted(nest), dot);
        ASSERT_BSONOBJ_EQ(nest, dotted2nested(dot));
    }

    void run() {
        t(BSON("a" << BSON("b" << 1)), BSON("a.b" << 1));
        t(BSON("a" << BSON("b" << 1 << "c" << 1)), BSON("a.b" << 1 << "a.c" << 1));
        t(BSON("a" << BSON("b" << 1 << "c" << 1) << "d" << 1),
          BSON("a.b" << 1 << "a.c" << 1 << "d" << 1));
        t(BSON("a" << BSON("b" << 1 << "c" << 1 << "e" << BSON("f" << 1)) << "d" << 1),
          BSON("a.b" << 1 << "a.c" << 1 << "a.e.f" << 1 << "d" << 1));
    }
};

struct BSONArrayBuilderTest {
    void run() {
        BSONObjBuilder objb;
        BSONArrayBuilder arrb;

        auto fieldNameGenerator = [i = 0]() mutable {
            return std::to_string(i++);
        };

        objb << fieldNameGenerator() << 100;
        arrb << 100;

        objb << fieldNameGenerator() << 1.0;
        arrb << 1.0;

        objb << fieldNameGenerator() << "Hello";
        arrb << "Hello";

        objb << fieldNameGenerator() << std::string("World");
        arrb << std::string("World");

        objb << fieldNameGenerator()
             << BSON("a" << 1 << "b"
                         << "foo");
        arrb << BSON("a" << 1 << "b"
                         << "foo");

        objb << fieldNameGenerator() << BSON("a" << 1)["a"];
        arrb << BSON("a" << 1)["a"];

        OID oid;
        oid.init();
        objb << fieldNameGenerator() << oid;
        arrb << oid;

        objb.appendUndefined(fieldNameGenerator());
        arrb.appendUndefined();

        objb.appendRegex(fieldNameGenerator(), "test", "imx");
        arrb.appendRegex("test", "imx");

        objb.appendBinData(fieldNameGenerator(), 4, BinDataGeneral, "wow");
        arrb.appendBinData(4, BinDataGeneral, "wow");

        objb.appendCode(fieldNameGenerator(), "function(){ return 1; }");
        arrb.appendCode("function(){ return 1; }");

        objb.appendCodeWScope(fieldNameGenerator(), "function(){ return a; }", BSON("a" << 1));
        arrb.appendCodeWScope("function(){ return a; }", BSON("a" << 1));

        time_t dt(0);
        objb.appendTimeT(fieldNameGenerator(), dt);
        arrb.appendTimeT(dt);

        Date_t date{};
        objb.appendDate(fieldNameGenerator(), date);
        arrb.appendDate(date);

        objb.append(fieldNameGenerator(), BSONRegEx("test2", "s"));
        arrb.append(BSONRegEx("test2", "s"));

        BSONObj obj = objb.obj();
        BSONArray arr = arrb.arr();

        ASSERT_BSONOBJ_EQ(obj, arr);

        BSONObj o = BSON("obj" << obj << "arr" << arr << "arr2" << BSONArray(obj) << "regex"
                               << BSONRegEx("reg", "x"));
        ASSERT_EQUALS(o["obj"].type(), BSONType::object);
        ASSERT_EQUALS(o["arr"].type(), BSONType::array);
        ASSERT_EQUALS(o["arr2"].type(), BSONType::array);
        ASSERT_EQUALS(o["regex"].type(), BSONType::regEx);
    }
};

struct ArrayMacroTest {
    void run() {
        BSONArray arr = BSON_ARRAY("hello" << 1
                                           << BSON("foo" << BSON_ARRAY("bar" << "baz"
                                                                             << "qux")));
        BSONObj obj = BSON("0" << "hello"
                               << "1" << 1 << "2"
                               << BSON("foo" << BSON_ARRAY("bar" << "baz"
                                                                 << "qux")));

        ASSERT_BSONOBJ_EQ(arr, obj);
        ASSERT_EQUALS(arr["2"].type(), BSONType::object);
        ASSERT_EQUALS(arr["2"].embeddedObject()["foo"].type(), BSONType::array);
    }
};

class bson2settest {
public:
    void run() {
        BSONObj o = BSON("z" << 1 << "a" << 2 << "m" << 3 << "c" << 4);
        BSONObjIteratorSorted i(o);
        std::stringstream ss;
        while (i.more())
            ss << i.next().fieldName();
        ASSERT_EQUALS("acmz", ss.str());

        {
            Timer t;
            for (int i = 0; i < 10000; i++) {
                BSONObjIteratorSorted j(o);
                [[maybe_unused]] int l = 0;
                while (j.more())
                    l += strlen(j.next().fieldName());
            }
            // unsigned long long tm = t.micros();
            // cout << "time: " << tm << endl;
        }

        BSONObj o2 = BSON("2" << "a"
                              << "11"
                              << "b");
        BSONObjIteratorSorted i2(o2);
        // First field in sorted order should be "11" due use of a lexical comparison.
        ASSERT_EQUALS("11", std::string(i2.next().fieldName()));
    }
};

class BSONArrayIteratorSorted {
public:
    void run() {
        BSONArrayBuilder bab;
        for (int i = 0; i < 11; ++i) {
            bab << "a";
        }
        BSONArray arr = bab.arr();
        // The sorted iterator should perform numeric comparisons and return results in the same
        // order as the unsorted iterator.
        BSONObjIterator unsorted(arr);
        mongo::BSONArrayIteratorSorted sorted(arr);
        while (unsorted.more()) {
            ASSERT(sorted.more());
            ASSERT_EQUALS(std::string(unsorted.next().fieldName()), sorted.next().fieldName());
        }
        ASSERT(!sorted.more());
    }
};

class checkForStorageTests {
public:
    void good(std::string s) {
        good(fromjson(s));
    }

    void good(BSONObj o) {
        if (o.storageValidEmbedded().isOK())
            return;
        uasserted(12528, std::string("should be ok for storage:") + o.toString());
    }

    void bad(std::string s) {
        bad(fromjson(s));
    }

    void bad(BSONObj o) {
        if (!o.storageValidEmbedded().isOK())
            return;
        uasserted(12529, std::string("should NOT be ok for storage:") + o.toString());
    }

    void run() {
        // basic docs are good
        good("{}");
        good("{x:1}");
        good("{x:{a:2}}");

        // Check for $
        bad("{x:{'$a':2}}");
        good("{'a$b':2}");
        good("{'a$': {b: 2}}");
        good("{'a$':2}");
        good("{'a $ a': 'foo'}");

        // Queries are not ok
        bad("{num: {$gt: 1}}");
        bad("{$gt: 2}");
        bad("{a : { oo: [ {$bad:1}, {good:1}] }}");
        good("{a : { oo: [ {'\\\\$good':1}, {good:1}] }}");

        // DBRef stuff -- json parser can't handle this yet
        good(BSON("a" << BSON("$ref" << "coll"
                                     << "$id" << 1)));
        good(BSON("a" << BSON("$ref" << "coll"
                                     << "$id" << 1 << "$db"
                                     << "a")));
        good(BSON("a" << BSON("$ref" << "coll"
                                     << "$id" << 1 << "stuff" << 1)));
        good(BSON("a" << BSON("$ref" << "coll"
                                     << "$id" << 1 << "$db"
                                     << "a"
                                     << "stuff" << 1)));

        bad(BSON("a" << BSON("$ref" << 1 << "$id" << 1)));
        bad(BSON("a" << BSON("$ref" << 1 << "$id" << 1 << "$db"
                                    << "a")));
        bad(BSON("a" << BSON("$ref" << "coll"
                                    << "$id" << 1 << "$db" << 1)));
        bad(BSON("a" << BSON("$ref" << "coll")));
        bad(BSON("a" << BSON("$ref" << "coll"
                                    << "$db"
                                    << "db")));
        bad(BSON("a" << BSON("$id" << 1)));
        bad(BSON("a" << BSON("$id" << 1 << "$ref"
                                   << "coll")));
        bad(BSON("a" << BSON("$ref" << "coll"
                                    << "$id" << 1 << "$hater" << 1)));
    }
};

class InvalidIDFind {
public:
    void run() {
        BSONObj x = BSON("_id" << 5 << "t" << 2);
        {
            char* crap = (char*)mongoMalloc(x.objsize());
            memcpy(crap, x.objdata(), x.objsize());
            BSONObj y(crap);
            ASSERT_BSONOBJ_EQ(x, y);
            free(crap);
        }

        {
            char* crap = (char*)mongoMalloc(x.objsize());
            memcpy(crap, x.objdata(), x.objsize());
            int* foo = (int*)crap;
            foo[0] = 1'231'231'231;
            int state = 0;
            try {
                BSONObj y(crap);
                state = 1;
            } catch (std::exception& e) {
                state = 2;
                ASSERT(strstr(e.what(), "_id: 5") != nullptr);
            }
            free(crap);
            ASSERT_EQUALS(2, state);
        }
    }
};

class ElementSetTest {
public:
    void run() {
        BSONObj x = BSON("a" << 1 << "b" << 1 << "c" << 2);
        BSONElement a = x["a"];
        BSONElement b = x["b"];
        BSONElement c = x["c"];
        // cout << "c: " << c << endl;
        ASSERT(a.woCompare(b) != 0);
        ASSERT(a.woCompare(b, false) == 0);

        BSONElementSet s;
        s.insert(a);
        ASSERT_EQUALS(1U, s.size());
        s.insert(b);
        ASSERT_EQUALS(1U, s.size());
        ASSERT(!s.count(c));

        ASSERT(s.find(a) != s.end());
        ASSERT(s.find(b) != s.end());
        ASSERT(s.find(c) == s.end());


        s.insert(c);
        ASSERT_EQUALS(2U, s.size());


        ASSERT(s.find(a) != s.end());
        ASSERT(s.find(b) != s.end());
        ASSERT(s.find(c) != s.end());

        ASSERT(s.count(a));
        ASSERT(s.count(b));
        ASSERT(s.count(c));

        {
            BSONElementSet x;
            BSONObj o = fromjson("{ 'a' : [ 1 , 2 , 1 ] }");
            BSONObjIterator i(o["a"].embeddedObjectUserCheck());
            while (i.more()) {
                x.insert(i.next());
            }
            ASSERT_EQUALS(2U, x.size());
        }
    }
};

class EmbeddedNumbers {
public:
    void run() {
        BSONObj x = BSON("a" << BSON("b" << 1));
        BSONObj y = BSON("a" << BSON("b" << 1.0));
        ASSERT_BSONOBJ_EQ(x, y);
        ASSERT_EQUALS(0, x.woCompare(y));
    }
};

class BuilderPartialItearte {
public:
    void run() {
        {
            BSONObjBuilder b;
            b.append("x", 1);
            b.append("y", 2);

            BSONObjIterator i = b.iterator();
            ASSERT(i.more());
            ASSERT_EQUALS(1, i.next().numberInt());
            ASSERT(i.more());
            ASSERT_EQUALS(2, i.next().numberInt());
            ASSERT(!i.more());

            b.append("z", 3);

            i = b.iterator();
            ASSERT(i.more());
            ASSERT_EQUALS(1, i.next().numberInt());
            ASSERT(i.more());
            ASSERT_EQUALS(2, i.next().numberInt());
            ASSERT(i.more());
            ASSERT_EQUALS(3, i.next().numberInt());
            ASSERT(!i.more());

            ASSERT_BSONOBJ_EQ(BSON("x" << 1 << "y" << 2 << "z" << 3), b.obj());
        }
    }
};

class BSONForEachTest {
public:
    void run() {
        BSONObj obj = BSON("a" << 1 << "a" << 2 << "a" << 3);

        int count = 0;
        for (auto&& e : obj) {
            ASSERT_EQUALS(e.fieldName(), std::string("a"));
            count += e.Int();
        }

        ASSERT_EQUALS(count, 1 + 2 + 3);
    }
};

class CompareOps {
public:
    void run() {
        BSONObj a = BSON("a" << 1);
        BSONObj b = BSON("a" << 1);
        BSONObj c = BSON("a" << 2);
        BSONObj d = BSON("a" << 3);
        BSONObj e = BSON("a" << 4);
        BSONObj f = BSON("a" << 4);

        ASSERT(!SimpleBSONObjComparator::kInstance.evaluate((a < b)));
        ASSERT(SimpleBSONObjComparator::kInstance.evaluate(a <= b));
        ASSERT(SimpleBSONObjComparator::kInstance.evaluate(a < c));

        ASSERT(SimpleBSONObjComparator::kInstance.evaluate(f > d));
        ASSERT(SimpleBSONObjComparator::kInstance.evaluate(f >= e));
        ASSERT(!(SimpleBSONObjComparator::kInstance.evaluate(f > e)));
    }
};

class NestedBuilderOversize {
public:
    void run() {
        try {
            BSONObjBuilder outer;
            BSONObjBuilder inner(outer.subobjStart("inner"));

            std::string bigStr(1000, 'x');
            while (true) {
                ASSERT_LESS_THAN_OR_EQUALS(inner.len(), BufferMaxSize);
                inner.append("", bigStr);
            }

            ASSERT(!"Expected Throw");
        } catch (const DBException& e) {
            if (e.code() != 13548)  // we expect the code for oversized buffer
                throw;
        }
    }
};

class All : public unittest::OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("jsobj") {}

    void setupTests() override {
        add<BufBuilderBasic>();
        add<BufBuilderReallocLimit>();
        add<BSONElementBasic>();
        add<BSONObjTests::NullString>();
        add<BSONObjTests::Create>();
        add<BSONObjTests::WoCompareBasic>();
        add<BSONObjTests::NumericCompareBasic>();
        add<BSONObjTests::WoCompareEmbeddedObject>();
        add<BSONObjTests::WoCompareEmbeddedArray>();
        add<BSONObjTests::WoCompareOrdered>();
        add<BSONObjTests::WoCompareDifferentLength>();
        add<BSONObjTests::IsPrefixOf>();
        add<BSONObjTests::MultiKeySortOrder>();
        add<BSONObjTests::Nan>();
        add<BSONObjTests::AsTempObj>();
        add<BSONObjTests::AppendNumber>();
        add<BSONObjTests::ToStringNumber>();
        add<BSONObjTests::AppendAs>();
        add<BSONObjTests::ToStringRecursionDepth>();
        add<BSONObjTests::StringWithNull>();

        add<BSONObjTests::Validation::BadType>();
        add<BSONObjTests::Validation::EooBeforeEnd>();
        add<BSONObjTests::Validation::Undefined>();
        add<BSONObjTests::Validation::TotalSizeTooSmall>();
        add<BSONObjTests::Validation::EooMissing>();
        add<BSONObjTests::Validation::WrongStringSize>();
        add<BSONObjTests::Validation::ZeroStringSize>();
        add<BSONObjTests::Validation::NegativeStringSize>();
        add<BSONObjTests::Validation::WrongSubobjectSize>();
        add<BSONObjTests::Validation::WrongDbrefNsSize>();
        add<BSONObjTests::Validation::NoFieldNameEnd>();
        add<BSONObjTests::Validation::BadRegex>();
        add<BSONObjTests::Validation::BadRegexOptions>();
        add<BSONObjTests::Validation::CodeWScopeSmallSize>();
        add<BSONObjTests::Validation::CodeWScopeZeroStrSize>();
        add<BSONObjTests::Validation::CodeWScopeSmallStrSize>();
        add<BSONObjTests::Validation::CodeWScopeNoSizeForObj>();
        add<BSONObjTests::Validation::CodeWScopeSmallObjSize>();
        add<BSONObjTests::Validation::CodeWScopeBadObject>();
        add<BSONObjTests::Validation::NoSize>(BSONType::symbol);
        add<BSONObjTests::Validation::NoSize>(BSONType::code);
        add<BSONObjTests::Validation::NoSize>(BSONType::string);
        add<BSONObjTests::Validation::NoSize>(BSONType::codeWScope);
        add<BSONObjTests::Validation::NoSize>(BSONType::dbRef);
        add<BSONObjTests::Validation::NoSize>(BSONType::object);
        add<BSONObjTests::Validation::NoSize>(BSONType::array);
        add<BSONObjTests::Validation::NoSize>(BSONType::binData);
        add<OIDTests::init1>();
        add<OIDTests::initParse1>();
        add<OIDTests::append>();
        add<OIDTests::increasing>();
        add<OIDTests::ToDate>();
        add<OIDTests::FromDate>();
        add<ValueStreamTests::LabelBasic>();
        add<ValueStreamTests::LabelShares>();
        add<ValueStreamTests::LabelDouble>();
        add<ValueStreamTests::LabelDoubleShares>();
        add<ValueStreamTests::LabelSize>();
        add<ValueStreamTests::LabelMulti>();
        add<ValueStreamTests::LabelishOr>();
        add<ValueStreamTests::ElementAppend>();
        add<ValueStreamTests::AllTypes>();
        add<SubObjectBuilder>();
        add<DateBuilder>();
        add<DateNowBuilder>();
        add<TimeTBuilder>();
        add<MinMaxKeyBuilder>();
        add<MinMaxElementTest>();
        add<ComparatorTest>();
        add<CompatBSON>();
        add<CompareDottedFieldNamesTest>();
        add<CompareDottedArrayFieldNamesTest>();
        add<NestedDottedConversions>();
        add<BSONArrayBuilderTest>();
        add<ArrayMacroTest>();
        add<bson2settest>();
        add<BSONArrayIteratorSorted>();
        add<checkForStorageTests>();
        add<InvalidIDFind>();
        add<ElementSetTest>();
        add<EmbeddedNumbers>();
        add<BuilderPartialItearte>();
        add<BSONForEachTest>();
        add<CompareOps>();
        add<NestedBuilderOversize>();
    }
};

unittest::OldStyleSuiteInitializer<All> myall;

}  // namespace JsobjTests
}  // namespace mongo
