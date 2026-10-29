// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/util/net/ssl_options.h"

#include "mongo/base/status.h"
#include "mongo/config.h"
#include "mongo/db/server_options.h"
#include "mongo/util/ctype.h"
#include "mongo/util/hex.h"
#include "mongo/util/options_parser/startup_options.h"

#include <string_view>

#include <absl/strings/str_split.h>
#include <boost/filesystem/operations.hpp>

#if MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_OPENSSL
#include <openssl/ssl.h>
#endif  // #ifdef MONGO_CONFIG_SSL

namespace mongo {

using std::string;

SSLParams sslGlobalParams;

namespace {
std::vector<uint8_t> hexToVector(std::string_view hex) {
    try {
        std::string data = hexblob::decode(hex);
        return std::vector<uint8_t>(data.begin(), data.end());
    } catch (const ExceptionFor<ErrorCodes::FailedToParse>&) {
        if (std::any_of(hex.begin(), hex.end(), [](char c) { return !ctype::isXdigit(c); })) {
            uasserted(ErrorCodes::BadValue, "Not a valid hex string");
        }
        if (hex.size() % 2) {
            uasserted(ErrorCodes::BadValue, "Not an even number of hexits");
        }
        throw;
    }
}
}  // namespace

Status storeSSLDisabledProtocols(const std::string& disabledProtocols,
                                 SSLDisabledProtocolsMode mode /* =kStandardFormat */) {
    if (disabledProtocols == "none") {
        // Allow overriding the default behavior below of implicitly disabling TLS 1.0 and TLS 1.1.
        return Status::OK();
    }

    // The disabledProtocols field is composed of a comma separated list of protocols to
    // disable. First, tokenize the field.
    const auto tokens = absl::StrSplit(disabledProtocols, ",", absl::SkipEmpty());

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
    for (const auto& t : tokens) {
        std::string token(t);
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
                                std::string_view name,
                                std::string_view value) {
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
        selector->subject = std::string{value.substr(delim + 1)};
        return Status::OK();
    }

    if (key != "thumbprint") {
        return {ErrorCodes::BadValue,
                str::stream() << "Unknown certificate selector property for '" << name << "': '"
                              << key << "'"};
    }

    try {
        selector->thumbprint = hexToVector(value.substr(delim + 1));
    } catch (const DBException& ex) {
        return {ErrorCodes::BadValue,
                str::stream() << "Invalid certificate selector value for '" << name
                              << "': " << ex.reason()};
    }
    return Status::OK();
}

StatusWith<SSLParams::SSLModes> SSLParams::sslModeParse(std::string_view strMode) {
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

StatusWith<SSLParams::SSLModes> SSLParams::tlsModeParse(std::string_view strMode) {
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
            // Default case because sslMode is an Atomic<int> and not bound by enum rules.
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
            // Default case because sslMode is an Atomic<int> and not bound by enum rules.
            return "unknown";
    }
}


}  // namespace mongo
