/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include "mongo/client/sdam/json_test_arg_parser.h"

#include "mongo/logger/logger.h"
#include "mongo/logv2/log.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"

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

    ret = parser.run(options, toStringVector(argc, argv), {}, &environment);
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
    LOGV2(20199, "Verbosity: {verbose}", "verbose"_attr = _verbose);
    LOGV2(20200, "Source Directory: {sourceDirectory}", "sourceDirectory"_attr = _sourceDirectory);
    if (_testFilters.size()) {
        LOGV2(20201,
              "Filters: {boost_join_testFilters}",
              "boost_join_testFilters"_attr = boost::join(_testFilters, ", "));
    }
}


};  // namespace mongo::sdam
