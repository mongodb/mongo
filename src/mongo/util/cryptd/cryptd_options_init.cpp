// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/base/status.h"
#include "mongo/db/server_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cryptd/cryptd_options.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/quick_exit.h"

#include <iostream>

#include <boost/filesystem.hpp>

namespace mongo {
namespace {
MONGO_GENERAL_STARTUP_OPTIONS_REGISTER(MongoCryptDOptions)(InitializerContext* context) {
    serverGlobalParams.port = ServerGlobalParams::CryptDServerPort;
    uassertStatusOK(addMongoCryptDOptions(&moe::startupOptions));
}

MONGO_STARTUP_OPTIONS_VALIDATE(MongoCryptDOptions)(InitializerContext* context) {
    if (!handlePreValidationMongoCryptDOptions(moe::startupOptionsParsed)) {
        quickExit(ExitCode::clean);
    }
    uassertStatusOK(validateMongoCryptDOptions(moe::startupOptionsParsed));
    uassertStatusOK(canonicalizeMongoCryptDOptions(&moe::startupOptionsParsed));
    uassertStatusOK(moe::startupOptionsParsed.validate());
}

MONGO_INITIALIZER_GENERAL(CryptdOptions_Store,
                          ("BeginStartupOptionStorage"),
                          ("EndStartupOptionStorage"))
(InitializerContext* context) {
    if (Status ret = storeMongoCryptDOptions(moe::startupOptionsParsed, context->args());
        !ret.isOK()) {
        std::cerr << ret.toString() << std::endl;
        std::cerr << "try '" << context->args()[0] << " --help' for more information" << std::endl;
        quickExit(ExitCode::badOptions);
    }

    // Enable local TCP/IP by default since some drivers (i.e. C#), do not support unix domain
    // sockets
    // Bind only to localhost, ignore any defaults or command line options
    serverGlobalParams.bind_ips.clear();
    serverGlobalParams.bind_ips.push_back("127.0.0.1");

    // Not all machines have ipv6 so users have to opt-in.
    if (serverGlobalParams.enableIPv6) {
        serverGlobalParams.bind_ips.push_back("::1");
    }

    serverGlobalParams.port = mongoCryptDGlobalParams.port;

#ifndef _WIN32
    boost::filesystem::path socketFile(serverGlobalParams.socket);
    socketFile /= "mongocryptd.sock";

    if (!serverGlobalParams.noUnixSocket) {
        serverGlobalParams.bind_ips.push_back(socketFile.generic_string());
        // Set noUnixSocket so that TransportLayer does not create a unix domain socket by default
        // and instead just uses the one we tell it to use.
        serverGlobalParams.noUnixSocket = true;
    }
#endif
}
}  // namespace
}  // namespace mongo
