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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include <iostream>
#include <string>
#include <vector>

#include "mongo/base/initializer.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/server_options_base.h"
#include "mongo/db/server_options_helpers.h"
#include "mongo/db/service_context.h"
#include "mongo/logger/logger.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/transport_layer_asio.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/signal_handlers_synchronous.h"

using namespace mongo;

namespace {

ConnectionString fixtureConnectionString{};

}  // namespace

namespace mongo {
namespace unittest {

ConnectionString getFixtureConnectionString() {
    return fixtureConnectionString;
}

}  // namespace unittest
}  // namespace mongo

int main(int argc, char** argv, char** envp) {
    setupSynchronousSignalHandlers();
    runGlobalInitializersOrDie(argc, argv, envp);
    setGlobalServiceContext(ServiceContext::make());
    quickExit(unittest::Suite::run(std::vector<std::string>(), "", "", 1));
}

namespace {

namespace moe = mongo::optionenvironment;

MONGO_INITIALIZER_GENERAL(ForkServer, ("EndStartupOptionHandling"), ("default"))
(InitializerContext* context) {
    // Integration tests do not fork, however the init graph requires a deliberate initializer that
    // _could_ fork and here choses not to do so.
    return Status::OK();
}

MONGO_GENERAL_STARTUP_OPTIONS_REGISTER(IntegrationTestOptions)(InitializerContext*) {
    uassertStatusOK(addBaseServerOptions(&moe::startupOptions));

    return Status::OK();
}

MONGO_STARTUP_OPTIONS_VALIDATE(IntegrationTestOptions)(InitializerContext*) {
    auto& env = moe::startupOptionsParsed;
    auto& opts = moe::startupOptions;

    if (auto ret = env.validate(); !ret.isOK()) {
        return ret;
    }

    if (auto ret = validateBaseOptions(env); !ret.isOK()) {
        return ret;
    }

    if (env.count("help")) {
        std::cout << opts.helpString() << std::endl;
        quickExit(EXIT_SUCCESS);
    }

    return Status::OK();
}

MONGO_STARTUP_OPTIONS_STORE(IntegrationTestOptions)(InitializerContext*) {
    auto& env = moe::startupOptionsParsed;

    if (auto ret = canonicalizeBaseOptions(&env); !ret.isOK()) {
        return ret;
    }

    if (auto ret = storeBaseOptions(env); !ret.isOK()) {
        return ret;
    }

    std::string connectionString = env["connectionString"].as<std::string>();

    auto swConnectionString = ConnectionString::parse(connectionString);
    if (!swConnectionString.isOK()) {
        return swConnectionString.getStatus();
    }

    fixtureConnectionString = std::move(swConnectionString.getValue());
    LOGV2(23050,
          "Using test fixture with connection string = {connectionString}",
          "connectionString"_attr = connectionString);


    return Status::OK();
}

}  // namespace
