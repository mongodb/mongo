// jsobjtests.cpp - Tests for jsobj.{h,cpp} code
//

/**
 *    Copyright (C) 2008 10gen Inc.
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

#include <cmath>
#include <iostream>

#include "mongo/bson/util/builder.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/storage/mmap_v1/btree/key.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/allocator.h"
#include "mongo/util/embedded_builder.h"
#include "mongo/util/log.h"
#include "mongo/util/stringutils.h"

namespace mongo {

using std::cout;
using std::endl;
using std::numeric_limits;
using std::string;
using std::stringstream;
using std::vector;

namespace dps = ::mongo::dotted_path_support;

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
void nested2dotted(BSONObjBuilder& b, const BSONObj& obj, const string& base = "") {
    BSONObjIterator it(obj);
    while (it.more()) {
        BSONElement e = it.next();
        if (e.type() == Object) {
            string newbase = base + e.fieldName() + ".";
            nested2dotted(b, e.embeddedObject(), newbase);
        } else {
            string newbase = base + e.fieldName();
            b.appendAs(e, newbase);
        }
    }
}

BSONObj nested2dotted(const BSONObj& obj) {
    BSONObjBuilder b;
    nested2dotted(b, obj);
    return b.obj();
}

FieldCompareResult compareDottedFieldNames(const string& l, const string& r, const LexNumCmp& cmp) {
    static int maxLoops = 1024 * 1024;

    size_t lstart = 0;
    size_t rstart = 0;

    for (int i = 0; i < maxLoops; i++) {
        size_t a = l.find('.', lstart);
        size_t b = r.find('.', rstart);

        size_t lend = a == string::npos ? l.size() : a;
        size_t rend = b == string::npos ? r.size() : b;

        const string& c = l.substr(lstart, lend - lstart);
        const string& d = r.substr(rstart, rend - rstart);

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

    log() << "compareDottedFieldNames ERROR  l: " << l << " r: " << r << "  TOO MANY LOOPS" << endl;
    verify(0);
    return SAME;  // will never get here
}
}

namespace JsobjTests {

void keyTest(const BSONObj& o, bool mustBeCompact = false) {
    static KeyV1Owned* kLast;
    static BSONObj last;

    KeyV1Owned* key = new KeyV1Owned(o);
    KeyV1Owned& k = *key;

    ASSERT(!mustBeCompact || k.isCompactFormat());

    BSONObj x = k.toBson();
    int res = o.woCompare(x, BSONObj(), /*considerfieldname*/ false);
    if (res) {
        cout << o.toString() << endl;
        k.toBson();
        cout << x.toString() << endl;
        o.woCompare(x, BSONObj(), /*considerfieldname*/ false);
        ASSERT(res == 0);
    }
    ASSERT(k.woEqual(k));
    ASSERT(!k.isCompactFormat() || k.dataSize() < o.objsize());

    {
        // check BSONObj::equal.  this part not a KeyV1 test.
        int res = o.woCompare(last);
        ASSERT((res == 0) == o.equal(last));
    }

    if (kLast) {
        int r1 = o.woCompare(last, BSONObj(), false);
        int r2 = k.woCompare(*kLast, Ordering::make(BSONObj()));
        bool ok = (r1 < 0 && r2 < 0) || (r1 > 0 && r2 > 0) || r1 == r2;
        if (!ok) {
            cout << "r1r2 " << r1 << ' ' << r2 << endl;
            cout << "o:" << o.toString() << endl;
            cout << "last:" << last.toString() << endl;
            cout << "k:" << k.toString() << endl;
            cout << "kLast:" << kLast->toString() << endl;
            int r3 = k.woCompare(*kLast, Ordering::make(BSONObj()));
            cout << r3 << endl;
        }
        ASSERT(ok);
        if (k.isCompactFormat() && kLast->isCompactFormat()) {
            // only check if not bson as bson woEqual is broken! (or was may2011)
            if (k.woEqual(*kLast) != (r2 == 0)) {  // check woEqual matches
                cout << r2 << endl;
                cout << k.toString() << endl;
                cout << kLast->toString() << endl;
                k.woEqual(*kLast);
                ASSERT(false);
            }
        }
    }

    delete kLast;
    kLast = key;
    last = o.getOwned();
}

