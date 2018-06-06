/* Copyright 2013 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/platform/basic.h"

#include "mongo/util/net/ssl_options.h"

#include <boost/filesystem/operations.hpp>

#include "mongo/base/status.h"
#include "mongo/config.h"
#include "mongo/db/server_options.h"
#include "mongo/util/hex.h"
#include "mongo/util/log.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/text.h"

#if MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_OPENSSL
#include <openssl/ssl.h>
#endif  // #ifdef MONGO_CONFIG_SSL

namespace mongo {

namespace moe = mongo::optionenvironment;
using std::string;

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
        return (fromHex(hex[idx]) << 4) | fromHex(hex[idx + 1]);
    });
    return ret;
}

/**
 * Older versions of mongod/mongos accepted --sslDisabledProtocols values
 * in the form 'noTLS1_0,noTLS1_1'.  kAcceptNegativePrefix allows us to
 * continue accepting this format on mongod/mongos while only supporting
 * the "standard" TLS1_X format in the shell.
 */
enum DisabledProtocolsMode {
    kStandardFormat,
    kAcceptNegativePrefix,
};

Status storeDisabledProtocols(const std::string& disabledProtocols,
                              DisabledProtocolsMode mode = kStandardFormat) {
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
    };

    // These noTLS* tokens exist for backwards compatibility.
    const std::map<std::string, SSLParams::Protocols> validNoConfigs{
        {"noTLS1_0", SSLParams::Protocols::TLS1_0},
        {"noTLS1_1", SSLParams::Protocols::TLS1_1},
        {"noTLS1_2", SSLParams::Protocols::TLS1_2},
    };

    // Map the tokens to their enum values, and push them onto the list of disabled protocols.
    for (const std::string& token : tokens) {
        auto mappedToken = validConfigs.find(token);
        if (mappedToken != validConfigs.end()) {
            sslGlobalParams.sslDisabledProtocols.push_back(mappedToken->second);
            continue;
        }

        if (mode == DisabledProtocolsMode::kAcceptNegativePrefix) {
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
}  // nameapace

SSLParams sslGlobalParams;

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
                              << key
                              << "'"};
    }

    auto swHex = hexToVector(value.substr(delim + 1));
    if (!swHex.isOK()) {
        return {ErrorCodes::BadValue,
                str::stream() << "Invalid certificate selector value for '" << name << "': "
                              << swHex.getStatus().reason()};
    }

    selector->thumbprint = std::move(swHex.getValue());

    return Status::OK();
}

