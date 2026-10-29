// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

#include <string_view>

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

Status MongotParams::onValidateHost(std::string_view str, const boost::optional<TenantId>&) {
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
