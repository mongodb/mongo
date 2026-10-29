// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


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
#include "mongo/unittest/unittest.h"
#include "mongo/unittest/unittest_main_core.h"
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
    unittest::MainProgress progress({}, std::vector<std::string>(argv, argv + argc));
    progress.initialize();
    auto serviceContextHolder = ServiceContext::make();
    setGlobalServiceContext(std::move(serviceContextHolder));
    quickExit(progress.test());
}

namespace {

namespace moe = mongo::optionenvironment;

MONGO_INITIALIZER_GENERAL(ForkServer, ("EndStartupOptionHandling"), ("default"))
(InitializerContext* context) {
    // Integration tests do not fork, however the init graph requires a deliberate initializer that
    // _could_ fork and here choses not to do so.
}

MONGO_STARTUP_OPTIONS_VALIDATE(IntegrationTestOptions)(InitializerContext*) {
    auto& env = moe::startupOptionsParsed;
    auto& opts = moe::startupOptions;

    uassertStatusOK(env.validate());

    if (env.count("help")) {
        std::cout << opts.helpString() << std::endl;
        quickExit(ExitCode::clean);
    }
}

MONGO_STARTUP_OPTIONS_STORE(IntegrationTestOptions)(InitializerContext*) {
    auto& env = moe::startupOptionsParsed;

    std::string connectionString = env["connectionString"].as<std::string>();
    useEgressGRPC = env["useEgressGRPC"].as<bool>();

    auto swConnectionString = ConnectionString::parse(connectionString);
    uassertStatusOK(swConnectionString);

    fixtureConnectionString = std::move(swConnectionString.getValue());
    LOGV2(23050,
          "Using test fixture with connection string = {connectionString}",
          "connectionString"_attr = connectionString);
}

}  // namespace