Status addSSLServerOptions(moe::OptionSection* options) {
    options
        ->addOptionChaining("net.ssl.sslOnNormalPorts",
                            "sslOnNormalPorts",
                            moe::Switch,
                            "use ssl on configured ports")
        .setSources(moe::SourceAllLegacy)
        .incompatibleWith("net.ssl.mode");

    options->addOptionChaining(
        "net.ssl.mode",
        "sslMode",
        moe::String,
        "set the SSL operation mode (disabled|allowSSL|preferSSL|requireSSL)");

    options->addOptionChaining(
        "net.ssl.PEMKeyFile", "sslPEMKeyFile", moe::String, "PEM file for ssl");

    options
        ->addOptionChaining(
            "net.ssl.PEMKeyPassword", "sslPEMKeyPassword", moe::String, "PEM file password")
        .setImplicit(moe::Value(std::string("")));

    options->addOptionChaining("net.ssl.clusterFile",
                               "sslClusterFile",
                               moe::String,
                               "Key file for internal SSL authentication");

    options
        ->addOptionChaining("net.ssl.clusterPassword",
                            "sslClusterPassword",
                            moe::String,
                            "Internal authentication key file password")
        .setImplicit(moe::Value(std::string("")));

    options->addOptionChaining(
        "net.ssl.CAFile", "sslCAFile", moe::String, "Certificate Authority file for SSL");

    options->addOptionChaining(
        "net.ssl.CRLFile", "sslCRLFile", moe::String, "Certificate Revocation List file for SSL");

    options
        ->addOptionChaining("net.ssl.sslCipherConfig",
                            "sslCipherConfig",
                            moe::String,
                            "OpenSSL cipher configuration string")
        .hidden();

    options->addOptionChaining(
        "net.ssl.disabledProtocols",
        "sslDisabledProtocols",
        moe::String,
        "Comma separated list of TLS protocols to disable [TLS1_0,TLS1_1,TLS1_2]");

    options->addOptionChaining("net.ssl.weakCertificateValidation",
                               "sslWeakCertificateValidation",
                               moe::Switch,
                               "allow client to connect without "
                               "presenting a certificate");

    // Alias for --sslWeakCertificateValidation.
    options->addOptionChaining("net.ssl.allowConnectionsWithoutCertificates",
                               "sslAllowConnectionsWithoutCertificates",
                               moe::Switch,
                               "allow client to connect without presenting a certificate");

    options->addOptionChaining("net.ssl.allowInvalidHostnames",
                               "sslAllowInvalidHostnames",
                               moe::Switch,
                               "Allow server certificates to provide non-matching hostnames");

    options->addOptionChaining("net.ssl.allowInvalidCertificates",
                               "sslAllowInvalidCertificates",
                               moe::Switch,
                               "allow connections to servers with invalid certificates");

    options->addOptionChaining(
        "net.ssl.FIPSMode", "sslFIPSMode", moe::Switch, "activate FIPS 140-2 mode at startup");

#ifdef MONGO_CONFIG_SSL_CERTIFICATE_SELECTORS
    options
        ->addOptionChaining("net.ssl.certificateSelector",
                            "sslCertificateSelector",
                            moe::String,
                            "SSL Certificate in system store")
        .incompatibleWith("net.ssl.PEMKeyFile")
        .incompatibleWith("net.ssl.PEMKeyPassword");
    options
        ->addOptionChaining("net.ssl.clusterCertificateSelector",
                            "sslClusterCertificateSelector",
                            moe::String,
                            "SSL Certificate in system store for internal SSL authentication")
        .incompatibleWith("net.ssl.clusterFile")
        .incompatibleWith("net.ssl.clusterFilePassword");
#endif

    return Status::OK();
}

Status addSSLClientOptions(moe::OptionSection* options) {
    options->addOptionChaining("ssl", "ssl", moe::Switch, "use SSL for all connections");

    options
        ->addOptionChaining(
            "ssl.CAFile", "sslCAFile", moe::String, "Certificate Authority file for SSL")
        .requires("ssl");

    options
        ->addOptionChaining(
            "ssl.PEMKeyFile", "sslPEMKeyFile", moe::String, "PEM certificate/key file for SSL")
        .requires("ssl");

    options
        ->addOptionChaining("ssl.PEMKeyPassword",
                            "sslPEMKeyPassword",
                            moe::String,
                            "password for key in PEM file for SSL")
        .requires("ssl");

    options
        ->addOptionChaining(
            "ssl.CRLFile", "sslCRLFile", moe::String, "Certificate Revocation List file for SSL")
        .requires("ssl")
        .requires("ssl.CAFile");

    options
        ->addOptionChaining("net.ssl.allowInvalidHostnames",
                            "sslAllowInvalidHostnames",
                            moe::Switch,
                            "allow connections to servers with non-matching hostnames")
        .requires("ssl");

    options
        ->addOptionChaining("ssl.allowInvalidCertificates",
                            "sslAllowInvalidCertificates",
                            moe::Switch,
                            "allow connections to servers with invalid certificates")
        .requires("ssl");

    options->addOptionChaining(
        "ssl.FIPSMode", "sslFIPSMode", moe::Switch, "activate FIPS 140-2 mode at startup");

#ifdef MONGO_CONFIG_SSL_CERTIFICATE_SELECTORS
    options
        ->addOptionChaining("ssl.certificateSelector",
                            "sslCertificateSelector",
                            moe::String,
                            "SSL Certificate in system store")
        .incompatibleWith("ssl.PEMKeyFile")
        .incompatibleWith("ssl.PEMKeyPassword");
#endif

    options->addOptionChaining(
        "ssl.disabledProtocols",
        "sslDisabledProtocols",
        moe::String,
        "Comma separated list of TLS protocols to disable [TLS1_0,TLS1_1,TLS1_2]");

    return Status::OK();
}

