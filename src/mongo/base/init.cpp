// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/init.h"  // IWYU pragma: keep

#include "mongo/base/initializer.h"
#include "mongo/util/assert_util.h"

#include <cstdlib>
#include <iostream>
#include <utility>

namespace mongo {

GlobalInitializerRegisterer::GlobalInitializerRegisterer(std::string name,
                                                         InitializerFunction initFn,
                                                         DeinitializerFunction deinitFn,
                                                         std::vector<std::string> prerequisites,
                                                         std::vector<std::string> dependents) {
    try {
        getGlobalInitializer().addInitializer(std::move(name),
                                              std::move(initFn),
                                              std::move(deinitFn),
                                              std::move(prerequisites),
                                              std::move(dependents));
    } catch (const DBException& ex) {
        std::cerr << "Attempt to add global initializer failed, status: " << ex.toString()
                  << std::endl;
        ::abort();
    }
}

namespace {
GlobalInitializerRegisterer defaultInitializerRegisterer("default", [](auto) {}, nullptr, {}, {});
}  // namespace

}  // namespace mongo