class BufBuilderBasic {
public:
    void run() {
        {
            BufBuilder b(0);
            b.appendStr("foo");
            ASSERT_EQUALS(4, b.len());
            ASSERT(strcmp("foo", b.buf()) == 0);
        }
        {
            mongo::StackBufBuilder b;
            b.appendStr("foo");
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
            for (; written <= 64 * 1024 * 1024 + 1; ++written)
                // (re)alloc past the buffer 64mb limit
                b.appendStr("a");
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
    static BSONObj basic(const char* name, vector<int> val) {
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
        {
            BSONObj k = BSON("x" << 1);
            verify(!k.isPrefixOf(BSON("a" << 1)));
            verify(k.isPrefixOf(BSON("x" << 1)));
            verify(k.isPrefixOf(BSON("x" << 1 << "a" << 1)));
            verify(!k.isPrefixOf(BSON("a" << 1 << "x" << 1)));
        }
        {
            BSONObj k = BSON("x" << 1 << "y" << 1);
            verify(!k.isPrefixOf(BSON("x" << 1)));
            verify(!k.isPrefixOf(BSON("x" << 1 << "z" << 1)));
            verify(k.isPrefixOf(BSON("x" << 1 << "y" << 1)));
            verify(k.isPrefixOf(BSON("x" << 1 << "y" << 1 << "z" << 1)));
        }
        {
            BSONObj k = BSON("x" << 1);
            verify(!k.isPrefixOf(BSON("x"
                                      << "hi")));
            verify(k.isPrefixOf(BSON("x" << 1 << "a"
                                         << "hi")));
        }
        {
            BSONObj k = BSON("x" << 1);
            verify(k.isFieldNamePrefixOf(BSON("x"
                                              << "hi")));
            verify(!k.isFieldNamePrefixOf(BSON("a" << 1)));
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
        vector<int> i;
        i.push_back(1);
        i.push_back(2);
        vector<double> d;
        d.push_back(1);
        d.push_back(2);
        ASSERT(basic("a", i).woCompare(basic("a", d)) == 0);

        vector<int> j;
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
        ASSERT(BSON("x"
                    << "a")
                   .woCompare(BSON("x"
                                   << "b")) < 0);
        ASSERT(BSON("x"
                    << "b")
                   .woCompare(BSON("x"
                                   << "a")) > 0);

        ASSERT(BSON("x"
                    << "a"
                    << "y"
                    << "a")
                   .woCompare(BSON("x"
                                   << "a"
                                   << "y"
                                   << "b")) < 0);
        ASSERT(BSON("x"
                    << "a"
                    << "y"
                    << "a")
                   .woCompare(BSON("x"
                                   << "b"
                                   << "y"
                                   << "a")) < 0);
        ASSERT(BSON("x"
                    << "a"
                    << "y"
                    << "a")
                   .woCompare(BSON("x"
                                   << "b")) < 0);

        ASSERT(BSON("x"
                    << "c")
                   .woCompare(BSON("x"
                                   << "b"
                                   << "y"
                                   << "h")) > 0);
        ASSERT(BSON("x"
                    << "b"
                    << "y"
                    << "b")
                   .woCompare(BSON("x"
                                   << "c")) < 0);

        BSONObj key = BSON("x" << 1 << "y" << 1);

        ASSERT(dps::compareObjectsAccordingToSort(BSON("x"
                                                       << "c"),
                                                  BSON("x"
                                                       << "b"
                                                       << "y"
                                                       << "h"),
                                                  key) > 0);
        ASSERT(BSON("x"
                    << "b"
                    << "y"
                    << "b")
                   .woCompare(BSON("x"
                                   << "c"),
                              key) < 0);

        key = BSON("" << 1 << "" << 1);

        ASSERT(dps::compareObjectsAccordingToSort(BSON(""
                                                       << "c"),
                                                  BSON(""
                                                       << "b"
                                                       << ""
                                                       << "h"),
                                                  key) > 0);
        ASSERT(BSON(""
                    << "b"
                    << ""
                    << "b")
                   .woCompare(BSON(""
                                   << "c"),
                              key) < 0);

        {
            // test a big key
            string x(2000, 'z');
            BSONObj o = BSON("q" << x);
            keyTest(o, false);
        }
        {
            string y(200, 'w');
            BSONObjBuilder b;
            for (int i = 0; i < 10; i++) {
                b.append("x", y);
            }
            keyTest(b.obj(), true);
        }
        {
            double nan = numeric_limits<double>::quiet_NaN();
            BSONObj o = BSON("y" << nan);
            keyTest(o);
        }

        {
            BSONObjBuilder b;
            b.append("", "c");
            b.appendNull("");
            BSONObj o = b.obj();
            keyTest(o);
            ASSERT(dps::compareObjectsAccordingToSort(o,
                                                      BSON(""
                                                           << "b"
                                                           << ""
                                                           << "h"),
                                                      key) > 0);
            ASSERT(dps::compareObjectsAccordingToSort(BSON(""
                                                           << "b"
                                                           << ""
                                                           << "h"),
                                                      o,
                                                      key) < 0);
        }

        ASSERT(BSON(""
                    << "a")
                   .woCompare(BSON(""
                                   << "a"
                                   << ""
                                   << "c")) < 0);
        {
            BSONObjBuilder b;
            b.append("", "a");
            b.appendNull("");
            ASSERT(b.obj().woCompare(BSON(""
                                          << "a"
                                          << ""
                                          << "c")) < 0);  // SERVER-282
        }
    }
};

class Nan : public Base {
public:
    void run() {
        double inf = numeric_limits<double>::infinity();
        double nan = numeric_limits<double>::quiet_NaN();
        double nan2 = numeric_limits<double>::signaling_NaN();
        ASSERT(std::isnan(nan));
        ASSERT(std::isnan(nan2));
        ASSERT(!std::isnan(inf));

        ASSERT(BSON("a" << inf).woCompare(BSON("a" << inf)) == 0);
        ASSERT(BSON("a" << inf).woCompare(BSON("a" << 1)) > 0);
        ASSERT(BSON("a" << 1).woCompare(BSON("a" << inf)) < 0);

        ASSERT(BSON("a" << nan).woCompare(BSON("a" << nan)) == 0);
        ASSERT(BSON("a" << nan).woCompare(BSON("a" << 1)) < 0);

        ASSERT(BSON("a" << nan).woCompare(BSON("a" << 5000000000LL)) < 0);

        {
            KeyV1Owned a(BSON("a" << nan));
            KeyV1Owned b(BSON("a" << 1));
            Ordering o = Ordering::make(BSON("a" << 1));
            ASSERT(a.woCompare(b, o) < 0);
        }

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
            ASSERT(tmp.valid());
            ASSERT(tmp.hasField("a"));
            ASSERT(!tmp.hasField("b"));
            ASSERT(tmp == BSON("a" << 1));

            bb << "b" << 2;
            BSONObj obj = bb.obj();
            ASSERT_EQUALS(obj.objsize(), 4 + (1 + 2 + 4) + (1 + 2 + 4) + 1);
            ASSERT(obj.valid());
            ASSERT(obj.hasField("a"));
            ASSERT(obj.hasField("b"));
            ASSERT(obj == BSON("a" << 1 << "b" << 2));
        }
        {
            BSONObjBuilder bb;
            bb << "a" << GT << 1;
            BSONObj tmp = bb.asTempObj();
            ASSERT(tmp.objsize() == 4 + (1 + 2 + (4 + 1 + 4 + 4 + 1)) + 1);
            ASSERT(tmp.valid());
            ASSERT(tmp.hasField("a"));
            ASSERT(!tmp.hasField("b"));
            ASSERT(tmp == BSON("a" << BSON("$gt" << 1)));

            bb << "b" << LT << 2;
            BSONObj obj = bb.obj();
            ASSERT(obj.objsize() ==
                   4 + (1 + 2 + (4 + 1 + 4 + 4 + 1)) + (1 + 2 + (4 + 1 + 4 + 4 + 1)) + 1);
            ASSERT(obj.valid());
            ASSERT(obj.hasField("a"));
            ASSERT(obj.hasField("b"));
            ASSERT(obj == BSON("a" << BSON("$gt" << 1) << "b" << BSON("$lt" << 2)));
        }
        {
            BSONObjBuilder bb(32);
            bb << "a" << 1;
            BSONObj tmp = bb.asTempObj();
            ASSERT(tmp.objsize() == 4 + (1 + 2 + 4) + 1);
            ASSERT(tmp.valid());
            ASSERT(tmp.hasField("a"));
            ASSERT(!tmp.hasField("b"));
            ASSERT(tmp == BSON("a" << 1));

            // force a realloc
            BSONArrayBuilder arr;
            for (int i = 0; i < 10000; i++) {
                arr << i;
            }
            bb << "b" << arr.arr();
            BSONObj obj = bb.obj();
            ASSERT(obj.valid());
            ASSERT(obj.hasField("a"));
            ASSERT(obj.hasField("b"));
        }
    }
};

struct AppendIntOrLL {
    void run() {
        const long long billion = 1000 * 1000 * 1000;

        long long n = 0x3333111122224444LL;
        {
            double d = (double)n;
            BSONObj a = BSON("x" << n);
            BSONObj b = BSON("x" << d);

            long long back = (long long)d;
            // 3719
            //////                    int res = a.woCompare(b);

            ASSERT(n > back);
            // ASSERT( res > 0 );  // SERVER-3719

            keyTest(a, false);

            KeyV1Owned A(a);
            KeyV1Owned B(b);
            // 3719
            //////                    int res2 =  A.woCompare(B, Ordering::make(BSONObj()));
            // ASSERT( res2 > 0 ); // SERVER-3719

            // fixing requires an index v# change.

            cout << "todo fix SERVER-3719 and uncomment test in AppendIntOrLL" << endl;

            n++;
        }

        {
            BSONObjBuilder b;
            b.appendIntOrLL("L4", -4 * billion);
            keyTest(b.obj());
            keyTest(BSON("" << billion));
        }

        BSONObjBuilder b;
        b.appendIntOrLL("i1", 1);
        b.appendIntOrLL("i2", -1);
        b.appendIntOrLL("i3", 1 * billion);
        b.appendIntOrLL("i4", -1 * billion);

        b.appendIntOrLL("L1", 2 * billion);
        b.appendIntOrLL("L2", -2 * billion);
        b.appendIntOrLL("L3", 4 * billion);
        b.appendIntOrLL("L4", -4 * billion);
        b.appendIntOrLL("L5", 16 * billion);
        b.appendIntOrLL("L6", -16 * billion);

        BSONObj o = b.obj();
        keyTest(o);

        ASSERT(o["i1"].type() == NumberInt);
        ASSERT(o["i1"].number() == 1);
        ASSERT(o["i2"].type() == NumberInt);
        ASSERT(o["i2"].number() == -1);
        ASSERT(o["i3"].type() == NumberInt);
        ASSERT(o["i3"].number() == 1 * billion);
        ASSERT(o["i4"].type() == NumberInt);
        ASSERT(o["i4"].number() == -1 * billion);

        ASSERT(o["L1"].isNumber());
        ASSERT(o["L1"].number() == 2 * billion);
        ASSERT(o["L2"].isNumber());
        ASSERT(o["L2"].number() == -2 * billion);
        ASSERT(o["L3"].type() == NumberLong);
        ASSERT(o["L3"].number() == 4 * billion);
        ASSERT(o["L4"].type() == NumberLong);
        ASSERT(o["L4"].number() == -4 * billion);
        ASSERT(o["L5"].type() == NumberLong);
        ASSERT(o["L5"].number() == 16 * billion);
        ASSERT(o["L6"].type() == NumberLong);
        ASSERT(o["L6"].number() == -16 * billion);
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
        keyTest(o);

        ASSERT(o["a"].type() == NumberInt);
        ASSERT(o["b"].type() == NumberDouble);
        ASSERT(o["c"].type() == NumberInt);
        ASSERT(o["d"].type() == NumberDouble);
        ASSERT(o["e"].type() == NumberLong);
        ASSERT(o["f"].type() == NumberDecimal);
    }
};

class ToStringArray {
public:
    void run() {
        string spec = "{ a: [ \"a\", \"b\" ] }";
        ASSERT_EQUALS(spec, fromjson(spec).toString());

        BSONObj x = BSON("a"
                         << "astring"
                         << "b"
                         << "str");
        keyTest(x);
        keyTest(x);
        BSONObj y = BSON("a"
                         << "astring"
                         << "b"
                         << "stra");
        keyTest(y);
        y = BSON("a"
                 << "");
        keyTest(y);

        keyTest(BSON("abc" << true));
        keyTest(BSON("abc" << false));
        keyTest(BSON("abc" << false << "b" << true));

        Date_t now = jsTime();
        keyTest(BSON("" << now << "" << 3 << "" << jstNULL << "" << true));
        keyTest(BSON("" << now << "" << 3 << "" << BSONObj() << "" << true));

        {{// check signed dates with new key format
          KeyV1Owned a(BSONObjBuilder().appendDate("", Date_t::fromMillisSinceEpoch(-50)).obj());
        KeyV1Owned b(BSONObjBuilder().appendDate("", Date_t::fromMillisSinceEpoch(50)).obj());
        ASSERT(a.woCompare(b, Ordering::make(BSONObj())) < 0);
    }
    {
        // backward compatibility
        KeyBson a(BSONObjBuilder().appendDate("", Date_t::fromMillisSinceEpoch(-50)).obj());
        KeyBson b(BSONObjBuilder().appendDate("", Date_t::fromMillisSinceEpoch(50)).obj());
        ASSERT(a.woCompare(b, Ordering::make(BSONObj())) > 0);
    }
    {
        // this is an uncompactable key:
        BSONObj uc1 = BSONObjBuilder()
                          .appendDate("", Date_t::fromMillisSinceEpoch(-50))
                          .appendCode("", "abc")
                          .obj();
        BSONObj uc2 = BSONObjBuilder()
                          .appendDate("", Date_t::fromMillisSinceEpoch(55))
                          .appendCode("", "abc")
                          .obj();
        ASSERT(uc1.woCompare(uc2, Ordering::make(BSONObj())) < 0);
        {
            KeyV1Owned a(uc1);
            KeyV1Owned b(uc2);
            ASSERT(!a.isCompactFormat());
            ASSERT(a.woCompare(b, Ordering::make(BSONObj())) < 0);
        }
        {
            KeyBson a(uc1);
            KeyBson b(uc2);
            ASSERT(!a.isCompactFormat());
            ASSERT(a.woCompare(b, Ordering::make(BSONObj())) > 0);
        }
    }
}

{
    BSONObjBuilder b;
    b.appendBinData("f", 8, (BinDataType)1, "aaaabbbb");
    b.appendBinData("e", 3, (BinDataType)1, "aaa");
    b.appendBinData("b", 1, (BinDataType)1, "x");
    BSONObj o = b.obj();
    keyTest(o, true);
}

{
    // check (non)equality
    BSONObj a = BSONObjBuilder().appendBinData("", 8, (BinDataType)1, "abcdefgh").obj();
    BSONObj b = BSONObjBuilder().appendBinData("", 8, (BinDataType)1, "abcdefgj").obj();
    ASSERT(!a.equal(b));
    int res_ab = a.woCompare(b);
    ASSERT(res_ab != 0);
    keyTest(a, true);
    keyTest(b, true);

    // check subtypes do not equal
    BSONObj c = BSONObjBuilder().appendBinData("", 8, (BinDataType)4, "abcdefgh").obj();
    BSONObj d = BSONObjBuilder().appendBinData("", 8, (BinDataType)0x81, "abcdefgh").obj();
    ASSERT(!a.equal(c));
    int res_ac = a.woCompare(c);
    ASSERT(res_ac != 0);
    keyTest(c, true);
    ASSERT(!a.equal(d));
    int res_ad = a.woCompare(d);
    ASSERT(res_ad != 0);
    keyTest(d, true);

    KeyV1Owned A(a);
    KeyV1Owned B(b);
    KeyV1Owned C(c);
    KeyV1Owned D(d);
    ASSERT(!A.woEqual(B));
    ASSERT(A.woCompare(B, Ordering::make(BSONObj())) < 0 && res_ab < 0);
    ASSERT(!A.woEqual(C));
    ASSERT(A.woCompare(C, Ordering::make(BSONObj())) < 0 && res_ac < 0);
    ASSERT(!A.woEqual(D));
    ASSERT(A.woCompare(D, Ordering::make(BSONObj())) < 0 && res_ad < 0);
}

{
    BSONObjBuilder b;
    b.appendBinData("f", 33, (BinDataType)1, "123456789012345678901234567890123");
    BSONObj o = b.obj();
    keyTest(o, false);
}

{
    for (int i = 1; i <= 3; i++) {
        for (int j = 1; j <= 3; j++) {
            BSONObjBuilder b;
            b.appendBinData("f", i, (BinDataType)j, "abc");
            BSONObj o = b.obj();
            keyTest(o, j != ByteArrayDeprecated);
        }
    }
}

{
    BSONObjBuilder b;
    b.appendBinData("f", 1, (BinDataType)133, "a");
    BSONObj o = b.obj();
    keyTest(o, true);
}

{
    BSONObjBuilder b;
    b.append("AA", 3);
    b.appendBinData("f", 0, (BinDataType)0, "");
    b.appendBinData("e", 3, (BinDataType)7, "aaa");
    b.appendBinData("b", 1, (BinDataType)128, "x");
    b.append("z", 3);
    b.appendBinData("bb", 0, (BinDataType)129, "x");
    BSONObj o = b.obj();
    keyTest(o, true);
}

{
    // 9 is not supported in compact format. so test a non-compact case here.
    BSONObjBuilder b;
    b.appendBinData("f", 9, (BinDataType)0, "aaaabbbbc");
    BSONObj o = b.obj();
    keyTest(o);
}
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
        keyTest(x);

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

            // test that nulls are ok within bson strings
            ASSERT(!(A == B));
            ASSERT(A > B);

            ASSERT(!(B == C));
            ASSERT(C > B);

            // check iteration is ok
            ASSERT(B["z"].Bool() && A["z"].Bool() && C["z"].Bool());
        }

        BSONObjBuilder b;
        b.append("a", "a\0b", 4);
        string z("a\0b", 3);
        b.append("b", z);
        b.appendAs(b.asTempObj()["a"], "c");
        BSONObj o = b.obj();
        keyTest(o);

        stringstream ss;
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
        ASSERT_EQUALS(BSON("bar" << 1), b.done());
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
        string nestedBSONString;
        size_t found;

        // recursion depth one less than max allowed-- do not shorten the string
        nestedBSON = recursiveBSON(BSONObj::maxToStringRecursionDepth - 1);
        nestedBSON.toString(s, true, false);
        nestedBSONString = s.str();
        found = nestedBSONString.find("...");
        // did not find the "..." pattern
        ASSERT_EQUALS(found != string::npos, false);

        // recursion depth is equal to max allowed  -- do not shorten the string
        nestedBSON = recursiveBSON(BSONObj::maxToStringRecursionDepth);
        nestedBSON.toString(s, true, false);
        nestedBSONString = s.str();
        found = nestedBSONString.find("...");
        // did not find the "..." pattern
        ASSERT_EQUALS(found != string::npos, false);

        // recursion depth - one greater than max allowed -- shorten the string
        nestedBSON = recursiveBSON(BSONObj::maxToStringRecursionDepth + 1);
        nestedBSON.toString(s, false, false);
        nestedBSONString = s.str();
        found = nestedBSONString.find("...");
        // found the "..." pattern
        ASSERT_EQUALS(found != string::npos, true);

        /* recursion depth - one greater than max allowed but with full=true
         * should fail with an assertion
         */
        nestedBSON = recursiveBSON(BSONObj::maxToStringRecursionDepth + 1);
        ASSERT_THROWS(nestedBSON.toString(s, false, true), UserException);
    }
};

class StringWithNull {
public:
    void run() {
        const string input = string("a") + '\0' + 'b';
        ASSERT_EQUALS(input.size(), 3U);

        BSONObj obj = BSON("str" << input);
        const string output = obj.firstElement().String();
        ASSERT_EQUALS(escape(output), escape(input));  // for better failure output
        ASSERT_EQUALS(output, input);
    }
};

namespace Validation {

class Base {
public:
    virtual ~Base() {}
    void run() {
        ASSERT(valid().valid());
        ASSERT(!invalid().valid());
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
    BSONObj valid() const {
        return fromjson("{\"a\":1}");
    }
    BSONObj invalid() const {
        BSONObj ret = valid();
        set(ret, 4, 50);
        return ret;
    }
};

class EooBeforeEnd : public Base {
    BSONObj valid() const {
        return fromjson("{\"a\":1}");
    }
    BSONObj invalid() const {
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
        set(o, 4, mongo::Undefined);
        ASSERT(o.valid());
    }
};

class TotalSizeTooSmall : public Base {
    BSONObj valid() const {
        return fromjson("{\"a\":1}");
    }
    BSONObj invalid() const {
        BSONObj ret = valid();
        // (first byte of size)--
        set(ret, 0, get(ret, 0) - 1);
        // re-read size for BSONObj::details
        return ret.copy();
    }
};

class EooMissing : public Base {
    BSONObj valid() const {
        return fromjson("{\"a\":1}");
    }
    BSONObj invalid() const {
        BSONObj ret = valid();
        set(ret, ret.objsize() - 1, (char)0xff);
        // (first byte of size)--
        set(ret, 0, get(ret, 0) - 1);
        // re-read size for BSONObj::details
        return ret.copy();
    }
};

class WrongStringSize : public Base {
    BSONObj valid() const {
        return fromjson("{\"a\":\"b\"}");
    }
    BSONObj invalid() const {
        BSONObj ret = valid();
        ASSERT_EQUALS(ret.firstElement().valuestr()[0], 'b');
        ASSERT_EQUALS(ret.firstElement().valuestr()[1], 0);
        ((char*)ret.firstElement().valuestr())[1] = 1;
        return ret.copy();
    }
};

class ZeroStringSize : public Base {
    BSONObj valid() const {
        return fromjson("{\"a\":\"b\"}");
    }
    BSONObj invalid() const {
        BSONObj ret = valid();
        set(ret, 7, 0);
        return ret;
    }
};

class NegativeStringSize : public Base {
    BSONObj valid() const {
        return fromjson("{\"a\":\"b\"}");
    }
    BSONObj invalid() const {
        BSONObj ret = valid();
        set(ret, 10, -100);
        return ret;
    }
};

class WrongSubobjectSize : public Base {
    BSONObj valid() const {
        return fromjson("{\"a\":{\"b\":1}}");
    }
    BSONObj invalid() const {
        BSONObj ret = valid();
        set(ret, 0, get(ret, 0) + 1);
        set(ret, 7, get(ret, 7) + 1);
        return ret.copy();
    }
};

class WrongDbrefNsSize : public Base {
    BSONObj valid() const {
        return fromjson("{ \"a\": Dbref( \"b\", \"ffffffffffffffffffffffff\" ) }");
    }
    BSONObj invalid() const {
        BSONObj ret = valid();
        set(ret, 0, get(ret, 0) + 1);
        set(ret, 7, get(ret, 7) + 1);
        return ret.copy();
    };
};

class NoFieldNameEnd : public Base {
    BSONObj valid() const {
        return fromjson("{\"a\":1}");
    }
    BSONObj invalid() const {
        BSONObj ret = valid();
        memset(const_cast<char*>(ret.objdata()) + 5, 0xff, ret.objsize() - 5);
        return ret;
    }
};

class BadRegex : public Base {
    BSONObj valid() const {
        return fromjson("{\"a\":/c/i}");
    }
    BSONObj invalid() const {
        BSONObj ret = valid();
        memset(const_cast<char*>(ret.objdata()) + 7, 0xff, ret.objsize() - 7);
        return ret;
    }
};

class BadRegexOptions : public Base {
    BSONObj valid() const {
        return fromjson("{\"a\":/c/i}");
    }
    BSONObj invalid() const {
        BSONObj ret = valid();
        memset(const_cast<char*>(ret.objdata()) + 9, 0xff, ret.objsize() - 9);
        return ret;
    }
};

class CodeWScopeBase : public Base {
    BSONObj valid() const {
        BSONObjBuilder b;
        BSONObjBuilder scope;
        scope.append("a", "b");
        b.appendCodeWScope("c", "d", scope.done());
        return b.obj();
    }
    BSONObj invalid() const {
        BSONObj ret = valid();
        modify(ret);
        return ret;
    }

protected:
    virtual void modify(BSONObj& o) const = 0;
};

class CodeWScopeSmallSize : public CodeWScopeBase {
    void modify(BSONObj& o) const {
        set(o, 7, 7);
    }
};

class CodeWScopeZeroStrSize : public CodeWScopeBase {
    void modify(BSONObj& o) const {
        set(o, 11, 0);
    }
};

class CodeWScopeSmallStrSize : public CodeWScopeBase {
    void modify(BSONObj& o) const {
        set(o, 11, 1);
    }
};

class CodeWScopeNoSizeForObj : public CodeWScopeBase {
    void modify(BSONObj& o) const {
        set(o, 7, 13);
    }
};

class CodeWScopeSmallObjSize : public CodeWScopeBase {
    void modify(BSONObj& o) const {
        set(o, 17, 1);
    }
};

class CodeWScopeBadObject : public CodeWScopeBase {
    void modify(BSONObj& o) const {
        set(o, 21, JSTypeMax + 1);
    }
};

class NoSize {
public:
    NoSize(BSONType type) : type_(type) {}
    void run() {
        const char data[] = {0x07, 0x00, 0x00, 0x00, char(type_), 'a', 0x00};
        BSONObj o(data);
        ASSERT(!o.valid());
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
        b.appendOID("a", 0);
        b.appendOID("b", 0, false);
        b.appendOID("c", 0, true);
        BSONObj o = b.obj();
        keyTest(o);

        ASSERT(o["a"].__oid().toString() == "000000000000000000000000");
        ASSERT(o["b"].__oid().toString() == "000000000000000000000000");
        ASSERT(o["c"].__oid().toString() != "000000000000000000000000");
    }
};

class increasing {
public:
    BSONObj g() {
        BSONObjBuilder b;
        b.appendOID("_id", 0, true);
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
        ASSERT_LT(BSON("" << min), BSON("" << oid));
        ASSERT_GT(BSON("" << max), BSON("" << oid));
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
    BSONObj expected() {
        return BSON("a" << (BSON("$gt" << 1)));
    }
    BSONObj actual() {
        return BSON("a" << GT << 1);
    }
};

class LabelShares : public LabelBase {
    BSONObj expected() {
        return BSON("z"
                    << "q"
                    << "a"
                    << (BSON("$gt" << 1))
                    << "x"
                    << "p");
    }
    BSONObj actual() {
        return BSON("z"
                    << "q"
                    << "a"
                    << GT
                    << 1
                    << "x"
                    << "p");
    }
};

class LabelDouble : public LabelBase {
    BSONObj expected() {
        return BSON("a" << (BSON("$gt" << 1 << "$lte"
                                       << "x")));
    }
    BSONObj actual() {
        return BSON("a" << GT << 1 << LTE << "x");
    }
};

class LabelDoubleShares : public LabelBase {
    BSONObj expected() {
        return BSON("z"
                    << "q"
                    << "a"
                    << (BSON("$gt" << 1 << "$lte"
                                   << "x"))
                    << "x"
                    << "p");
    }
    BSONObj actual() {
        return BSON("z"
                    << "q"
                    << "a"
                    << GT
                    << 1
                    << LTE
                    << "x"
                    << "x"
                    << "p");
    }
};

class LabelSize : public LabelBase {
    BSONObj expected() {
        return BSON("a" << BSON("$size" << 4));
    }
    BSONObj actual() {
        return BSON("a" << mongo::BSIZE << 4);
    }
};

class LabelMulti : public LabelBase {
    BSONObj expected() {
        return BSON("z"
                    << "q"
                    << "a"
                    << BSON("$gt" << 1 << "$lte"
                                  << "x")
                    << "b"
                    << BSON("$ne" << 1 << "$ne"
                                  << "f"
                                  << "$ne"
                                  << 22.3)
                    << "x"
                    << "p");
    }
    BSONObj actual() {
        return BSON("z"
                    << "q"
                    << "a"
                    << GT
                    << 1
                    << LTE
                    << "x"
                    << "b"
                    << NE
                    << 1
                    << NE
                    << "f"
                    << NE
                    << 22.3
                    << "x"
                    << "p");
    }
};
class LabelishOr : public LabelBase {
    BSONObj expected() {
        return BSON("$or" << BSON_ARRAY(BSON("a" << BSON("$gt" << 1 << "$lte"
                                                               << "x"))
                                        << BSON("b" << BSON("$ne" << 1 << "$ne"
                                                                  << "f"
                                                                  << "$ne"
                                                                  << 22.3))
                                        << BSON("x"
                                                << "p")));
    }
    BSONObj actual() {
        return OR(BSON("a" << GT << 1 << LTE << "x"),
                  BSON("b" << NE << 1 << NE << "f" << NE << 22.3),
                  BSON("x"
                       << "p"));
    }
};

class Unallowed {
public:
    void run() {
        ASSERT_THROWS(BSON(GT << 4), MsgAssertionException);
        ASSERT_THROWS(BSON("a" << 1 << GT << 4), MsgAssertionException);
    }
};

class ElementAppend {
public:
    void run() {
        BSONObj a = BSON("a" << 17);
        BSONObj b = BSON("b" << a["a"]);
        ASSERT_EQUALS(NumberInt, a["a"].type());
        ASSERT_EQUALS(NumberInt, b["b"].type());
        ASSERT_EQUALS(17, b["b"].number());
    }
};

class AllTypes {
public:
    void run() {
        // These are listed in order of BSONType

        ASSERT_EQUALS(objTypeOf(MINKEY), MinKey);
        ASSERT_EQUALS(arrTypeOf(MINKEY), MinKey);

        // EOO not valid in middle of BSONObj

        ASSERT_EQUALS(objTypeOf(1.0), NumberDouble);
        ASSERT_EQUALS(arrTypeOf(1.0), NumberDouble);

        ASSERT_EQUALS(objTypeOf(""), String);
        ASSERT_EQUALS(arrTypeOf(""), String);
        ASSERT_EQUALS(objTypeOf(string()), String);
        ASSERT_EQUALS(arrTypeOf(string()), String);
        ASSERT_EQUALS(objTypeOf(StringData("")), String);
        ASSERT_EQUALS(arrTypeOf(StringData("")), String);

        ASSERT_EQUALS(objTypeOf(BSONObj()), Object);
        ASSERT_EQUALS(arrTypeOf(BSONObj()), Object);

        ASSERT_EQUALS(objTypeOf(BSONArray()), Array);
        ASSERT_EQUALS(arrTypeOf(BSONArray()), Array);

        ASSERT_EQUALS(objTypeOf(BSONBinData("", 0, BinDataGeneral)), BinData);
        ASSERT_EQUALS(arrTypeOf(BSONBinData("", 0, BinDataGeneral)), BinData);

        ASSERT_EQUALS(objTypeOf(BSONUndefined), Undefined);
        ASSERT_EQUALS(arrTypeOf(BSONUndefined), Undefined);

        ASSERT_EQUALS(objTypeOf(OID()), jstOID);
        ASSERT_EQUALS(arrTypeOf(OID()), jstOID);

        ASSERT_EQUALS(objTypeOf(true), Bool);
        ASSERT_EQUALS(arrTypeOf(true), Bool);

        ASSERT_EQUALS(objTypeOf(Date_t()), Date);
        ASSERT_EQUALS(arrTypeOf(Date_t()), Date);

        ASSERT_EQUALS(objTypeOf(BSONNULL), jstNULL);
        ASSERT_EQUALS(arrTypeOf(BSONNULL), jstNULL);

        ASSERT_EQUALS(objTypeOf(BSONRegEx("", "")), RegEx);
        ASSERT_EQUALS(arrTypeOf(BSONRegEx("", "")), RegEx);

        ASSERT_EQUALS(objTypeOf(BSONDBRef("", OID())), DBRef);
        ASSERT_EQUALS(arrTypeOf(BSONDBRef("", OID())), DBRef);

        ASSERT_EQUALS(objTypeOf(BSONCode("")), Code);
        ASSERT_EQUALS(arrTypeOf(BSONCode("")), Code);

        ASSERT_EQUALS(objTypeOf(BSONSymbol("")), Symbol);
        ASSERT_EQUALS(arrTypeOf(BSONSymbol("")), Symbol);

        ASSERT_EQUALS(objTypeOf(BSONCodeWScope("", BSONObj())), CodeWScope);
        ASSERT_EQUALS(arrTypeOf(BSONCodeWScope("", BSONObj())), CodeWScope);

        ASSERT_EQUALS(objTypeOf(1), NumberInt);
        ASSERT_EQUALS(arrTypeOf(1), NumberInt);

        ASSERT_EQUALS(objTypeOf(Timestamp()), bsonTimestamp);
        ASSERT_EQUALS(arrTypeOf(Timestamp()), bsonTimestamp);

        ASSERT_EQUALS(objTypeOf(1LL), NumberLong);
        ASSERT_EQUALS(arrTypeOf(1LL), NumberLong);

        ASSERT_EQUALS(objTypeOf(mongo::Decimal128("1")), NumberDecimal);
        ASSERT_EQUALS(arrTypeOf(mongo::Decimal128("1")), NumberDecimal);

        ASSERT_EQUALS(objTypeOf(MAXKEY), MaxKey);
        ASSERT_EQUALS(arrTypeOf(MAXKEY), MaxKey);
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
        ASSERT(ret.valid());
        ASSERT(ret.woCompare(fromjson("{a:'bcd',foo:{ggg:44},f:10}")) == 0);
    }
};

class DateBuilder {
public:
    void run() {
        BSONObj o = BSON("" << Date_t::fromMillisSinceEpoch(1234567890));
        ASSERT(o.firstElement().type() == Date);
        ASSERT(o.firstElement().date() == Date_t::fromMillisSinceEpoch(1234567890));
    }
};

class DateNowBuilder {
public:
    void run() {
        Date_t before = jsTime();
        BSONObj o = BSON("now" << DATENOW);
        Date_t after = jsTime();

        ASSERT(o.valid());

        BSONElement e = o["now"];
        ASSERT(e.type() == Date);
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

        ASSERT(o.valid());

        BSONElement e = o["now"];
        ASSERT_EQUALS(Date, e.type());
        ASSERT_EQUALS(aTime, e.date().toTimeT());
    }
};

class MinMaxKeyBuilder {
public:
    void run() {
        BSONObj min = BSON("a" << MINKEY);
        BSONObj max = BSON("b" << MAXKEY);

        ASSERT(min.valid());
        ASSERT(max.valid());

        BSONElement minElement = min["a"];
        BSONElement maxElement = max["b"];
        ASSERT(minElement.type() == MinKey);
        ASSERT(maxElement.type() == MaxKey);
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
        for (int t = 1; t < JSTypeMax; t++) {
            stringstream ss;
            ss << "type: " << t;
            string s = ss.str();
            ASSERT(min(t).woCompare(max(t)) <= 0);
            ASSERT(max(t).woCompare(min(t)) >= 0);
            ASSERT(min(t).woCompare(min(t)) == 0);
            ASSERT(max(t).woCompare(max(t)) == 0);
        }
    }
};

class ComparatorTest {
public:
    BSONObj one(string s) {
        return BSON("x" << s);
    }
    BSONObj two(string x, string y) {
        BSONObjBuilder b;
        b.append("x", x);
        if (y.size())
            b.append("y", y);
        else
            b.appendNull("y");
        return b.obj();
    }

