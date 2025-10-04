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


#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status_with.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/server_options_base.h"
#include "mongo/db/server_options_helpers.h"
#include "mongo/db/service_context.h"
#include "mongo/db/wire_version.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/options_parser/value.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/signal_handlers_synchronous.h"
#include "mongo/util/testing_proctor.h"

#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


using namespace mongo;

namespace {

ConnectionString fixtureConnectionString{};
std::string testFilter;
std::string fileNameFilter;
std::vector<std::string> testSuites{};
bool useEgressGRPC;

}  // namespace

namespace mongo {
namespace unittest {

ConnectionString getFixtureConnectionString() {
    return fixtureConnectionString;
}

bool shouldUseGRPCEgress() {
    return useEgressGRPC;
}

}  // namespace unittest
}  // namespace mongo

namespace {
ServiceContext::ConstructorActionRegisterer registerWireSpec{
    "RegisterWireSpec", [](ServiceContext* service) {
        WireSpec::getWireSpec(service).initialize(WireSpec::Specification{});
    }};
}  // namespace

int main(int argc, char** argv) {
    setupSynchronousSignalHandlers();
    TestingProctor::instance().setEnabled(true);
    runGlobalInitializersOrDie(std::vector<std::string>(argv, argv + argc));
    setTestCommandsEnabled(true);
    auto serviceContextHolder = ServiceContext::make();
    setGlobalServiceContext(std::move(serviceContextHolder));
    quickExit(unittest::Suite::run(testSuites, testFilter, fileNameFilter, 1));
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
    useEgressGRPC = env["useEgressGRPC"].as<bool>();

    env.get("filter", &testFilter).ignore();
    env.get("fileNameFilter", &fileNameFilter).ignore();
    env.get("suite", &testSuites).ignore();

    auto swConnectionString = ConnectionString::parse(connectionString);
    uassertStatusOK(swConnectionString);

    fixtureConnectionString = std::move(swConnectionString.getValue());
    LOGV2(23050,
          "Using test fixture with connection string = {connectionString}",
          "connectionString"_attr = connectionString);
}

}  // namespace
