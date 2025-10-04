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

#include "mongo/base/string_data.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/base64.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/queue.h"
#include "mongo/util/str.h"
#include "mongo/util/text.h"  // IWYU pragma: keep
#include "mongo/util/timer.h"

#include <cstddef>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <absl/strings/str_join.h>
#include <absl/strings/str_split.h>

namespace mongo {
namespace BasicTests {

using std::cout;
using std::dec;
using std::endl;
using std::hex;
using std::string;
using std::stringstream;
using std::vector;

class RarelyTest {
public:
    void run() {
        int first = 0;
        int second = 0;
        for (int i = 0; i < 128; ++i) {
            incRarely(first);
            incRarely2(second);
        }
        ASSERT_EQUALS(1, first);
        ASSERT_EQUALS(1, second);
    }

private:
    void incRarely(int& c) {
        static mongo::Rarely s;
        if (s.tick())
            ++c;
    }
    void incRarely2(int& c) {
        static mongo::Rarely s;
        if (s.tick())
            ++c;
    }
};

class Base64Tests {
public:
    void roundTrip(string s) {
        ASSERT_EQUALS(s, base64::decode(base64::encode(s)));
    }

    void roundTrip(const unsigned char* _data, int len) {
        const char* data = (const char*)_data;
        string s = base64::encode(StringData(data, len));
        string out = base64::decode(s);
        ASSERT_EQUALS(out.size(), static_cast<size_t>(len));
        bool broke = false;
        for (int i = 0; i < len; i++) {
            if (data[i] != out[i])
                broke = true;
        }
        if (!broke)
            return;

        cout << s << endl;
        for (int i = 0; i < len; i++)
            cout << hex << (data[i] & 0xFF) << dec << " ";
        cout << endl;
        for (int i = 0; i < len; i++)
            cout << hex << (out[i] & 0xFF) << dec << " ";
        cout << endl;

        ASSERT(0);
    }

    void run() {
        ASSERT_EQUALS("ZWxp", base64::encode("eli"_sd));
        ASSERT_EQUALS("ZWxpb3Rz", base64::encode("eliots"_sd));
        ASSERT_EQUALS("ZWxpb3Rz", base64::encode("eliots"));

        ASSERT_EQUALS("ZQ==", base64::encode("e"_sd));
        ASSERT_EQUALS("ZWw=", base64::encode("el"_sd));

        roundTrip("e");
        roundTrip("el");
        roundTrip("eli");
        roundTrip("elio");
        roundTrip("eliot");
        roundTrip("eliots");
        roundTrip("eliotsz");

        unsigned char z[] = {0x1, 0x2, 0x3, 0x4};
        roundTrip(z, 4);

        unsigned char y[] = {0x01, 0x10, 0x83, 0x10, 0x51, 0x87, 0x20, 0x92, 0x8B, 0x30,
                             0xD3, 0x8F, 0x41, 0x14, 0x93, 0x51, 0x55, 0x97, 0x61, 0x96,
                             0x9B, 0x71, 0xD7, 0x9F, 0x82, 0x18, 0xA3, 0x92, 0x59, 0xA7,
                             0xA2, 0x9A, 0xAB, 0xB2, 0xDB, 0xAF, 0xC3, 0x1C, 0xB3, 0xD3,
                             0x5D, 0xB7, 0xE3, 0x9E, 0xBB, 0xF3, 0xDF, 0xBF};
        roundTrip(y, 4);
        roundTrip(y, 40);
    }
};

namespace stringbuildertests {
#define SBTGB(x) \
    ss << (x);   \
    sb << (x);

class Base {
    virtual void pop() = 0;

public:
    Base() {}
    virtual ~Base() {}

    void run() {
        pop();
        ASSERT_EQUALS(ss.str(), sb.str());
    }

    stringstream ss;
    StringBuilder sb;
};

class simple1 : public Base {
    void pop() override {
        SBTGB(1);
        SBTGB("yo");
        SBTGB(2);
    }
};

class simple2 : public Base {
    void pop() override {
        SBTGB(1);
        SBTGB("yo");
        SBTGB(2);
        SBTGB(12123123123LL);
        SBTGB("xxx");
        SBTGB(5.4);
        SBTGB(5.4312);
        SBTGB("yyy");
        SBTGB((short)5);
        SBTGB((short)(1231231231231LL));
    }
};

class reset1 {
public:
    void run() {
        StringBuilder sb;
        sb << "1"
           << "abc"
           << "5.17";
        ASSERT_EQUALS("1abc5.17", sb.str());
        ASSERT_EQUALS("1abc5.17", sb.str());
        sb.reset();
        ASSERT_EQUALS("", sb.str());
        sb << "999";
        ASSERT_EQUALS("999", sb.str());
    }
};

class reset2 {
public:
    void run() {
        StringBuilder sb;
        sb << "1"
           << "abc"
           << "5.17";
        ASSERT_EQUALS("1abc5.17", sb.str());
        ASSERT_EQUALS("1abc5.17", sb.str());
        sb.reset(1);
        ASSERT_EQUALS("", sb.str());
        sb << "999";
        ASSERT_EQUALS("999", sb.str());
    }
};
}  // namespace stringbuildertests

class AssertTests {
public:
    int x;