    void test(BSONObj order, BSONObj l, BSONObj r, bool wanted) {
        BSONObjCmp c(order);
        bool got = c(l, r);
        if (got == wanted)
            return;
        cout << " order: " << order << " l: " << l << "r: " << r << " wanted: " << wanted
             << " got: " << got << endl;
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
    void t(FieldCompareResult res, const string& l, const string& r) {
        LexNumCmp cmp(true);
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
    void t(FieldCompareResult res, const string& l, const string& r) {
        LexNumCmp cmp(false);  // Specify numeric comparison for array field names.
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
        ASSERT_EQUALS(nested2dotted(nest), dot);
        ASSERT_EQUALS(nest, dotted2nested(dot));
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
        int i = 0;
        BSONObjBuilder objb;
        BSONArrayBuilder arrb;

        objb << objb.numStr(i++) << 100;
        arrb << 100;

        objb << objb.numStr(i++) << 1.0;
        arrb << 1.0;

        objb << objb.numStr(i++) << "Hello";
        arrb << "Hello";

        objb << objb.numStr(i++) << string("World");
        arrb << string("World");

        objb << objb.numStr(i++) << BSON("a" << 1 << "b"
                                             << "foo");
        arrb << BSON("a" << 1 << "b"
                         << "foo");

        objb << objb.numStr(i++) << BSON("a" << 1)["a"];
        arrb << BSON("a" << 1)["a"];

        OID oid;
        oid.init();
        objb << objb.numStr(i++) << oid;
        arrb << oid;

        objb.appendUndefined(objb.numStr(i++));
        arrb.appendUndefined();

        objb.appendRegex(objb.numStr(i++), "test", "imx");
        arrb.appendRegex("test", "imx");

        objb.appendBinData(objb.numStr(i++), 4, BinDataGeneral, "wow");
        arrb.appendBinData(4, BinDataGeneral, "wow");

        objb.appendCode(objb.numStr(i++), "function(){ return 1; }");
        arrb.appendCode("function(){ return 1; }");

        objb.appendCodeWScope(objb.numStr(i++), "function(){ return a; }", BSON("a" << 1));
        arrb.appendCodeWScope("function(){ return a; }", BSON("a" << 1));

        time_t dt(0);
        objb.appendTimeT(objb.numStr(i++), dt);
        arrb.appendTimeT(dt);

        Date_t date{};
        objb.appendDate(objb.numStr(i++), date);
        arrb.appendDate(date);

        objb.append(objb.numStr(i++), BSONRegEx("test2", "s"));
        arrb.append(BSONRegEx("test2", "s"));

        BSONObj obj = objb.obj();
        BSONArray arr = arrb.arr();

        ASSERT_EQUALS(obj, arr);

        BSONObj o = BSON("obj" << obj << "arr" << arr << "arr2" << BSONArray(obj) << "regex"
                               << BSONRegEx("reg", "x"));
        keyTest(o);

        ASSERT_EQUALS(o["obj"].type(), Object);
        ASSERT_EQUALS(o["arr"].type(), Array);
        ASSERT_EQUALS(o["arr2"].type(), Array);
        ASSERT_EQUALS(o["regex"].type(), RegEx);
    }
};

struct ArrayMacroTest {
    void run() {
        BSONArray arr = BSON_ARRAY("hello" << 1 << BSON("foo" << BSON_ARRAY("bar"
                                                                            << "baz"
                                                                            << "qux")));
        BSONObj obj = BSON("0"
                           << "hello"
                           << "1"
                           << 1
                           << "2"
                           << BSON("foo" << BSON_ARRAY("bar"
                                                       << "baz"
                                                       << "qux")));

        ASSERT_EQUALS(arr, obj);
        ASSERT_EQUALS(arr["2"].type(), Object);
        ASSERT_EQUALS(arr["2"].embeddedObject()["foo"].type(), Array);
    }
};

class bson2settest {
public:
    void run() {
        BSONObj o = BSON("z" << 1 << "a" << 2 << "m" << 3 << "c" << 4);
        BSONObjIteratorSorted i(o);
        stringstream ss;
        while (i.more())
            ss << i.next().fieldName();
        ASSERT_EQUALS("acmz", ss.str());

        {
            Timer t;
            for (int i = 0; i < 10000; i++) {
                BSONObjIteratorSorted j(o);
                int l = 0;
                while (j.more())
                    l += strlen(j.next().fieldName());
            }
            // unsigned long long tm = t.micros();
            // cout << "time: " << tm << endl;
        }

        BSONObj o2 = BSON("2"
                          << "a"
                          << "11"
                          << "b");
        BSONObjIteratorSorted i2(o2);
        // First field in sorted order should be "11" due use of a lexical comparison.
        ASSERT_EQUALS("11", string(i2.next().fieldName()));
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
            ASSERT_EQUALS(string(unsorted.next().fieldName()), sorted.next().fieldName());
        }
        ASSERT(!sorted.more());
    }
};

class checkForStorageTests {
public:
    void good(string s) {
        good(fromjson(s));
    }

