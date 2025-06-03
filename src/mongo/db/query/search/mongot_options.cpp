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

#include "mongo/db/query/search/mongot_options.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/query/search/mongot_options_gen.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/text.h"

namespace moe = mongo::optionenvironment;

namespace mongo {

MongotParams globalMongotParams;

MongotParams::MongotParams() {
    host = kMongotHostDefault;
    sslMode = transport::ConnectSSLMode::kGlobalSSLMode;
}

Status MongotParams::onSetHost(const std::string&) {
    return Status::OK();
}

Status MongotParams::onValidateHost(StringData str, const boost::optional<TenantId>&) {
    // Unset value is OK
    if (str.empty()) {
        return Status::OK();
    }

    // `mongotHost` must be able to parse into a HostAndPort
    if (auto status = HostAndPort::parse(str); !status.isOK()) {
        return status.getStatus().withContext("mongoHost must be of the form \"host:port\"");
    }

    globalMongotParams.enabled = true;
    return Status::OK();
}

MONGO_STARTUP_OPTIONS_STORE(SearchTLSModeOptions)(InitializerContext*) {
    auto& params = moe::startupOptionsParsed;

    if (!params.count("setParameter")) {
        return;
    }

    std::map<std::string, std::string> parameters =
        params["setParameter"].as<std::map<std::string, std::string>>();

    const auto searchTLSModeParameter = parameters.find("searchTLSMode");
    if (searchTLSModeParameter == parameters.end()) {
        return;
    }
    const auto& searchTLSMode = searchTLSModeParameter->second;

    if (searchTLSMode == "globalTLS") {
        globalMongotParams.sslMode = transport::ConnectSSLMode::kGlobalSSLMode;
        return;
    }

    auto swMode = SSLParams::tlsModeParse(searchTLSMode);
    uassert(ErrorCodes::BadValue,
            "searchTLSMode must be one of: (globalTLS|disabled|allowTLS|preferTLS|requireTLS). "
            "Input was: " +
                searchTLSMode,
            swMode.isOK());

    auto mode = swMode.getValue();
    if ((mode == SSLParams::SSLMode_disabled) || (mode == SSLParams::SSLMode_allowSSL)) {
        // 'allowSSL' mode makes unecrypted outgoing connections, so we disable SSL for connecting
        // to mongot.
        globalMongotParams.sslMode = transport::ConnectSSLMode::kDisableSSL;
    } else {
        // Ensure certificate is provided for 'preferTLS' and 'requireTLS'.
        uassert(
            ErrorCodes::BadValue,
            "searchTLSMode set to enable TLS for connecting to mongot (preferTLS or requireTLS), "
            "but no TLS certificate provided. Please specify net.tls.certificateKeyFile.",
            params.count("net.tls.certificateKeyFile"));

        globalMongotParams.sslMode = transport::ConnectSSLMode::kEnableSSL;
    }
}

}  // namespace mongo