Status validateSSLServerOptions(const moe::Environment& params) {
#ifdef _WIN32
    if (params.count("install") || params.count("reinstall")) {
        if (params.count("net.ssl.PEMKeyFile") &&
            !boost::filesystem::path(params["net.ssl.PEMKeyFile"].as<string>()).is_absolute()) {
            return Status(ErrorCodes::BadValue,
                          "PEMKeyFile requires an absolute file path with Windows services");
        }

        if (params.count("net.ssl.clusterFile") &&
            !boost::filesystem::path(params["net.ssl.clusterFile"].as<string>()).is_absolute()) {
            return Status(ErrorCodes::BadValue,
                          "clusterFile requires an absolute file path with Windows services");
        }

        if (params.count("net.ssl.CAFile") &&
            !boost::filesystem::path(params["net.ssl.CAFile"].as<string>()).is_absolute()) {
            return Status(ErrorCodes::BadValue,
                          "CAFile requires an absolute file path with Windows services");
        }

        if (params.count("net.ssl.CRLFile") &&
            !boost::filesystem::path(params["net.ssl.CRLFile"].as<string>()).is_absolute()) {
            return Status(ErrorCodes::BadValue,
                          "CRLFile requires an absolute file path with Windows services");
        }
    }
#endif

    return Status::OK();
}

Status canonicalizeSSLServerOptions(moe::Environment* params) {
    if (params->count("net.ssl.sslOnNormalPorts") &&
        (*params)["net.ssl.sslOnNormalPorts"].as<bool>() == true) {
        Status ret = params->set("net.ssl.mode", moe::Value(std::string("requireSSL")));
        if (!ret.isOK()) {
            return ret;
        }
        ret = params->remove("net.ssl.sslOnNormalPorts");
        if (!ret.isOK()) {
            return ret;
        }
    }

    return Status::OK();
}

