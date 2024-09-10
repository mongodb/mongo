/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include "cryptd_options.h"

#include <boost/filesystem.hpp>
#include <iostream>

#include "mongo/base/status.h"
#include "mongo/db/server_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/quick_exit.h"

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
