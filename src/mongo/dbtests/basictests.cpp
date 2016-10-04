// basictests.cpp : basic unit tests
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

#include "mongo/platform/basic.h"

#include <iostream>

#include "mongo/db/client.h"
#include "mongo/db/storage/mmap_v1/compress.h"
#include "mongo/db/storage/paths.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/util/base64.h"
#include "mongo/util/queue.h"
#include "mongo/util/stringutils.h"
#include "mongo/util/text.h"
#include "mongo/util/thread_safe_string.h"
#include "mongo/util/time_support.h"

namespace BasicTests {

using std::unique_ptr;
using std::shared_ptr;
using std::cout;
using std::dec;
using std::endl;
using std::hex;
using std::string;
using std::stringstream;
using std::vector;

class Rarely {
public:
    void run() {
        int first = 0;
        int second = 0;
        int third = 0;
        for (int i = 0; i < 128; ++i) {
            incRarely(first);
            incRarely2(second);
            ONCE++ third;
        }
        ASSERT_EQUALS(1, first);
        ASSERT_EQUALS(1, second);
        ASSERT_EQUALS(1, third);
    }

private:
    void incRarely(int& c) {
        RARELY++ c;
    }
    void incRarely2(int& c) {
        RARELY++ c;
    }
};

class Base64Tests {
public:
    void roundTrip(string s) {
        ASSERT_EQUALS(s, base64::decode(base64::encode(s)));
    }

    void roundTrip(const unsigned char* _data, int len) {
        const char* data = (const char*)_data;
        string s = base64::encode(data, len);
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
        ASSERT_EQUALS("ZWxp", base64::encode("eli", 3));
        ASSERT_EQUALS("ZWxpb3Rz", base64::encode("eliots", 6));
        ASSERT_EQUALS("ZWxpb3Rz", base64::encode("eliots"));

        ASSERT_EQUALS("ZQ==", base64::encode("e", 1));
        ASSERT_EQUALS("ZWw=", base64::encode("el", 2));

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
    void pop() {
        SBTGB(1);
        SBTGB("yo");
        SBTGB(2);
    }
};

class simple2 : public Base {
    void pop() {
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
}

class sleeptest {
public:
    void run() {
        Timer t;
        int matches = 0;
        for (int p = 0; p < 3; p++) {
            sleepsecs(1);
            int sec = (t.millis() + 2) / 1000;
            if (sec == 1)
                matches++;
            else
                mongo::unittest::log() << "temp millis: " << t.millis() << endl;
            ASSERT(sec >= 0 && sec <= 2);
            t.reset();
        }
        if (matches < 2)
            mongo::unittest::log() << "matches:" << matches << endl;
        ASSERT(matches >= 2);

        sleepmicros(1527123);
        ASSERT(t.micros() > 1000000);
        ASSERT(t.micros() < 2000000);

        t.reset();
        sleepmillis(1727);
        ASSERT(t.millis() >= 1000);
        ASSERT(t.millis() <= 2500);

        {
            int total = 1200;
            int ms = 2;
            t.reset();
            for (int i = 0; i < (total / ms); i++) {
                sleepmillis(ms);
            }
            {
                int x = t.millis();
                if (x < 1000 || x > 2500) {
                    cout << "sleeptest finds sleep accuracy to be not great. x: " << x << endl;
                    ASSERT(x >= 1000);
                    ASSERT(x <= 20000);
                }
            }
        }

#ifdef __linux__
        {
            int total = 1200;
            int micros = 100;
            t.reset();
            int numSleeps = 1000 * (total / micros);
            for (int i = 0; i < numSleeps; i++) {
                sleepmicros(micros);
            }
            {
                int y = t.millis();
                if (y < 1000 || y > 2500) {
                    cout << "sleeptest y: " << y << endl;
                    ASSERT(y >= 1000);
                    /* ASSERT( y <= 100000 ); */
                }
            }
        }
#endif
    }
};

class SleepBackoffTest {
public:
    void run() {
        int maxSleepTimeMillis = 1000;

        Backoff backoff(maxSleepTimeMillis, maxSleepTimeMillis * 2);

        // Double previous sleep duration
        ASSERT_EQUALS(backoff.getNextSleepMillis(0, 0, 0), 1);
        ASSERT_EQUALS(backoff.getNextSleepMillis(2, 0, 0), 4);
        ASSERT_EQUALS(backoff.getNextSleepMillis(256, 0, 0), 512);

        // Make sure our backoff increases to the maximum value
        ASSERT_EQUALS(backoff.getNextSleepMillis(maxSleepTimeMillis - 200, 0, 0),
                      maxSleepTimeMillis);
        ASSERT_EQUALS(backoff.getNextSleepMillis(maxSleepTimeMillis * 2, 0, 0), maxSleepTimeMillis);

        // Make sure that our backoff gets reset if we wait much longer than the maximum wait
        unsigned long long resetAfterMillis = maxSleepTimeMillis + maxSleepTimeMillis * 2;
        ASSERT_EQUALS(backoff.getNextSleepMillis(20, resetAfterMillis, 0), 40);  // no reset here
        ASSERT_EQUALS(backoff.getNextSleepMillis(20, resetAfterMillis + 1, 0),
                      1);  // reset expected
    }
};

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

class ThreadSafeStringTest {
public:
    void run() {
        ThreadSafeString s;
        s = "eliot";
        ASSERT_EQUALS(s.toString(), "eliot");
        ASSERT(s.toString() != "eliot2");

        ThreadSafeString s2;
        s2 = s.toString().c_str();
        ASSERT_EQUALS(s2.toString(), "eliot");


        {
            string foo;
            {
                ThreadSafeString bar;
                bar = "eliot2";
                foo = bar.toString();
            }
            ASSERT_EQUALS("eliot2", foo);
        }
    }
};

struct StringSplitterTest {
    void test(string s) {
        vector<string> v = StringSplitter::split(s, ",");
        ASSERT_EQUALS(s, StringSplitter::join(v, ","));
    }

