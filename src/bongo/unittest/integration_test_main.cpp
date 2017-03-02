/**
 *    Copyright 2015 BongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define BONGO_LOG_DEFAULT_COMPONENT ::bongo::logger::LogComponent::kDefault

#include "bongo/platform/basic.h"

#include <iostream>
#include <string>
#include <vector>

#include "bongo/base/initializer.h"
#include "bongo/client/connection_string.h"
#include "bongo/unittest/unittest.h"
#include "bongo/util/log.h"
#include "bongo/util/options_parser/environment.h"
#include "bongo/util/options_parser/option_section.h"
#include "bongo/util/options_parser/options_parser.h"
#include "bongo/util/options_parser/startup_option_init.h"
#include "bongo/util/options_parser/startup_options.h"
#include "bongo/util/quick_exit.h"
#include "bongo/util/signal_handlers_synchronous.h"

using namespace bongo;

namespace {

ConnectionString fixtureConnectionString{};

const char kConnectionStringFlag[] = "connectionString";

}  // namespace

namespace bongo {
namespace unittest {

ConnectionString getFixtureConnectionString() {
    return fixtureConnectionString;
}

}  // namespace unittest
}  // namespace bongo

int main(int argc, char** argv, char** envp) {
    setupSynchronousSignalHandlers();
    runGlobalInitializersOrDie(argc, argv, envp);

    return unittest::Suite::run(std::vector<std::string>(), "", 1);
}

namespace moe = bongo::optionenvironment;

BONGO_GENERAL_STARTUP_OPTIONS_REGISTER(IntegrationTestOptions)(InitializerContext*) {
    auto& opts = moe::startupOptions;
    opts.addOptionChaining("help", "help", moe::Switch, "Display help");
    opts.addOptionChaining(kConnectionStringFlag,
                           kConnectionStringFlag,
                           moe::String,
                           "The connection string associated with the test fixture that this "
                           "integration test should run against.")
        .setDefault(moe::Value("localhost:27017"));
    return Status::OK();
}

BONGO_STARTUP_OPTIONS_VALIDATE(IntegrationTestOptions)(InitializerContext*) {
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

BONGO_STARTUP_OPTIONS_STORE(IntegrationTestOptions)(InitializerContext*) {
    auto& env = moe::startupOptionsParsed;
    moe::Value connectionString;
    auto ret = env.get(moe::Key(kConnectionStringFlag), &connectionString);
    if (!ret.isOK()) {
        return ret;
    }

    auto swConnectionString = ConnectionString::parse(connectionString.as<std::string>());
    if (!swConnectionString.isOK()) {
        return swConnectionString.getStatus();
    }

    log() << "Using test fixture with connection string = " << connectionString.as<std::string>();

    fixtureConnectionString = std::move(swConnectionString.getValue());

    return Status::OK();
}
