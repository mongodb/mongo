/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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


#include "mongo/platform/basic.h"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fmt/format.h>
#include <fmt/printf.h>
#include <functional>
#include <map>
#include <random>
#include <signal.h>
#include <sstream>
#include <utility>
#include <vector>

#include "mongo/base/parse_number.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/config.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/hex.h"
#include "mongo/util/pcre.h"
#include "mongo/util/stacktrace.h"

/** `sigaltstack` was introduced in glibc-2.12 in 2010. */
#if !defined(_WIN32)
#define HAVE_SIGALTSTACK
#endif

#ifdef __linux__
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#endif  //  __linux__

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


// Needs to have linkage so we can test metadata. Needs to be extern
// "C" so it doesn't get mangled so we can name it with EXPORT_SYMBOLS
// in SConscript.
extern "C" MONGO_COMPILER_NOINLINE MONGO_COMPILER_API_EXPORT void
mongo_stacktrace_test_detail_testFunctionWithLinkage() {
    printf("...");
}

namespace mongo {

namespace stack_trace_test_detail {

struct RecursionParam {
    std::uint64_t n;
    std::function<void()> f;
};

MONGO_COMPILER_NOINLINE int recurseWithLinkage(RecursionParam& p, std::uint64_t i = 0) {
    if (i == p.n) {
        p.f();
    } else {
        recurseWithLinkage(p, i + 1);
    }
    return 0;
}

}  // namespace stack_trace_test_detail

namespace {

using namespace fmt::literals;
using namespace std::literals::chrono_literals;

constexpr bool kSuperVerbose = 0;  // Devel instrumentation

#if defined(_WIN32)
constexpr bool kIsWindows = true;
#else
constexpr bool kIsWindows = false;
#endif

class LogAdapter {
public:
    std::string toString() const {  // LAME
        std::ostringstream os;
        os << *this;
        return os.str();
    }
    friend std::ostream& operator<<(std::ostream& os, const LogAdapter& x) {
        x.doPrint(os);
        return os;
    }

private:
    virtual void doPrint(std::ostream& os) const = 0;
};

template <typename T>
class LogVec : public LogAdapter {
public:
    explicit LogVec(const T& v, StringData sep = ","_sd) : v(v), sep(sep) {}

private:
    void doPrint(std::ostream& os) const override {
        os << std::hex;
        os << "{";
        StringData s;
        for (auto&& e : v) {
            os << s << e;
            s = sep;
        }
        os << "}";
        os << std::dec;
    }
    const T& v;
    StringData sep = ","_sd;
};

uintptr_t fromHex(const std::string& s) {
    return static_cast<uintptr_t>(std::stoull(s, nullptr, 16));
}

bool consume(const pcre::Regex& re, StringData* in, std::string* out) {
    auto m = re.matchView(*in);
    if (!m)
        return false;
    *in = in->substr(m[0].size());
    *out = std::string{m[1]};
    return true;
}

// Break down a printStackTrace output for a contrived call tree and sanity-check it.
TEST(StackTrace, PosixFormat) {
    if (kIsWindows) {
        return;
    }

    std::string trace;
    stack_trace_test_detail::RecursionParam param{3, [&] {
                                                      StringStackTraceSink sink{trace};
                                                      printStackTrace(sink);
                                                  }};
    stack_trace_test_detail::recurseWithLinkage(param, 3);

    if (kSuperVerbose) {
        LOGV2_OPTIONS(24153, {logv2::LogTruncation::Disabled}, "Trace", "trace"_attr = trace);
    }

    // Expect log to be a "BACKTRACE:" 1-line record, followed by some "Frame:" lines.
    // Each "Frame:" line holds a full json object, but we only examine its "a" field here.
    std::string jsonLine;
    std::vector<uintptr_t> humanAddrs;
    StringData in{trace};
    static const pcre::Regex jsonLineRE(R"re(^BACKTRACE: (\{.*\})\n?)re");
    ASSERT_TRUE(consume(jsonLineRE, &in, &jsonLine)) << "\"" << in << "\"";
    while (true) {
        std::string frameLine;
        static const pcre::Regex frameRE(R"re(^  Frame: (\{.*\})\n?)re");
        if (!consume(frameRE, &in, &frameLine))
            break;
        BSONObj frameObj = fromjson(frameLine);  // throwy
        humanAddrs.push_back(fromHex(frameObj["a"].String()));
    }
    ASSERT_TRUE(in.empty()) << "must be consumed fully: \"" << in << "\"";

    BSONObj jsonObj = fromjson(jsonLine);  // throwy
    ASSERT_TRUE(jsonObj.hasField("backtrace"));
    ASSERT_TRUE(jsonObj.hasField("processInfo"));

    ASSERT_TRUE(jsonObj["processInfo"].Obj().hasField("somap"));
    struct SoMapEntry {
        uintptr_t base;
        std::string path;
    };

    std::map<uintptr_t, SoMapEntry> soMap;
    for (const auto& so : jsonObj["processInfo"]["somap"].Array()) {
        auto soObj = so.Obj();
        SoMapEntry ent{};
        ent.base = fromHex(soObj["b"].String());
        if (soObj.hasField("path")) {
            ent.path = soObj["path"].String();
        }
        soMap[ent.base] = ent;
    }

    // Sanity check: make sure all BACKTRACE addrs are represented in the Frame section.
    std::vector<uintptr_t> btAddrs;
    for (const auto& btElem : jsonObj["backtrace"].embeddedObject()) {
        btAddrs.push_back(fromHex(btElem.embeddedObject()["a"].String()));
    }

    // Mac OS backtrace returns extra frames in "backtrace".
    ASSERT_TRUE(std::search(btAddrs.begin(), btAddrs.end(), humanAddrs.begin(), humanAddrs.end()) ==
                btAddrs.begin())
        << LogVec(btAddrs) << " vs " << LogVec(humanAddrs);
}


std::vector<std::string> splitLines(std::string in) {
    std::vector<std::string> lines;
    while (true) {
        auto pos = in.find("\n");
        if (pos == std::string::npos) {
            break;
        } else {
            lines.push_back(in.substr(0, pos));
            in = in.substr(pos + 1);
        }
    }
    if (!in.empty()) {
        lines.push_back(in);
    }
    return lines;
}

TEST(StackTrace, WindowsFormat) {
    if (!kIsWindows) {
        return;
    }

    std::string trace = [&] {
        std::string s;
        stack_trace_test_detail::RecursionParam param{3, [&] {
                                                          StringStackTraceSink sink{s};
                                                          printStackTrace(sink);
                                                      }};
        stack_trace_test_detail::recurseWithLinkage(param);
        return s;
    }();

    std::vector<std::string> lines = splitLines(trace);

    auto re = pcre::Regex(R"re(^BACKTRACE: (\{.*\})$)re");
    auto m = re.matchView(lines[0]);
    ASSERT_TRUE(!!m);
    std::string jsonLine{m[1]};

    std::vector<uintptr_t> humanAddrs;
    for (size_t i = 1; i < lines.size(); ++i) {
        static const pcre::Regex re(R"re(^  Frame: (?:\{"a":"(.*?)",.*\})$)re");
        uintptr_t addr;
        auto m = re.matchView(lines[i]);
        ASSERT_TRUE(!!m) << lines[i];
        ASSERT_OK(NumberParser{}.base(16)(m[1], &addr));
        humanAddrs.push_back(addr);
    }

    BSONObj jsonObj = fromjson(jsonLine);  // throwy
    ASSERT_TRUE(jsonObj.hasField("backtrace")) << tojson(jsonObj);
    std::vector<uintptr_t> btAddrs;
    for (const auto& btElem : jsonObj["backtrace"].Obj()) {
        btAddrs.push_back(fromHex(btElem.Obj()["a"].String()));
    }

    ASSERT_TRUE(std::search(btAddrs.begin(), btAddrs.end(), humanAddrs.begin(), humanAddrs.end()) ==
                btAddrs.begin())
        << LogVec(btAddrs) << " vs " << LogVec(humanAddrs);
}

std::string traceString() {
    std::ostringstream os;
    printStackTrace(os);
    return os.str();
}

/** Emit a stack trace before main() runs, for use in the EarlyTraceSanity test. */
std::string earlyTrace = traceString();

/**
 * Verify that the JSON object emitted as part of a stack trace contains a "processInfo"
 * field, and that it, in turn, contains the fields expected by mongosymb.py. Verify in
 * particular that a stack trace emitted before main() and before the MONGO_INITIALIZERS
 * have run will be valid according to mongosymb.py.
 */
TEST(StackTrace, EarlyTraceSanity) {
    if (kIsWindows) {
        return;
    }

    const std::string trace = traceString();
    const std::string substrings[] = {
        R"("processInfo":)",
        R"("gitVersion":)",
        R"("compiledModules":)",
        R"("uname":)",
        R"("somap":)",
    };
    for (const auto& sub : substrings) {
        ASSERT_STRING_CONTAINS(earlyTrace, sub);
    }
    for (const auto& sub : substrings) {
        ASSERT_STRING_CONTAINS(trace, sub);
    }
}

#ifndef _WIN32
// `MetadataGenerator::load` should fill its meta member with something reasonable.
// Only testing with functions which we expect the dynamic linker to know about, so
// they must have external linkage.
TEST(StackTrace, MetadataGenerator) {
    StackTraceAddressMetadataGenerator gen;
    struct {
        void* ptr;
        std::string fileSub;
        std::string symbolSub;
    } const tests[] = {
        {
            reinterpret_cast<void*>(&mongo_stacktrace_test_detail_testFunctionWithLinkage),
            "stacktrace_test",
            "testFunctionWithLinkage",
        },
        {
            // printf's file tricky (surprises under ASAN, Mac, ...),
            // but we should at least get a symbol name containing "printf" out of it.
            reinterpret_cast<void*>(&std::printf),
            {},
            "printf",
        },
    };

    for (const auto& test : tests) {
        const auto& meta = gen.load(test.ptr);
        if (kSuperVerbose) {
            OstreamStackTraceSink sink{std::cout};
            meta.printTo(sink);
        }
        ASSERT_EQUALS(meta.address(), reinterpret_cast<uintptr_t>(test.ptr));
        if (!test.fileSub.empty()) {
            ASSERT_TRUE(meta.file());
            ASSERT_STRING_CONTAINS(meta.file().name(), test.fileSub);
        }

        if (!test.symbolSub.empty()) {
            ASSERT_TRUE(meta.symbol());
            ASSERT_STRING_CONTAINS(meta.symbol().name(), test.symbolSub);
        }
    }
}

TEST(StackTrace, MetadataGeneratorFunctionMeasure) {
    // Measure the size of a C++ function as a test of metadata retrieval.
    // Load increasing addresses until the metadata's symbol name changes.
    StackTraceAddressMetadataGenerator gen;
    void* fp = reinterpret_cast<void*>(&mongo_stacktrace_test_detail_testFunctionWithLinkage);
    const auto& meta = gen.load(fp);
    if (!meta.symbol())
        return;  // No symbol for `fp`. forget it.
    std::string savedSymbol{meta.symbol().name()};
    uintptr_t fBase = meta.symbol().base();
    ASSERT_EQ(fBase, reinterpret_cast<uintptr_t>(fp))
        << "function pointer should match its symbol base";
    size_t fSize = 0;
    for (; true; ++fSize) {
        auto& m = gen.load(reinterpret_cast<void*>(fBase + fSize));
        if (!m.symbol() || m.symbol().name() != savedSymbol)
            break;
    }
    // Place some reasonable expectation on the size of the tiny test function.
    ASSERT_GT(fSize, 0);
    ASSERT_LT(fSize, 512);
}
#endif  // _WIN32

#ifdef HAVE_SIGALTSTACK
extern "C" typedef void(sigAction_t)(int signum, siginfo_t* info, void* context);

class StackTraceSigAltStackTest : public unittest::Test {
public:
    using unittest::Test::Test;

