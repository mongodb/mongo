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

#include "mongo/util/options_parser/startup_option_init.h"

#include <stack>

#include "mongo/util/assert_util.h"

/*
 * These are the initializer groups for command line and config file option registration, parsing,
 * validation, and storage
 */

namespace mongo {
namespace {

std::string makeInitializer(const std::string& name,
                            const std::vector<std::string>& after,
                            const std::vector<std::string>& before) {
    uassertStatusOK(getGlobalInitializer().getInitializerDependencyGraph().addInitializer(
        name,
        [](InitializerContext*) { return Status::OK(); },
        [](DeinitializerContext*) { return Status::OK(); },
        after,
        before));
    return name;
}

// Initializer groups for general option registration. Useful for controlling the order in which
// options are registered for modules, which affects the order in which they are printed in help
// output.
void StaticInit() {
    struct NestedStages {
        std::string name;
        std::vector<NestedStages> children;
    };
    struct StackEntry {
        const NestedStages* n;
        std::vector<std::string> after;
        std::vector<std::string> before;
    };
    const NestedStages stages{"StartupOptionHandling",
                              {
                                  {"StartupOptionRegistration",
                                   {
                                       {"GeneralStartupOptionRegistration"},  //
                                       {"ModuleStartupOptionRegistration"},   //
                                   }},
                                  {"StartupOptionParsing"},
                                  {"StartupOptionValidation"},
                                  {"StartupOptionSetup"},
                                  {"StartupOptionStorage"},
                                  {"PostStartupOptionStorage"},
                              }};
    std::stack<StackEntry> stack{{{&stages, {"GlobalLogManager", "ValidateLocale"}, {"default"}}}};
    while (!stack.empty()) {
        auto top = stack.top();
        stack.pop();
        std::string tail = makeInitializer("Begin" + top.n->name, top.after, {});
        for (const auto& child : top.n->children) {
            stack.push({&child, {tail}, {}});
            tail = "End" + child.name;
        }
        tail = makeInitializer("End" + top.n->name, {tail}, {top.before});
    }
}

const int dummy = (StaticInit(), 0);

}  // namespace
}  // namespace mongo
