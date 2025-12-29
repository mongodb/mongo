/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/unittest/unittest_options.h"

#include "mongo/base/parse_number.h"
#include "mongo/base/status.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/exit_code.h"

#include <any>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <stdio.h>
#else
#include <unistd.h>
#endif

#include <variant>

#include <boost/filesystem.hpp>
#include <boost/optional.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::unittest {
namespace {

int strToInt(StringData s) {
    int i;
    if (auto st = NumberParser{}(s, &i); !st.isOK())
        iasserted(st);
    return i;
}

bool strToBool(StringData s) {
    if (s == "1" || s == "true")
        return true;
    if (s == "0" || s == "false")
        return false;
    iasserted(Status{ErrorCodes::BadValue, "Expected a bool"});
}

enum Kind {
    switchBool,
    optBool,
    str,
    strArray,
    integer,
};

using Arg = std::variant<std::monostate, bool, int, std::string, std::vector<std::string>>;

struct OptSpec {
    std::string name;
    std::string description;
    Kind kind;
    Arg defaultValue;
    Arg implicitValue;  // value used if no arg is given
    Arg value;
};

class TestOptionParser {
public:
    explicit TestOptionParser(std::vector<OptSpec>* opts) : _opts{opts} {}

    void run(std::vector<std::string>& argVec) {
        auto ai = argVec.begin();
        if (ai == argVec.end())
            return;
        ++ai;  // Skip argv[0]
        for (; ai != argVec.end();) {
            if (*ai == "--")
                return;  // A "--" that terminates the options.
            bool found = false;
            for (auto&& o : *_opts) {
                StringData a = *ai;
                if (!(_consumePrefix(a, "--") && _consumePrefix(a, o.name)))
                    continue;
                if (_consumePrefix(a, "=")) {  // Matches name, has embedded argument.
                    iassert(ErrorCodes::BadValue,
                            fmt::format("Option {:?} cannot accept argument", o.name),
                            o.kind != Kind::switchBool);
                    _acceptArg(o, a);
                    ai = argVec.erase(ai);
                    found = true;
                    break;
                }
                if (!a.empty()) {  // not a match
                    ++ai;
                    continue;
                }

                found = true;
                if (o.kind == Kind::switchBool) {
                    o.value = true;
                    ai = argVec.erase(ai);
                    break;
                }

                if (!holds_alternative<std::monostate>(o.implicitValue)) {
                    o.value = o.implicitValue;
                    ai = argVec.erase(ai);
                    break;
                }

                // Consider next token as arg.
                ai = argVec.erase(ai);
                iassert(ErrorCodes::BadValue,
                        fmt::format("Option {:?} requires an argument", o.name),
                        ai != argVec.end());
                _acceptArg(o, *ai);
                ai = argVec.erase(ai);
                break;
            }
            // Didn't match any opt spec.
            if (!found)
                ++ai;
        }
    }

private:
    bool _consumePrefix(StringData& s, StringData pat) {
        if (!s.starts_with(pat))
            return false;
        s.remove_prefix(pat.size());
        return true;
    }

    void _acceptArg(OptSpec& o, StringData a) {
        switch (o.kind) {
            case Kind::switchBool:
                MONGO_UNREACHABLE;
            case Kind::optBool:
                o.value = strToBool(a);
                break;
            case Kind::integer:
                o.value = strToInt(a);
                break;
            case Kind::str:
                o.value = std::string{a};
                break;
            case Kind::strArray:
                if (holds_alternative<std::monostate>(o.value))
                    o.value = std::vector<std::string>{};
                get<std::vector<std::string>>(o.value).push_back(std::string{a});
                break;
        }
    }

    std::vector<OptSpec>* _opts;
};


std::vector<OptSpec> allOptions() {
    return std::vector<OptSpec>{
        {
            "help",
            "describe all options",
            Kind::switchBool,
            false,
        },
        {
            "list",
            "List all test suites in this unit test.",
            Kind::switchBool,
            false,
        },
        {
            "suite",
            "Test suite name. Specify --suite more than once to run multiple suites.",
            Kind::strArray,
        },
        {
            "filter",
            "Test case name filter. Specify a regex partial-matching the test names.",
            Kind::str,
        },
        {
            "fileNameFilter",
            "Filter test cases by source file name by partial-matching regex.",
            Kind::str,
        },
        {
            "repeat",
            "Specifies the number of runs for each test.",
            Kind::integer,
            1,
        },
        {
            "verbose",
            "Log more verbose output.  Specify one or more 'v's to increase verbosity.",
            Kind::str,
            {},
            std::string{"v"},
        },
        {
            "tempPath",
            "Directory to place mongo::TempDir subdirectories",
            Kind::str,
        },
        {
            "autoUpdateAsserts",
            "Auto-update expected output for asserts in unit tests which support "
            "auto-updating.",
            Kind::switchBool,
            false,
        },
        {
            "rewriteAllAutoAsserts",
            "Rewrite all auto-updating assertions for self-testing purposes.",
            Kind::switchBool,
            false,
        },
        {
            "enhancedReporter",
            "Use the enhanced test result reporter.",
            Kind::optBool,
            true,
        },
        {
            "showEachTest",
            "Use with --enhancedReporter. Show all test results, not just failing ones.",
            Kind::switchBool,
            false,
        },
    };
}

}  // namespace

UnitTestOptions parseUnitTestOptions(std::vector<std::string>& argVec) {
    std::vector<OptSpec> opts = allOptions();
    TestOptionParser parser{&opts};
    parser.run(argVec);

    auto load = [&]<typename T>(boost::optional<T>* out, StringData key) {
        for (const auto& o : opts) {
            if (o.name != key)
                continue;
            if (holds_alternative<T>(o.value)) {
                out->emplace(get<T>(o.value));
                return;
            }
        }
    };
    UnitTestOptions uto;
    load(&uto.help, "help");
    load(&uto.list, "list");
    load(&uto.suites, "suite");
    load(&uto.filter, "filter");
    load(&uto.fileNameFilter, "fileNameFilter");
    load(&uto.repeat, "repeat");
    load(&uto.verbose, "verbose");
    load(&uto.tempPath, "tempPath");
    load(&uto.autoUpdateAsserts, "autoUpdateAsserts");
    load(&uto.rewriteAllAutoAsserts, "rewriteAllAutoAsserts");
    load(&uto.enhancedReporter, "enhancedReporter");
    load(&uto.showEachTest, "showEachTest");
    return uto;
}

std::string getUnitTestOptionsHelpString(std::vector<std::string>& argVec) {
    std::string s;
    s += fmt::format("Supported MongoDB unit test options:\n");
    for (auto&& o : allOptions()) {
        s += fmt::format(" --{}", o.name);
        if (o.kind == Kind::optBool)
            s += "=<bool>";
        else if (o.kind == Kind::integer)
            s += "=<int>";
        else if (o.kind == Kind::str)
            s += "=<string>";
        else if (o.kind == Kind::strArray)
            s += "=<string> (repeatable)";
        s += fmt::format("\n    {}\n", o.description);
    }
    return s;
}

}  // namespace mongo::unittest
