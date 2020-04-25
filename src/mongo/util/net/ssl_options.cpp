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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

#include "mongo/platform/basic.h"

#include "mongo/util/net/ssl_options.h"

#include <boost/filesystem/operations.hpp>

#include "mongo/base/status.h"
#include "mongo/config.h"
#include "mongo/db/server_options.h"
#include "mongo/util/hex.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/text.h"

#if MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_OPENSSL
#include <openssl/ssl.h>
#endif  // #ifdef MONGO_CONFIG_SSL

namespace mongo {

namespace moe = mongo::optionenvironment;
using std::string;

SSLParams sslGlobalParams;

namespace {
StatusWith<std::vector<uint8_t>> hexToVector(StringData hex) {
    if (std::any_of(hex.begin(), hex.end(), [](char c) { return !isxdigit(c); })) {
        return {ErrorCodes::BadValue, "Not a valid hex string"};
    }
    if (hex.size() % 2) {
        return {ErrorCodes::BadValue, "Not an even number of hexits"};
    }

    std::vector<uint8_t> ret;
    ret.resize(hex.size() >> 1);
    int idx = -2;
    std::generate(ret.begin(), ret.end(), [&hex, &idx] {
        idx += 2;
        return (uassertStatusOK(fromHex(hex[idx])) << 4) | uassertStatusOK(fromHex(hex[idx + 1]));
    });
    return ret;
}
}  // namespace

Status storeSSLDisabledProtocols(const std::string& disabledProtocols,
                                 SSLDisabledProtocolsMode mode /* =kStandardFormat */) {
    if (disabledProtocols == "none") {
        // Allow overriding the default behavior below of implicitly disabling TLS 1.0.
        return Status::OK();
    }

    // The disabledProtocols field is composed of a comma separated list of protocols to
    // disable. First, tokenize the field.
    const auto tokens = StringSplitter::split(disabledProtocols, ",");

    // All universally accepted tokens, and their corresponding enum representation.
    const std::map<std::string, SSLParams::Protocols> validConfigs{
        {"TLS1_0", SSLParams::Protocols::TLS1_0},
        {"TLS1_1", SSLParams::Protocols::TLS1_1},
        {"TLS1_2", SSLParams::Protocols::TLS1_2},
        {"TLS1_3", SSLParams::Protocols::TLS1_3},
    };

    // These noTLS* tokens exist for backwards compatibility.
    const std::map<std::string, SSLParams::Protocols> validNoConfigs{
        {"noTLS1_0", SSLParams::Protocols::TLS1_0},
        {"noTLS1_1", SSLParams::Protocols::TLS1_1},
        {"noTLS1_2", SSLParams::Protocols::TLS1_2},
        {"noTLS1_3", SSLParams::Protocols::TLS1_3},
    };

    // Map the tokens to their enum values, and push them onto the list of disabled protocols.
    for (const std::string& token : tokens) {
        auto mappedToken = validConfigs.find(token);
        if (mappedToken != validConfigs.end()) {
            sslGlobalParams.sslDisabledProtocols.push_back(mappedToken->second);
            continue;
        }

        if (mode == SSLDisabledProtocolsMode::kAcceptNegativePrefix) {
            auto mappedNoToken = validNoConfigs.find(token);
            if (mappedNoToken != validNoConfigs.end()) {
                sslGlobalParams.sslDisabledProtocols.push_back(mappedNoToken->second);
                continue;
            }
        }

        return Status(ErrorCodes::BadValue, "Unrecognized disabledProtocols '" + token + "'");
    }

    return Status::OK();
}

Status parseCertificateSelector(SSLParams::CertificateSelector* selector,
                                StringData name,
                                StringData value) {
    selector->subject.clear();
    selector->thumbprint.clear();

    const auto delim = value.find('=');
    if (delim == std::string::npos) {
        return {ErrorCodes::BadValue,
                str::stream() << "Certificate selector for '" << name
                              << "' must be a key=value pair"};
    }

    auto key = value.substr(0, delim);
    if (key == "subject") {
        selector->subject = value.substr(delim + 1).toString();
        return Status::OK();
    }

    if (key != "thumbprint") {
        return {ErrorCodes::BadValue,
                str::stream() << "Unknown certificate selector property for '" << name << "': '"
                              << key << "'"};
    }

    auto swHex = hexToVector(value.substr(delim + 1));
    if (!swHex.isOK()) {
        return {ErrorCodes::BadValue,
                str::stream() << "Invalid certificate selector value for '" << name
                              << "': " << swHex.getStatus().reason()};
    }

    selector->thumbprint = std::move(swHex.getValue());

    return Status::OK();
}

StatusWith<SSLParams::SSLModes> SSLParams::sslModeParse(StringData strMode) {
    if (strMode == "disabled") {
        return SSLParams::SSLMode_disabled;
    } else if (strMode == "allowSSL") {
        return SSLParams::SSLMode_allowSSL;
    } else if (strMode == "preferSSL") {
        return SSLParams::SSLMode_preferSSL;
    } else if (strMode == "requireSSL") {
        return SSLParams::SSLMode_requireSSL;
    } else {
        return Status(
            ErrorCodes::BadValue,
            str::stream()
                << "Invalid sslMode setting '" << strMode
                << "', expected one of: 'disabled', 'allowSSL', 'preferSSL', or 'requireSSL'");
    }
}

StatusWith<SSLParams::SSLModes> SSLParams::tlsModeParse(StringData strMode) {
    if (strMode == "disabled") {
        return SSLParams::SSLMode_disabled;
    } else if (strMode == "allowTLS") {
        return SSLParams::SSLMode_allowSSL;
    } else if (strMode == "preferTLS") {
        return SSLParams::SSLMode_preferSSL;
    } else if (strMode == "requireTLS") {
        return SSLParams::SSLMode_requireSSL;
    } else {
        return Status(
            ErrorCodes::BadValue,
            str::stream()
                << "Invalid tlsMode setting '" << strMode
                << "', expected one of: 'disabled', 'allowTLS', 'preferTLS', or 'requireTLS'");
    }
}


std::string SSLParams::sslModeFormat(int mode) {
    switch (mode) {
        case SSLParams::SSLMode_disabled:
            return "disabled";
        case SSLParams::SSLMode_allowSSL:
            return "allowSSL";
        case SSLParams::SSLMode_preferSSL:
            return "preferSSL";
        case SSLParams::SSLMode_requireSSL:
            return "requireSSL";
        default:
            // Default case because sslMode is an AtomicWord<int> and not bound by enum rules.
            return "unknown";
    }
}

std::string SSLParams::tlsModeFormat(int mode) {
    switch (mode) {
        case SSLParams::SSLMode_disabled:
            return "disabled";
        case SSLParams::SSLMode_allowSSL:
            return "allowTLS";
        case SSLParams::SSLMode_preferSSL:
            return "preferTLS";
        case SSLParams::SSLMode_requireSSL:
            return "requireTLS";
        default:
            // Default case because sslMode is an AtomicWord<int> and not bound by enum rules.
            return "unknown";
    }
}


}  // namespace mongo
