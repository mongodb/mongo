// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/client/sdam/json_test_arg_parser.h"

#include "mongo/base/status.h"
#include "mongo/client/sdam/json_test_runner_cli_options_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/options_parser/value.h"

#include <memory>

#include <boost/algorithm/string/join.hpp>
#include <boost/iterator/iterator_traits.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace moe = mongo::optionenvironment;

namespace mongo::sdam {

ArgParser::ArgParser(int argc, char* argv[]) {
    moe::OptionsParser parser;
    moe::Environment environment;
    moe::OptionSection options;

    Status ret = addCliOptions(&options);
    if (!ret.isOK()) {
        std::cerr << "Unexpected error adding cli options: " << ret.toString() << std::endl;
        MONGO_UNREACHABLE;
    }

    ret = parser.run(options, toStringVector(argc, argv), &environment);
    if (argc <= 1 || !ret.isOK() || environment.count("help")) {
        if (!ret.isOK()) {
            std::cerr << "An error occurred: " << ret.toString() << std::endl;
        }
        printHelpAndExit(argv[0], options.helpString());
    }

    const auto exitIfError = [](Status status) {
        if (!status.isOK()) {
            std::cerr << "An error occurred: " << status.toString() << std::endl;
            std::exit(kArgParseExitCode);
        }
    };

    if (environment.count(kSourceDirOption)) {
        ret = environment.get(kSourceDirOption, &_sourceDirectory);
        exitIfError(ret);
    }

    if (environment.count(moe::Key(kFilterOption))) {
        ret = environment.get(moe::Key(kFilterOption), &_testFilters);
        exitIfError(ret);
    }

    if (environment.count(moe::Key(kVerbose))) {
        std::string value;
        ret = environment.get(moe::Key(kVerbose), &value);
        if (!ret.isOK())
            exitIfError(ret);
        _verbose = value.size() + 1;
    }
}

void ArgParser::LogParams() const {
    LOGV2(20199, "Verbosity", "verbose"_attr = _verbose);
    LOGV2(20200, "Source directory", "directory"_attr = _sourceDirectory);
    if (_testFilters.size()) {
        LOGV2(20201, "Test filters", "filters"_attr = boost::join(_testFilters, ", "));
    }
}


};  // namespace mongo::sdam