    template <typename T>
    static std::string ostr(const T& v) {
        std::ostringstream os;
        os << v;
        return os.str();
    }

    static void handlerPreamble(int sig) {
        LOGV2(23387,
              "Thread caught signal!",
              "tid"_attr = ostr(stdx::this_thread::get_id()),
              "sig"_attr = sig);
        char storage;
        LOGV2(
            23388, "Local var", "var"_attr = "{:X}"_format(reinterpret_cast<uintptr_t>(&storage)));
    }

    static void tryHandler(sigAction_t* handler) {
        constexpr int sig = SIGUSR1;
        constexpr size_t kStackSize = size_t{1} << 20;  // 1 MiB
        auto buf = std::make_unique<std::array<unsigned char, kStackSize>>();
        constexpr unsigned char kSentinel = 0xda;
        std::fill(buf->begin(), buf->end(), kSentinel);
        LOGV2(24157,
              "sigaltstack buf",
              "size"_attr = "{:X}"_format(buf->size()),
              "data"_attr = "{:X}"_format(reinterpret_cast<uintptr_t>(buf->data())));
        stdx::thread thr([&] {
            LOGV2(23389, "Thread running", "tid"_attr = ostr(stdx::this_thread::get_id()));
            {
                stack_t ss;
                ss.ss_sp = buf->data();
                ss.ss_size = buf->size();
                ss.ss_flags = 0;
                if (int r = sigaltstack(&ss, nullptr); r < 0) {
                    perror("sigaltstack");
                }
            }
            {
                struct sigaction sa = {};
                sa.sa_sigaction = handler;
                sa.sa_mask = {};
                sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
                if (int r = sigaction(sig, &sa, nullptr); r < 0) {
                    perror("sigaction");
                }
            }
            raise(sig);
            {
                stack_t ss;
                ss.ss_sp = 0;
                ss.ss_size = 0;
                ss.ss_flags = SS_DISABLE;
                if (int r = sigaltstack(&ss, nullptr); r < 0) {
                    perror("sigaltstack");
                }
            }
        });
        thr.join();
        size_t used = std::distance(
            std::find_if(buf->begin(), buf->end(), [](unsigned char x) { return x != kSentinel; }),
            buf->end());
        LOGV2(23390, "Stack used", "bytes"_attr = used);
    }
};

extern "C" void stackTraceSigAltStackMinimalAction(int sig, siginfo_t*, void*) {
    StackTraceSigAltStackTest::handlerPreamble(sig);
}

TEST_F(StackTraceSigAltStackTest, Minimal) {
    StackTraceSigAltStackTest::tryHandler(stackTraceSigAltStackMinimalAction);
}

extern "C" void stackTraceSigAltStackPrintAction(int sig, siginfo_t*, void*) {
    StackTraceSigAltStackTest::handlerPreamble(sig);
    printStackTrace();
}

TEST_F(StackTraceSigAltStackTest, Print) {
    StackTraceSigAltStackTest::tryHandler(&stackTraceSigAltStackPrintAction);
}

extern "C" void stackTraceSigAltStackBacktraceAction(int sig, siginfo_t*, void*) {
    StackTraceSigAltStackTest::handlerPreamble(sig);
    std::array<void*, kStackTraceFrameMax> addrs;
    rawBacktrace(addrs.data(), addrs.size());
}

TEST_F(StackTraceSigAltStackTest, Backtrace) {
    StackTraceSigAltStackTest::tryHandler(stackTraceSigAltStackBacktraceAction);
}
#endif  // HAVE_SIGALTSTACK

class JsonTest : public unittest::Test {
public:
    using unittest::Test::Test;
    using Hex = stack_trace_detail::Hex;
    using Dec = stack_trace_detail::Dec;
};

TEST_F(JsonTest, Hex) {
    ASSERT_EQ(StringData(Hex(static_cast<void*>(0))), "0");
    ASSERT_EQ(StringData(Hex(0xffff)), "FFFF");
    ASSERT_EQ(Hex(0xfff0), "FFF0");
    ASSERT_EQ(Hex(0x8000'0000'0000'0000), "8000000000000000");
    ASSERT_EQ(Hex::fromHex("FFFF"), 0xffff);
    ASSERT_EQ(Hex::fromHex("0"), 0);
    ASSERT_EQ(Hex::fromHex("FFFFFFFFFFFFFFFF"), 0xffff'ffff'ffff'ffff);

    std::string s;
    StringStackTraceSink sink{s};
    sink << Hex(0xffff);
    ASSERT_EQ(s, R"(FFFF)");
}


#if defined(MONGO_STACKTRACE_CAN_DUMP_ALL_THREADS)
class PrintAllThreadStacksTest : public unittest::Test {
public:
    struct WatchInt {
        int v = 0;
        Mutex* m;
        stdx::condition_variable cond;

        void incr(int i) {
            stdx::unique_lock lock{*m};
            v += i;
            cond.notify_all();
        }
        void wait(int target) {
            stdx::unique_lock lock{*m};
            cond.wait(lock, [&] { return v == target; });
        }
    };

    struct Worker {
        stdx::thread thread;
        int tid;
        bool blocks = false;
    };

    void spawnWorker(bool blocksSignal = false) {
        pending.incr(1);
        auto& ref = workers.emplace_back();
        ref.thread = stdx::thread([&, blocksSignal] {
            ref.tid = syscall(SYS_gettid);
            ref.blocks = blocksSignal;
            if (blocksSignal) {
                sigset_t mask;
                sigemptyset(&mask);
                sigaddset(&mask, SIGUSR2);
                pthread_sigmask(SIG_BLOCK, &mask, nullptr);
            }
            pending.incr(-1);
            endAll.wait(1);
        });
        pending.wait(0);
    }

    void reapWorkers() {
        endAll.incr(1);
        for (auto& w : workers)
            w.thread.join();
    }

    void doPrintAllThreadStacks(size_t numThreads) {
        for (size_t i = 0; i < numThreads; ++i)
            spawnWorker();

        std::string dumped;
        StringStackTraceSink sink{dumped};
        printAllThreadStacks(sink);
        if (kSuperVerbose)
            LOGV2_OPTIONS(
                24156, {logv2::LogTruncation::Disabled}, "Dumped", "dumped"_attr = dumped);

        reapWorkers();

        std::set<int> seenTids;

        // Make some assertions about `dumped`.
        BSONObj jsonObj = fromjson(dumped);
        auto allInfoElement = jsonObj.getObjectField("threadInfo");
        for (const auto& ti : allInfoElement) {
            const BSONObj& obj = ti.Obj();
            seenTids.insert(obj.getIntField("tid"));
            ASSERT(obj.hasElement("backtrace"));
        }

        for (auto&& w : workers)
            ASSERT(seenTids.find(w.tid) != seenTids.end()) << "missing tid:" << w.tid;
    }

    Mutex mutex;
    WatchInt endAll{0, &mutex};
    WatchInt pending{0, &mutex};
    std::deque<Worker> workers;
};

TEST_F(PrintAllThreadStacksTest, WithDeadThreads) {
    for (int i = 0; i < 2; ++i)
        spawnWorker(true);
    for (int i = 0; i < 2; ++i)
        spawnWorker(false);

    std::string dumped;

    stdx::thread dumper([&] {
        StringStackTraceSink sink{dumped};
        printAllThreadStacks(sink);
    });

    // Give tracer some time to signal all the threads.
    // We know the threads with the signal blocked will not respond.
    // The dumper thread will poll until those are dead.
    // There's no API to monitor its progress, so sleep a hefty chunk of time.
    sleep(2);

    reapWorkers();

    dumper.join();

    BSONObj jsonObj = fromjson(dumped);
    std::map<int, BSONObj> tidDumps;  // All are references into jsonObj.

    std::set<int> mustSee;
    for (const auto& w : workers)
        mustSee.insert(w.tid);
    for (const auto& el : jsonObj.getObjectField("missedThreadIds"))
        mustSee.erase(el.Int());

    auto allInfoElement = jsonObj.getObjectField("threadInfo");
    for (const auto& ti : allInfoElement) {
        const BSONObj& obj = ti.Obj();
        int tid = obj.getIntField("tid");
        mustSee.erase(tid);
        auto witer =
            std::find_if(workers.begin(), workers.end(), [&](auto&& w) { return w.tid == tid; });
        if (witer == workers.end())
            continue;
        bool missingBacktrace = !obj.hasElement("backtrace");
        ASSERT_EQ(witer->blocks, missingBacktrace);
    }
    ASSERT(mustSee.empty()) << format(FMT_STRING("tids missing from report: {}"),
                                      fmt::join(mustSee, ","));
}

TEST_F(PrintAllThreadStacksTest, Go_2_Threads) {
    doPrintAllThreadStacks(2);
}
TEST_F(PrintAllThreadStacksTest, Go_20_Threads) {
    doPrintAllThreadStacks(20);
}
TEST_F(PrintAllThreadStacksTest, Go_200_Threads) {
    doPrintAllThreadStacks(200);
}

#endif  // defined(MONGO_STACKTRACE_CAN_DUMP_ALL_THREADS)

#if defined(MONGO_CONFIG_USE_LIBUNWIND) || defined(MONGO_CONFIG_HAVE_EXECINFO_BACKTRACE)
/**
 * Try to backtrace from a stack containing a libc function. To do this
 * we need a libc function that makes a user-provided callback, like qsort.
 */
TEST(StackTrace, BacktraceThroughLibc) {
    struct Result {
        void notify() {
            if (called)
                return;
            called = true;
            arrSize = rawBacktrace(arr.data(), arr.size());
        }
        bool called = 0;
        std::array<void*, kStackTraceFrameMax> arr;
        size_t arrSize;
    };
    static Result capture;
    std::array<int, 2> arr{{}};
    qsort(arr.data(), arr.size(), sizeof(arr[0]), [](const void* a, const void* b) {
        // Order them by position in the array.
        capture.notify();
        return static_cast<int>(static_cast<const int*>(a) < static_cast<const int*>(b));
    });
    LOGV2(23391, "Captured", "frameCount"_attr = capture.arrSize);
    for (size_t i = 0; i < capture.arrSize; ++i) {
        LOGV2(23392,
              "Frame",
              "i"_attr = i,
              "frame"_attr = "{:X}"_format(reinterpret_cast<uintptr_t>(capture.arr[i])));
    }
}
#endif  // mongo stacktrace backend


}  // namespace
}  // namespace mongo