Status storeSSLServerOptions(const moe::Environment& params) {
    if (params.count("net.ssl.mode")) {
        std::string sslModeParam = params["net.ssl.mode"].as<string>();
        if (sslModeParam == "disabled") {
            sslGlobalParams.sslMode.store(SSLParams::SSLMode_disabled);
        } else if (sslModeParam == "allowSSL") {
            sslGlobalParams.sslMode.store(SSLParams::SSLMode_allowSSL);
        } else if (sslModeParam == "preferSSL") {
            sslGlobalParams.sslMode.store(SSLParams::SSLMode_preferSSL);
        } else if (sslModeParam == "requireSSL") {
            sslGlobalParams.sslMode.store(SSLParams::SSLMode_requireSSL);
        } else {
            return Status(ErrorCodes::BadValue, "unsupported value for sslMode " + sslModeParam);
        }
    }

    if (params.count("net.ssl.PEMKeyFile")) {
        sslGlobalParams.sslPEMKeyFile =
            boost::filesystem::absolute(params["net.ssl.PEMKeyFile"].as<string>()).generic_string();
    }

    if (params.count("net.ssl.PEMKeyPassword")) {
        sslGlobalParams.sslPEMKeyPassword = params["net.ssl.PEMKeyPassword"].as<string>();
    }

    if (params.count("net.ssl.clusterFile")) {
        sslGlobalParams.sslClusterFile =
            boost::filesystem::absolute(params["net.ssl.clusterFile"].as<string>())
                .generic_string();
    }

    if (params.count("net.ssl.clusterPassword")) {
        sslGlobalParams.sslClusterPassword = params["net.ssl.clusterPassword"].as<string>();
    }

    if (params.count("net.ssl.CAFile")) {
        sslGlobalParams.sslCAFile =
            boost::filesystem::absolute(params["net.ssl.CAFile"].as<std::string>())
                .generic_string();
    }

    if (params.count("net.ssl.CRLFile")) {
        sslGlobalParams.sslCRLFile =
            boost::filesystem::absolute(params["net.ssl.CRLFile"].as<std::string>())
                .generic_string();
    }

    if (params.count("net.ssl.sslCipherConfig")) {
        warning()
            << "net.ssl.sslCipherConfig is deprecated. It will be removed in a future release.";
        if (!sslGlobalParams.sslCipherConfig.empty()) {
            return Status(ErrorCodes::BadValue,
                          "net.ssl.sslCipherConfig is incompatible with the openSSLCipherConfig "
                          "setParameter");
        }
        sslGlobalParams.sslCipherConfig = params["net.ssl.sslCipherConfig"].as<string>();
    }

    if (params.count("net.ssl.disabledProtocols")) {
        const auto status = storeDisabledProtocols(params["net.ssl.disabledProtocols"].as<string>(),
                                                   DisabledProtocolsMode::kAcceptNegativePrefix);
        if (!status.isOK()) {
            return status;
        }
#if (MONGO_CONFIG_SSL_PROVIDER != MONGO_CONFIG_SSL_PROVIDER_OPENSSL) || \
    (OPENSSL_VERSION_NUMBER >= 0x100000cf) /* 1.0.0l */
    } else {
        /* Disable TLS 1.0 by default on all platforms
         * except on mongod/mongos which were built with an
         * old version of OpenSSL (pre 1.0.0l)
         * which does not support TLS 1.1 or later.
         */
        log() << "Automatically disabling TLS 1.0, to force-enable TLS 1.0 "
                 "specify --sslDisabledProtocols 'none'";
        sslGlobalParams.sslDisabledProtocols.push_back(SSLParams::Protocols::TLS1_0);
#endif
    }

    if (params.count("net.ssl.weakCertificateValidation")) {
        sslGlobalParams.sslWeakCertificateValidation =
            params["net.ssl.weakCertificateValidation"].as<bool>();
    } else if (params.count("net.ssl.allowConnectionsWithoutCertificates")) {
        sslGlobalParams.sslWeakCertificateValidation =
            params["net.ssl.allowConnectionsWithoutCertificates"].as<bool>();
    }
    if (params.count("net.ssl.allowInvalidHostnames")) {
        sslGlobalParams.sslAllowInvalidHostnames =
            params["net.ssl.allowInvalidHostnames"].as<bool>();
    }
    if (params.count("net.ssl.allowInvalidCertificates")) {
        sslGlobalParams.sslAllowInvalidCertificates =
            params["net.ssl.allowInvalidCertificates"].as<bool>();
    }
    if (params.count("net.ssl.FIPSMode")) {
        sslGlobalParams.sslFIPSMode = params["net.ssl.FIPSMode"].as<bool>();
    }

#ifdef MONGO_CONFIG_SSL_CERTIFICATE_SELECTORS
    if (params.count("net.ssl.certificateSelector")) {
        const auto status =
            parseCertificateSelector(&sslGlobalParams.sslCertificateSelector,
                                     "net.ssl.certificateSelector",
                                     params["net.ssl.certificateSelector"].as<std::string>());
        if (!status.isOK()) {
            return status;
        }
    }
    if (params.count("net.ssl.clusterCertificateSelector")) {
        const auto status = parseCertificateSelector(
            &sslGlobalParams.sslClusterCertificateSelector,
            "net.ssl.clusterCertificateSelector",
            params["net.ssl.clusterCertificateSelector"].as<std::string>());
        if (!status.isOK()) {
            return status;
        }
    }
#endif

    int clusterAuthMode = serverGlobalParams.clusterAuthMode.load();
    if (sslGlobalParams.sslMode.load() != SSLParams::SSLMode_disabled) {
        bool usingCertifiateSelectors = params.count("net.ssl.certificateSelector");
        if (sslGlobalParams.sslPEMKeyFile.size() == 0 && !usingCertifiateSelectors) {
            return Status(ErrorCodes::BadValue,
                          "need sslPEMKeyFile or certificateSelector when SSL is enabled");
        }
        if (!sslGlobalParams.sslCRLFile.empty() && sslGlobalParams.sslCAFile.empty()) {
            return Status(ErrorCodes::BadValue, "need sslCAFile with sslCRLFile");
        }

        std::string sslCANotFoundError(
            "No SSL certificate validation can be performed since"
            " no CA file has been provided; please specify an"
            " sslCAFile parameter");

        // When using cetificate selectors, we use the local system certificate store for verifying
        // X.509 certificates for auth instead of relying on a CA file.
        if (sslGlobalParams.sslCAFile.empty() && !usingCertifiateSelectors &&
            clusterAuthMode == ServerGlobalParams::ClusterAuthMode_x509) {
            return Status(ErrorCodes::BadValue, sslCANotFoundError);
        }
    } else if (sslGlobalParams.sslPEMKeyFile.size() || sslGlobalParams.sslPEMKeyPassword.size() ||
               sslGlobalParams.sslClusterFile.size() || sslGlobalParams.sslClusterPassword.size() ||
               sslGlobalParams.sslCAFile.size() || sslGlobalParams.sslCRLFile.size() ||
               sslGlobalParams.sslCipherConfig.size() ||
               params.count("net.ssl.disabledProtocols") ||
#ifdef MONGO_CONFIG_SSL_CERTIFICATE_SELECTORS
               params.count("net.ssl.certificateSelector") ||
               params.count("net.ssl.clusterCertificateSelector") ||
#endif
               sslGlobalParams.sslWeakCertificateValidation) {
        return Status(ErrorCodes::BadValue,
                      "need to enable SSL via the sslMode flag when "
                      "using SSL configuration parameters");
    }
    if (clusterAuthMode == ServerGlobalParams::ClusterAuthMode_sendKeyFile ||
        clusterAuthMode == ServerGlobalParams::ClusterAuthMode_sendX509 ||
        clusterAuthMode == ServerGlobalParams::ClusterAuthMode_x509) {
        if (sslGlobalParams.sslMode.load() == SSLParams::SSLMode_disabled) {
            return Status(ErrorCodes::BadValue, "need to enable SSL via the sslMode flag");
        }
    }
    if (sslGlobalParams.sslMode.load() == SSLParams::SSLMode_allowSSL) {
        // allowSSL and x509 is valid only when we are transitioning to auth.
        if (clusterAuthMode == ServerGlobalParams::ClusterAuthMode_sendX509 ||
            (clusterAuthMode == ServerGlobalParams::ClusterAuthMode_x509 &&
             !serverGlobalParams.transitionToAuth)) {
            return Status(ErrorCodes::BadValue,
                          "cannot have x.509 cluster authentication in allowSSL mode");
        }
    }
    return Status::OK();
}