    void run() {
        test("a");
        test("a,b");
        test("a,b,c");

        vector<string> x = StringSplitter::split("axbxc", "x");
        ASSERT_EQUALS(3, (int)x.size());
        ASSERT_EQUALS("a", x[0]);
        ASSERT_EQUALS("b", x[1]);
        ASSERT_EQUALS("c", x[2]);

        x = StringSplitter::split("axxbxxc", "xx");
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
        ASSERT(t.seconds() > 3 && t.seconds() < 9);
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

class RelativePathTest {
public:
    void run() {
        RelativePath a = RelativePath::fromRelativePath("a");
        RelativePath b = RelativePath::fromRelativePath("a");
        RelativePath c = RelativePath::fromRelativePath("b");
        RelativePath d = RelativePath::fromRelativePath("a/b");


        ASSERT(a == b);
        ASSERT(a != c);
        ASSERT(a != d);
        ASSERT(c != d);
    }
};

struct CompressionTest1 {
    void run() {
        const char* c = "this is a test";
        std::string s;
        size_t len = compress(c, strlen(c) + 1, &s);
        verify(len > 0);

        std::string out;
        bool ok = uncompress(s.c_str(), s.size(), &out);
        verify(ok);
        verify(strcmp(out.c_str(), c) == 0);
    }
} ctest1;

class All : public Suite {
public:
    All() : Suite("basic") {}

    void setupTests() {
        add<Rarely>();
        add<Base64Tests>();

        add<stringbuildertests::simple1>();
        add<stringbuildertests::simple2>();
        add<stringbuildertests::reset1>();
        add<stringbuildertests::reset2>();

        add<sleeptest>();
        add<SleepBackoffTest>();
        add<AssertTests>();

        add<StringSplitterTest>();
        add<IsValidUTF8Test>();

        add<QueueTest>();

        add<StrTests>();

        add<HostAndPortTests>();
        add<RelativePathTest>();

        add<CompressionTest1>();
    }
};

SuiteInstance<All> myall;

}  // namespace BasicTests
