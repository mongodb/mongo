/**
*    Copyright (C) 2018 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/base/init.h"
#include "mongo/db/mongod_options.h"
#include "mongo/db/service_context.h"
#include "mongo/embedded/embedded.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/signal_handlers.h"
#include "mongo/util/text.h"

#include <yaml-cpp/yaml.h>

namespace mongo {
namespace {

MONGO_INITIALIZER_WITH_PREREQUISITES(SignalProcessingStartup, ("ThreadNameInitializer"))
(InitializerContext*) {
    // Make sure we call this as soon as possible but before any other threads are started. Before
    // embedded::initialize is too early and after is too late. So instead we hook in during the
    // global initialization at the right place.
    startSignalProcessingThread();
    return Status::OK();
}

int mongoeMain(int argc, char* argv[], char** envp) {
    ServiceContext* serviceContext = nullptr;

    registerShutdownTask([&]() {
        if (!serviceContext)
            return;

        if (auto tl = serviceContext->getTransportLayer()) {
            log(logger::LogComponent::kNetwork) << "shutdown: going to close listening sockets...";
            tl->shutdown();
        }

        embedded::shutdown(serviceContext);
    });

    setupSignalHandlers();

    log() << "MongoDB embedded standalone application, for testing purposes only";

    try {
        optionenvironment::OptionSection startupOptions("Options");
        uassertStatusOK(addMongodOptions(&startupOptions));

        // Manually run the code that's equivalent to the MONGO_INITIALIZERs for mongod. We can't do
        // this in initializers because embedded uses a different options format. However as long as
        // we store the options in the same place it will be valid for embedded too. Adding all
        // options mongod we don't have to maintain a separate set for this executable, some will be
        // unused but that's fine as this is just an executable for testing purposes anyway.
        std::vector<std::string> args;
        std::map<std::string, std::string> env;

        args.reserve(argc);
        std::copy(argv, argv + argc, std::back_inserter(args));

        optionenvironment::OptionsParser parser;
        uassertStatusOK(
            parser.run(startupOptions, args, env, &optionenvironment::startupOptionsParsed));
        uassertStatusOK(storeMongodOptions(optionenvironment::startupOptionsParsed));

        // Add embedded specific options that's not available in mongod here.
        YAML::Emitter yaml;
        serviceContext = embedded::initialize(yaml.c_str());

        auto tl =
            transport::TransportLayerManager::createWithConfig(&serverGlobalParams, serviceContext);
        uassertStatusOK(tl->setup());

        serviceContext->setTransportLayer(std::move(tl));

        uassertStatusOK(serviceContext->getServiceExecutor()->start());
        uassertStatusOK(serviceContext->getTransportLayer()->start());
    } catch (const std::exception& ex) {
        error() << ex.what();
        return EXIT_BADOPTIONS;
    }

    return waitForShutdown();
}

}  // namespace
}  // namespace mongo

#if defined(_WIN32)
// In Windows, wmain() is an alternate entry point for main(), and receives the same parameters
// as main() but encoded in Windows Unicode (UTF-16); "wide" 16-bit wchar_t characters.  The
// WindowsCommandLine object converts these wide character strings to a UTF-8 coded equivalent
// and makes them available through the argv() and envp() members.  This enables mongoDbMain()
// to process UTF-8 encoded arguments and environment variables without regard to platform.
int wmain(int argc, wchar_t* argvW[], wchar_t* envpW[]) {
    mongo::WindowsCommandLine wcl(argc, argvW, envpW);
    return mongo::mongoeMain(argc, wcl.argv(), wcl.envp());
}
#else
int main(int argc, char* argv[], char** envp) {
    return mongo::mongoeMain(argc, argv, envp);
}
#endif
