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

#include <iostream>
#include <string>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/base/initializer.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/concurrency/locker_noop_client_observer.h"
#include "mongo/db/server_options_base.h"
#include "mongo/db/server_options_helpers.h"
#include "mongo/db/service_context.h"
#include "mongo/db/wire_version.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/transport_layer_asio.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/signal_handlers_synchronous.h"
#include "mongo/util/testing_proctor.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


using namespace mongo;

namespace {

ConnectionString fixtureConnectionString{};

MONGO_INITIALIZER(WireSpec)(InitializerContext*) {
    WireSpec::instance().initialize(WireSpec::Specification{});
}

}  // namespace

namespace mongo {
namespace unittest {

ConnectionString getFixtureConnectionString() {
    return fixtureConnectionString;
}

}  // namespace unittest
}  // namespace mongo

int main(int argc, char** argv) {
    setupSynchronousSignalHandlers();
    TestingProctor::instance().setEnabled(true);
    runGlobalInitializersOrDie(std::vector<std::string>(argv, argv + argc));
    setTestCommandsEnabled(true);
    auto serviceContextHolder = ServiceContext::make();
    serviceContextHolder->registerClientObserver(std::make_unique<LockerNoopClientObserver>());
    setGlobalServiceContext(std::move(serviceContextHolder));
    quickExit(unittest::Suite::run(std::vector<std::string>(), "", "", 1));
}

namespace {

namespace moe = mongo::optionenvironment;

MONGO_INITIALIZER_GENERAL(ForkServer, ("EndStartupOptionHandling"), ("default"))
(InitializerContext* context) {
    // Integration tests do not fork, however the init graph requires a deliberate initializer that
    // _could_ fork and here choses not to do so.
}

MONGO_GENERAL_STARTUP_OPTIONS_REGISTER(IntegrationTestOptions)(InitializerContext*) {
    uassertStatusOK(addBaseServerOptions(&moe::startupOptions));
}

MONGO_STARTUP_OPTIONS_VALIDATE(IntegrationTestOptions)(InitializerContext*) {
    auto& env = moe::startupOptionsParsed;
    auto& opts = moe::startupOptions;

    uassertStatusOK(env.validate());
    uassertStatusOK(validateBaseOptions(env));

    if (env.count("help")) {
        std::cout << opts.helpString() << std::endl;
        quickExit(ExitCode::clean);
    }
}

MONGO_STARTUP_OPTIONS_STORE(IntegrationTestOptions)(InitializerContext*) {
    auto& env = moe::startupOptionsParsed;

    uassertStatusOK(canonicalizeBaseOptions(&env));
    uassertStatusOK(storeBaseOptions(env));

    std::string connectionString = env["connectionString"].as<std::string>();

    auto swConnectionString = ConnectionString::parse(connectionString);
    uassertStatusOK(swConnectionString);

    fixtureConnectionString = std::move(swConnectionString.getValue());
    LOGV2(23050,
          "Using test fixture with connection string = {connectionString}",
          "connectionString"_attr = connectionString);
}

}  // namespace
