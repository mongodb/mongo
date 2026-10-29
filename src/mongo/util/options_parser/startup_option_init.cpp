// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/options_parser/startup_option_init.h"

#include "mongo/base/initializer.h"

#include <stack>
#include <string>
#include <vector>

/*
 * These are the initializer groups for command line and config file option registration, parsing,
 * validation, and storage
 */

namespace mongo {
namespace {

std::string makeInitializer(const std::string& name,
                            const std::vector<std::string>& after,
                            const std::vector<std::string>& before) {
    getGlobalInitializer().addInitializer(
        name, [](InitializerContext*) {}, [](DeinitializerContext*) {}, after, before);
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
    std::stack<StackEntry> stack{{{&stages, {"ValidateLocale"}, {"default"}}}};
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
