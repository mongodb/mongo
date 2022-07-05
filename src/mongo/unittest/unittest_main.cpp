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

#include <iostream>
#include <string>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/wire_version.h"
#include "mongo/logv2/log_domain_global.h"
#include "mongo/logv2/log_manager.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/unittest/unittest_options_gen.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/signal_handlers_synchronous.h"
#include "mongo/util/testing_proctor.h"

using mongo::Status;

namespace moe = ::mongo::optionenvironment;

namespace mongo {
namespace {

MONGO_INITIALIZER(WireSpec)(InitializerContext*) {
    WireSpec::instance().initialize(WireSpec::Specification{});
}

}  // namespace
}  // namespace mongo

int main(int argc, char** argv) {
    std::vector<std::string> argVec(argv, argv + argc);

    ::mongo::clearSignalMask();
    ::mongo::setupSynchronousSignalHandlers();

    ::mongo::TestingProctor::instance().setEnabled(true);
    ::mongo::runGlobalInitializersOrDie(argVec);
    ::mongo::setTestCommandsEnabled(true);

    moe::OptionSection options;

    Status status = mongo::unittest::addUnitTestOptions(&options);
    if (!status.isOK()) {
        std::cerr << status;
        return static_cast<int>(mongo::ExitCode::fail);
    }

    moe::OptionsParser parser;
    moe::Environment environment;
    Status ret = parser.run(options, argVec, &environment);
    if (!ret.isOK()) {
        std::cerr << options.helpString();
        return static_cast<int>(mongo::ExitCode::fail);
    }

    bool list = false;
    moe::StringVector_t suites;
    std::string filter;
    int repeat = 1;
    std::string verbose;
    std::string fileNameFilter;
    std::string internalRunDeathTest;

    // "list" and "repeat" will be assigned with default values, if not present.
    invariant(environment.get("list", &list));
    invariant(environment.get("repeat", &repeat));
    // The default values of "suite" "filter" and "verbose" are empty.
    environment.get("suite", &suites).ignore();
    environment.get("filter", &filter).ignore();
    environment.get("verbose", &verbose).ignore();
    environment.get("fileNameFilter", &fileNameFilter).ignore();
    environment.get("internalRunDeathTest", &internalRunDeathTest).ignore();

    if (environment.count("tempPath")) {
        ::mongo::unittest::TempDir::setTempPath(environment["tempPath"].as<std::string>());
    }

    mongo::unittest::getSpawnInfo() = {argVec, internalRunDeathTest, true};

    if (std::any_of(verbose.cbegin(), verbose.cend(), [](char ch) { return ch != 'v'; })) {
        std::cerr << "The string for the --verbose option cannot contain characters other than 'v'"
                  << std::endl;
        std::cerr << options.helpString();
        return static_cast<int>(mongo::ExitCode::fail);
    }
    mongo::unittest::setMinimumLoggedSeverity(mongo::logv2::LogSeverity::Debug(verbose.size()));

    if (list) {
        auto suiteNames = ::mongo::unittest::getAllSuiteNames();
        for (auto name : suiteNames) {
            std::cout << name << std::endl;
        }
        return static_cast<int>(mongo::ExitCode::clean);
    }

    auto result = ::mongo::unittest::Suite::run(suites, filter, fileNameFilter, repeat);

    ret = ::mongo::runGlobalDeinitializers();
    if (!ret.isOK()) {
        std::cerr << "Global deinitilization failed: " << ret.reason() << std::endl;
    }

    ::mongo::TestingProctor::instance().exitAbruptlyIfDeferredErrors();

    return result;
}