    void good(BSONObj o) {
        if (o.okForStorageAsRoot())
            return;
        throw UserException(12528, (string) "should be ok for storage:" + o.toString());
    }

    void bad(string s) {
        bad(fromjson(s));
    }

    void bad(BSONObj o) {
        if (!o.okForStorageAsRoot())
            return;
        throw UserException(12529, (string) "should NOT be ok for storage:" + o.toString());
    }

    void run() {
        // basic docs are good
        good("{}");
        good("{x:1}");
        good("{x:{a:2}}");

        // no dots allowed
        bad("{'x.y':1}");
        bad("{'x\\.y':1}");

        // Check for $
        bad("{x:{'$a':2}}");
        good("{'a$b':2}");
        good("{'a$': {b: 2}}");
        good("{'a$':2}");
        good("{'a $ a': 'foo'}");

        // Queries are not ok
        bad("{num: {$gt: 1}}");
        bad("{_id: {$regex:'test'}}");
        bad("{$gt: 2}");
        bad("{a : { oo: [ {$bad:1}, {good:1}] }}");
        good("{a : { oo: [ {'\\\\$good':1}, {good:1}] }}");

        // DBRef stuff -- json parser can't handle this yet
        good(BSON("a" << BSON("$ref"
                              << "coll"
                              << "$id"
                              << 1)));
        good(BSON("a" << BSON("$ref"
                              << "coll"
                              << "$id"
                              << 1
                              << "$db"
                              << "a")));
        good(BSON("a" << BSON("$ref"
                              << "coll"
                              << "$id"
                              << 1
                              << "stuff"
                              << 1)));
        good(BSON("a" << BSON("$ref"
                              << "coll"
                              << "$id"
                              << 1
                              << "$db"
                              << "a"
                              << "stuff"
                              << 1)));

        bad(BSON("a" << BSON("$ref" << 1 << "$id" << 1)));
        bad(BSON("a" << BSON("$ref" << 1 << "$id" << 1 << "$db"
                                    << "a")));
        bad(BSON("a" << BSON("$ref"
                             << "coll"
                             << "$id"
                             << 1
                             << "$db"
                             << 1)));
        bad(BSON("a" << BSON("$ref"
                             << "coll")));
        bad(BSON("a" << BSON("$ref"
                             << "coll"
                             << "$db"
                             << "db")));
        bad(BSON("a" << BSON("$id" << 1)));
        bad(BSON("a" << BSON("$id" << 1 << "$ref"
                                   << "coll")));
        bad(BSON("a" << BSON("$ref"
                             << "coll"
                             << "$id"
                             << 1
                             << "$hater"
                             << 1)));
        bad(BSON("a" << BSON("$ref"
                             << "coll"
                             << "$id"
                             << 1
                             << "dot.dot"
                             << 1)));

        // _id isn't a RegEx, or Array
        good("{_id: 0}");
        good("{_id: {a:1, b:1}}");
        good("{_id: {rx: /a/}}");
        good("{_id: {rx: {$regex: 'a'}}}");
        bad("{_id: /a/ }");
        bad("{_id: /a/, other:1}");
        bad("{hi:1, _id: /a/ }");
        bad("{_id: /a/i }");
        bad("{first:/f/i, _id: /a/i }");
        // Not really a regex type
        bad("{_id: {$regex: 'a'} }");
        bad("{_id: {$regex: 'a', $options:'i'} }");
        bad("{_id:  [1,2]}");
        bad("{_id:  [1]}");
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
            ASSERT_EQUALS(x, y);
            free(crap);
        }

