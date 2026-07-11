// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/logv2/log.h"
#include "mongo/unittest/benchmark_options_gen.h"
#include "mongo/unittest/log_test.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/options_parser/value.h"
#include "mongo/util/signal_handlers_synchronous.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

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
