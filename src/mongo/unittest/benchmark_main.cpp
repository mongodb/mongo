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


#include "mongo/platform/basic.h"

#include <benchmark/benchmark.h>

#include "mongo/base/initializer.h"
#include "mongo/config.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/benchmark_options_gen.h"
#include "mongo/unittest/log_test.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/signal_handlers_synchronous.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

using mongo::Status;
namespace moe = ::mongo::optionenvironment;


int main(int argc, char** argv) {
    ::mongo::clearSignalMask();
    ::mongo::setupSynchronousSignalHandlers();

    ::mongo::runGlobalInitializersOrDie(std::vector<std::string>(argv, argv + argc));

    // this modifies argc and argv by parsing and removing the benchmark arguments
    ::benchmark::Initialize(&argc, argv);

    moe::OptionSection options;
    Status status = mongo::unittest::addBenchmarkOptions(&options);
    if (!status.isOK()) {
        std::cerr << status << std::endl;
        return static_cast<int>(mongo::ExitCode::fail);
    }

    std::vector<std::string> argVec(argv, argv + argc);
    moe::OptionsParser parser;
    moe::Environment environment;
    Status ret = parser.run(options, argVec, &environment);
    if (!ret.isOK()) {
        std::cerr << options.helpString() << std::endl;
        std::cerr << "Unrecognized Argument, see above message for valid mongo arguments or use "
                     "--help to see valid benchmark arguments."
                  << std::endl;
        return static_cast<int>(mongo::ExitCode::fail);
    }

    std::string verbose;
    environment.get("verbose", &verbose).ignore();
    if (std::any_of(verbose.cbegin(), verbose.cend(), [](char ch) { return ch != 'v'; })) {
        std::cerr << "The string for the --verbose option cannot contain characters other than 'v'"
                  << std::endl;
        std::cerr << options.helpString() << std::endl;
        return static_cast<int>(mongo::ExitCode::fail);
    }

    mongo::unittest::setMinimumLoggedSeverity(mongo::logv2::LogSeverity::Debug(verbose.size()));

#ifndef MONGO_CONFIG_OPTIMIZED_BUILD
    LOGV2(23049,
          "***WARNING*** MongoDB was built with --opt=off. Function timings may be "
          "affected. Always verify any code change against the production environment "
          "(e.g. --opt=on).");
#endif

    ::benchmark::RunSpecifiedBenchmarks();
}
