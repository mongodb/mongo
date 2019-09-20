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

#include <cstdio>
#include <cstdlib>
#include <fmt/format.h>
#include <fmt/printf.h>
#include <functional>
#include <regex>
#include <sstream>
#include <vector>

#include "mongo/base/backtrace_visibility_test.h"
#include "mongo/bson/json.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/stacktrace.h"


namespace mongo {

namespace {

using namespace fmt::literals;

constexpr bool kSuperVerbose = 0;  // Devel instrumentation

#if defined(_WIN32)
constexpr bool kIsWindows = true;
#else
constexpr bool kIsWindows = false;
#endif

struct RecursionParam {
    std::ostream& out;
    std::vector<std::function<void(RecursionParam&)>> stack;
};

// Pops a callable and calls it. printStackTrace when we're out of callables.
// Calls itself a few times to synthesize a nice big call stack.
MONGO_COMPILER_NOINLINE void recursionTest(RecursionParam& p) {
    if (p.stack.empty()) {
        // I've come to invoke `stack` elements and test `printStackTrace()`,
        // and I'm all out of `stack` elements.
        printStackTrace(p.out);
        return;
    }
    auto f = std::move(p.stack.back());
    p.stack.pop_back();
    f(p);
}

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

class LogJson : public LogAdapter {
public:
    explicit LogJson(const BSONObj& obj) : obj(obj) {}

private:
    void doPrint(std::ostream& os) const override {
        os << tojson(obj, Strict, /*pretty=*/true);
    }
    const BSONObj& obj;
};

auto tlog() {
    auto r = unittest::log();
    r.setIsTruncatable(false);
    return r;
}

uintptr_t fromHex(const std::string& s) {
    return static_cast<uintptr_t>(std::stoull(s, nullptr, 16));
}

std::string getBaseName(std::string path) {
    size_t lastSlash = path.rfind('/');
    if (lastSlash == std::string::npos)
        return path;
    return path.substr(lastSlash + 1);
}

struct HumanFrame {
    uintptr_t addr;
    std::string soFileName;
    uintptr_t soFileOffset;
    std::string symbolName;
    uintptr_t symbolOffset;
};

std::vector<HumanFrame> parseTraceBody(const std::string& traceBody) {
    std::vector<HumanFrame> r;
    // Three choices:
    //   just raw:      " ???[0x7F0A71AD4238]"
    //   just soFile:   " libfoo.so(+0xABC408)[0x7F0A71AD4238]"
    //   soFile + symb: " libfoo.so(someSym+0x408)[0x7F0A71AD4238]"
    const std::regex re(R"re( ()re"                              // line pattern open
                        R"re((?:)re"                             // choice open non-capturing
                        R"re((\?\?\?))re"                        // capture just raw case "???"
                        R"re(|)re"                               // next choice
                        R"re(([^(]*)\((.*)\+(0x[0-9A-F]+)\))re"  // so "(" sym offset ")"
                        R"re())re"                               // choice close
                        R"re( \[(0x[0-9A-F]+)\])re"              // raw addr suffix
                        R"re()\n)re");                           // line pattern close, newline
    for (auto i = std::sregex_iterator(traceBody.begin(), traceBody.end(), re);
         i != std::sregex_iterator();
         ++i) {
        if (kSuperVerbose) {
            tlog() << "{";
            for (size_t ei = 1; ei < i->size(); ++ei) {
                tlog() << "  {:2d}: `{}`"_format(ei, (*i)[ei]);
            }
            tlog() << "}";
        }
        size_t patternIdx = 1;
        std::string line = (*i)[patternIdx++];
        std::string rawOnly = (*i)[patternIdx++];  // "???" or empty
        std::string soFile = (*i)[patternIdx++];
        std::string symbol = (*i)[patternIdx++];
        std::string offset = (*i)[patternIdx++];
        std::string addr = (*i)[patternIdx++];
        if (kSuperVerbose) {
            tlog() << "    rawOnly:`{}`, soFile:`{}`, symbol:`{}`, offset: `{}`, addr:`{}`"
                      ""_format(rawOnly, soFile, symbol, offset, addr);
        }
        HumanFrame hf{};
        hf.addr = fromHex(addr);
        if (rawOnly.empty()) {
            // known soFile
            hf.soFileName = soFile;
            if (symbol.empty()) {
                hf.soFileOffset = fromHex(offset);
            } else {
                // known symbol
                hf.symbolName = symbol;
                hf.symbolOffset = fromHex(offset);
            }
        }
        r.push_back(hf);
    }
    return r;
}

// Break down a printStackTrace output for a contrived call tree and sanity-check it.
TEST(StackTrace, PosixFormat) {
    if (kIsWindows) {
        return;
    }

    const std::string trace = [&] {
        std::ostringstream os;
        RecursionParam param{os, {3, recursionTest}};
        recursionTest(param);
        return os.str();
    }();

    if (kSuperVerbose) {
        tlog() << "trace:{" << trace << "}";
    }

    std::smatch m;
    ASSERT_TRUE(
        std::regex_match(trace,
                         m,
                         std::regex(R"re(((?: [0-9A-F]+)+)\n)re"  // raw void* list `addrLine`
                                    R"re(----- BEGIN BACKTRACE -----\n)re"  // header line
                                    R"re((.*)\n)re"                         // huge `jsonLine`
                                    R"re(((?:.*\n)+))re"  // multi-line human-readable `traceBody`
                                    R"re(-----  END BACKTRACE  -----\n)re")))  // footer line
        << "trace: {}"_format(trace);
    std::string addrLine = m[1].str();
    std::string jsonLine = m[2].str();
    std::string traceBody = m[3].str();

    if (kSuperVerbose) {
        tlog() << "addrLine:{" << addrLine << "}";
        tlog() << "jsonLine:{" << jsonLine << "}";
        tlog() << "traceBody:{" << traceBody << "}";
    }

    std::vector<uintptr_t> addrs;
    {
        const std::regex re(R"re( ([0-9A-F]+))re");
        for (auto i = std::sregex_iterator(addrLine.begin(), addrLine.end(), re);
             i != std::sregex_iterator();
             ++i) {
            addrs.push_back(fromHex((*i)[1]));
        }
    }
    if (kSuperVerbose) {
        tlog() << "addrs[] = " << LogVec(addrs);
    }

    BSONObj jsonObj = fromjson(jsonLine);  // throwy
    ASSERT_TRUE(jsonObj.hasField("backtrace"));
    ASSERT_TRUE(jsonObj.hasField("processInfo"));

    if (kSuperVerbose) {
        for (auto& elem : jsonObj["backtrace"].Array()) {
            tlog() << "  btelem=" << LogJson(elem.Obj());
        }
        tlog() << "  processInfo=" << LogJson(jsonObj["processInfo"].Obj());
    }

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

    std::vector<HumanFrame> humanFrames = parseTraceBody(traceBody);

    {
        // Extract all the humanFrames .addr into a vector and match against the addr line.
        std::vector<uintptr_t> humanAddrs;
        std::transform(humanFrames.begin(),
                       humanFrames.end(),
                       std::back_inserter(humanAddrs),
                       [](auto&& h) { return h.addr; });
        ASSERT_TRUE(addrs == humanAddrs) << LogVec(addrs) << " vs " << LogVec(humanAddrs);
    }

    {
        // Match humanFrames against the "backtrace" json array
        auto btArray = jsonObj["backtrace"].Array();
        for (size_t i = 0; i < humanFrames.size(); ++i) {
            const auto& hf = humanFrames[i];
            const BSONObj& bt = btArray[i].Obj();
            ASSERT_TRUE(bt.hasField("b"));
            ASSERT_TRUE(bt.hasField("o"));
            ASSERT_EQUALS(!hf.symbolName.empty(), bt.hasField("s"));

            if (!hf.soFileName.empty()) {
                uintptr_t btBase = fromHex(bt["b"].String());
                auto soEntryIter = soMap.find(btBase);
                ASSERT_TRUE(soEntryIter != soMap.end()) << "not in soMap: 0x{:X}"_format(btBase);
                std::string soPath = getBaseName(soEntryIter->second.path);
                if (soPath.empty()) {
                    // As a special case, the "main" shared object has an empty soPath.
                } else {
                    ASSERT_EQUALS(hf.soFileName, soPath)
                        << "hf.soFileName:`{}`,soPath:`{}`"_format(hf.soFileName, soPath);
                }
            }
            if (!hf.symbolName.empty()) {
                ASSERT_EQUALS(hf.symbolName, bt["s"].String());
            }
        }
    }
}

TEST(StackTrace, WindowsFormat) {
    if (!kIsWindows) {
        return;
    }

    // TODO: rough: string parts are not escaped and can contain the ' ' delimiter.
    const std::string trace = [&] {
        std::ostringstream os;
        RecursionParam param{os, {3, recursionTest}};
        recursionTest(param);
        return os.str();
    }();
    const std::regex re(R"re(()re"                  // line pattern open
                        R"re(([^\\]?))re"           // moduleName: cannot have backslashes
                        R"re(\s*)re"                // pad
                        R"re(.*)re"                 // sourceAndLine: empty, or "...\dir\file(line)"
                        R"re(\s*)re"                // pad
                        R"re((?:)re"                // symbolAndOffset: choice open non-capturing
                        R"re(\?\?\?)re"             //     raw case: "???"
                        R"re(|)re"                  //   or
                        R"re((.*)\+0x[0-9a-f]*)re"  //     "symbolname+0x" lcHex...
                        R"re())re"                  // symbolAndOffset: choice close
                        R"re()\n)re");              // line pattern close, newline
    auto mark = trace.begin();
    for (auto i = std::sregex_iterator(trace.begin(), trace.end(), re); i != std::sregex_iterator();
         ++i) {
        mark = (*i)[0].second;
    }
    ASSERT_TRUE(mark == trace.end())
        << "cannot match suffix: `" << trace.substr(mark - trace.begin()) << "` "
        << "of trace: `" << trace << "`";
}

}  // namespace
}  // namespace mongo