    AssertTests() {
        x = 0;
    }

    string foo() {
        x++;
        return "";
    }
    void run() {
        uassert(-1, foo(), 1);
        if (x != 0) {
            ASSERT_EQUALS(0, x);
        }
        try {
            uassert(-1, foo(), 0);
        } catch (...) {
        }
        ASSERT_EQUALS(1, x);
    }
};

struct StringSplitterTest {
    void test(string s) {
        vector<string> v = absl::StrSplit(s, ",");
        ASSERT_EQUALS(s, absl::StrJoin(v, ","));
    }

    void run() {
        test("a");
        test("a,b");
        test("a,b,c");

        vector<string> x = absl::StrSplit("axbxc", "x", absl::SkipEmpty());
        ASSERT_EQUALS(3, (int)x.size());
        ASSERT_EQUALS("a", x[0]);
        ASSERT_EQUALS("b", x[1]);
        ASSERT_EQUALS("c", x[2]);

        x = absl::StrSplit("axxbxxc", "xx", absl::SkipEmpty());
        ASSERT_EQUALS(3, (int)x.size());
        ASSERT_EQUALS("a", x[0]);
        ASSERT_EQUALS("b", x[1]);
        ASSERT_EQUALS("c", x[2]);

        x = absl::StrSplit("xxaxxxxbxxcxx", "xx", absl::SkipEmpty());
        ASSERT_EQUALS(3, (int)x.size());
        ASSERT_EQUALS("a", x[0]);
        ASSERT_EQUALS("b", x[1]);
        ASSERT_EQUALS("c", x[2]);
    }
};

struct IsValidUTF8Test {
// macros used to get valid line numbers
#define good(s) ASSERT(isValidUTF8(s));
#define bad(s) ASSERT(!isValidUTF8(s));

    void run() {
        good("A");
        good("\xC2\xA2");          // cent: ¢
        good("\xE2\x82\xAC");      // euro: €
        good("\xF0\x9D\x90\x80");  // Blackboard A: 𝐀

        // abrupt end
        bad("\xC2");
        bad("\xE2\x82");
        bad("\xF0\x9D\x90");
        bad("\xC2 ");
        bad("\xE2\x82 ");
        bad("\xF0\x9D\x90 ");

        // too long
        bad("\xF8\x80\x80\x80\x80");
        bad("\xFC\x80\x80\x80\x80\x80");
        bad("\xFE\x80\x80\x80\x80\x80\x80");
        bad("\xFF\x80\x80\x80\x80\x80\x80\x80");

        bad("\xF5\x80\x80\x80");  // U+140000 > U+10FFFF
        bad("\x80");              // cant start with continuation byte
        bad("\xC0\x80");          // 2-byte version of ASCII NUL
#undef good
#undef bad
    }
};


class QueueTest {
public:
    void run() {
        BlockingQueue<int> q;
        Timer t;
        int x;
        ASSERT(!q.blockingPop(x, 5));
        ASSERT(t.seconds() > 3);
    }
};

class StrTests {
public:
    void run() {
        ASSERT_EQUALS(1u, str::count("abc", 'b'));
        ASSERT_EQUALS(3u, str::count("babab", 'b'));
    }
};

class HostAndPortTests {
public:
    void run() {
        HostAndPort a("x1", 1000);
        HostAndPort b("x1", 1000);
        HostAndPort c("x1", 1001);
        HostAndPort d("x2", 1000);

        ASSERT(a == b);
        ASSERT(a != c);
        ASSERT(a != d);
    }
};

class All : public unittest::OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("basic") {}

    void setupTests() override {
        add<RarelyTest>();
        add<Base64Tests>();

        add<stringbuildertests::simple1>();
        add<stringbuildertests::simple2>();
        add<stringbuildertests::reset1>();
        add<stringbuildertests::reset2>();

        add<AssertTests>();

        add<StringSplitterTest>();
        add<IsValidUTF8Test>();

        add<QueueTest>();

        add<StrTests>();

        add<HostAndPortTests>();
    }
};

unittest::OldStyleSuiteInitializer<All> myall;

}  // namespace BasicTests
}  // namespace mongo
