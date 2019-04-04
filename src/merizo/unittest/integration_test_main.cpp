/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#define MERIZO_LOG_DEFAULT_COMPONENT ::merizo::logger::LogComponent::kDefault

#include "merizo/platform/basic.h"

#include <iostream>
#include <string>
#include <vector>

#include "merizo/base/initializer.h"
#include "merizo/client/connection_string.h"
#include "merizo/db/service_context.h"
#include "merizo/transport/transport_layer_asio.h"
#include "merizo/unittest/unittest.h"
#include "merizo/util/log.h"
#include "merizo/util/options_parser/environment.h"
#include "merizo/util/options_parser/option_section.h"
#include "merizo/util/options_parser/options_parser.h"
#include "merizo/util/options_parser/startup_option_init.h"
#include "merizo/util/options_parser/startup_options.h"
#include "merizo/util/quick_exit.h"
#include "merizo/util/signal_handlers_synchronous.h"

using namespace merizo;

namespace {

ConnectionString fixtureConnectionString{};

}  // namespace

namespace merizo {
namespace unittest {

ConnectionString getFixtureConnectionString() {
    return fixtureConnectionString;
}

}  // namespace unittest
}  // namespace merizo

int main(int argc, char** argv, char** envp) {
    setupSynchronousSignalHandlers();
    runGlobalInitializersOrDie(argc, argv, envp);
    setGlobalServiceContext(ServiceContext::make());
    quickExit(unittest::Suite::run(std::vector<std::string>(), "", 1));
}

namespace moe = merizo::optionenvironment;

MERIZO_STARTUP_OPTIONS_VALIDATE(IntegrationTestOptions)(InitializerContext*) {
    auto& env = moe::startupOptionsParsed;
    auto& opts = moe::startupOptions;

    auto ret = env.validate();

    if (!ret.isOK()) {
        return ret;
    }

    if (env.count("help")) {
        std::cout << opts.helpString() << std::endl;
        quickExit(EXIT_SUCCESS);
    }

    return Status::OK();
}

MERIZO_STARTUP_OPTIONS_STORE(IntegrationTestOptions)(InitializerContext*) {
    const auto& env = moe::startupOptionsParsed;

    std::string connectionString = env["connectionString"].as<std::string>();

    auto swConnectionString = ConnectionString::parse(connectionString);
    if (!swConnectionString.isOK()) {
        return swConnectionString.getStatus();
    }

    fixtureConnectionString = std::move(swConnectionString.getValue());
    log() << "Using test fixture with connection string = " << connectionString;


    return Status::OK();
}