        {
            char* crap = (char*)mongoMalloc(x.objsize());
            memcpy(crap, x.objdata(), x.objsize());
            int* foo = (int*)crap;
            foo[0] = 123123123;
            int state = 0;
            try {
                BSONObj y(crap);
                state = 1;
            } catch (std::exception& e) {
                state = 2;
                ASSERT(strstr(e.what(), "_id: 5") != NULL);
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
        keyTest(x);
        keyTest(y);
        ASSERT_EQUALS(x, y);
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

            ASSERT_EQUALS(BSON("x" << 1 << "y" << 2 << "z" << 3), b.obj());
        }
    }
};

class BSONForEachTest {
public:
    void run() {
        BSONObj obj = BSON("a" << 1 << "a" << 2 << "a" << 3);

        int count = 0;
        BSONForEach(e, obj) {
            ASSERT_EQUALS(e.fieldName(), string("a"));
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

        ASSERT(!(a < b));
        ASSERT(a <= b);
        ASSERT(a < c);

        ASSERT(f > d);
        ASSERT(f >= e);
        ASSERT(!(f > e));
    }
};

class NestedBuilderOversize {
public:
    void run() {
        try {
            BSONObjBuilder outer;
            BSONObjBuilder inner(outer.subobjStart("inner"));

            string bigStr(1000, 'x');
            while (true) {
                ASSERT_LESS_THAN_OR_EQUALS(inner.len(), BufferMaxSize);
                inner.append("", bigStr);
            }

            ASSERT(!"Expected Throw");
        } catch (const DBException& e) {
            if (e.getCode() != 13548)  // we expect the code for oversized buffer
                throw;
        }
    }
};

class All : public Suite {
public:
    All() : Suite("jsobj") {}

    void setupTests() {
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
        add<BSONObjTests::AppendIntOrLL>();
        add<BSONObjTests::AppendNumber>();
        add<BSONObjTests::ToStringArray>();
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
        add<BSONObjTests::Validation::NoSize>(Symbol);
        add<BSONObjTests::Validation::NoSize>(Code);
        add<BSONObjTests::Validation::NoSize>(String);
        add<BSONObjTests::Validation::NoSize>(CodeWScope);
        add<BSONObjTests::Validation::NoSize>(DBRef);
        add<BSONObjTests::Validation::NoSize>(Object);
        add<BSONObjTests::Validation::NoSize>(Array);
        add<BSONObjTests::Validation::NoSize>(BinData);
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
        add<ValueStreamTests::Unallowed>();
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

SuiteInstance<All> myall;

}  // namespace JsobjTests