Status storeSSLClientOptions(const moe::Environment& params) {
    if (params.count("ssl") && params["ssl"].as<bool>() == true) {
        sslGlobalParams.sslMode.store(SSLParams::SSLMode_requireSSL);
    }
    if (params.count("ssl.PEMKeyFile")) {
        sslGlobalParams.sslPEMKeyFile = params["ssl.PEMKeyFile"].as<std::string>();
    }
    if (params.count("ssl.PEMKeyPassword")) {
        sslGlobalParams.sslPEMKeyPassword = params["ssl.PEMKeyPassword"].as<std::string>();
    }
    if (params.count("ssl.CAFile")) {
        sslGlobalParams.sslCAFile = params["ssl.CAFile"].as<std::string>();
    }
    if (params.count("ssl.CRLFile")) {
        sslGlobalParams.sslCRLFile = params["ssl.CRLFile"].as<std::string>();
    }
    if (params.count("net.ssl.allowInvalidHostnames")) {
        sslGlobalParams.sslAllowInvalidHostnames =
            params["net.ssl.allowInvalidHostnames"].as<bool>();
    }
    if (params.count("ssl.allowInvalidCertificates")) {
        sslGlobalParams.sslAllowInvalidCertificates = true;
    }
    if (params.count("ssl.FIPSMode")) {
        sslGlobalParams.sslFIPSMode = true;
    }

    if (params.count("ssl.disabledProtocols")) {
        const auto status =
            storeDisabledProtocols(params["ssl.disabledProtocols"].as<std::string>());
        if (!status.isOK()) {
            return status;
        }
#if ((MONGO_CONFIG_SSL_PROVIDER != MONGO_CONFIG_SSL_PROVIDER_OPENSSL) || \
     (OPENSSL_VERSION_NUMBER >= 0x100000cf)) /* 1.0.0l */
    } else {
        /* Similar to the server setting above, we auto-disable TLS 1.0
         * for shell clients which support TLS 1.1 and later.
         * Unlike above, we don't have a blanket exception for Apple,
         * since the reason for supporting external tooling does not apply.
         *
         * We also skip logging to keep the spam away from the interactive client op.
         */
        sslGlobalParams.sslDisabledProtocols.push_back(SSLParams::Protocols::TLS1_0);
#endif
    }

#ifdef MONGO_CONFIG_SSL_CERTIFICATE_SELECTORS
    if (params.count("ssl.certificateSelector")) {
        const auto status =
            parseCertificateSelector(&sslGlobalParams.sslCertificateSelector,
                                     "ssl.certificateSelector",
                                     params["ssl.certificateSelector"].as<std::string>());
        if (!status.isOK()) {
            return status;
        }
    }
#endif
    return Status::OK();
}

}  // namespace mongo
