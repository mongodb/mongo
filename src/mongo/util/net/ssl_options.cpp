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
        ->addOptionChaining("net.tls.tlsOnNormalPorts",
                            "tlsOnNormalPorts",
                            moe::Switch,
                            "Use TLS on configured ports",
                            {"net.ssl.sslOnNormalPorts"},
                            {"sslOnNormalPorts"})
        .setSources(moe::SourceAllLegacy)
        .incompatibleWith("net.tls.mode")
        .incompatibleWith("net.ssl.mode");

    options
        ->addOptionChaining("net.tls.mode",
                            "tlsMode",
                            moe::String,
                            "Set the TLS operation mode (disabled|allowTLS|preferTLS|requireTLS)")
        .incompatibleWith("net.ssl.mode");
    options
        ->addOptionChaining("net.ssl.mode",
                            "sslMode",
                            moe::String,
                            "Set the TLS operation mode (disabled|allowSSL|preferSSL|requireSSL)")
        .incompatibleWith("net.tls.mode");

    options->addOptionChaining("net.tls.PEMKeyFile",
                               "tlsPEMKeyFile",
                               moe::String,
                               "PEM file for TLS",
                               {"net.ssl.PEMKeyFile"},
                               {"sslPEMKeyFile"});

    options
        ->addOptionChaining("net.tls.PEMKeyPassword",
                            "tlsPEMKeyPassword",
                            moe::String,
                            "PEM file password",
                            {"net.ssl.PEMKeyPassword"},
                            {"sslPEMKeyPassword"})
        .setImplicit(moe::Value(std::string("")));

    options->addOptionChaining("net.tls.clusterFile",
                               "tlsClusterFile",
                               moe::String,
                               "Key file for internal TLS authentication",
                               {"net.ssl.clusterFile"},
                               {"sslClusterFile"});

    options
        ->addOptionChaining("net.tls.clusterPassword",
                            "tlsClusterPassword",
                            moe::String,
                            "Internal authentication key file password",
                            {"net.ssl.clusterPassword"},
                            {"sslClusterPassword"})
        .setImplicit(moe::Value(std::string("")));

    options->addOptionChaining("net.tls.CAFile",
                               "tlsCAFile",
                               moe::String,
                               "Certificate Authority file for TLS",
                               {"net.ssl.CAFile"},
                               {"sslCAFile"});

    options->addOptionChaining("net.tls.CRLFile",
                               "tlsCRLFile",
                               moe::String,
                               "Certificate Revocation List file for TLS",
                               {"net.ssl.CRLFile"},
                               {"sslCRLFile"});

    options
        ->addOptionChaining("net.tls.tlsCipherConfig",
                            "tlsCipherConfig",
                            moe::String,
                            "OpenSSL cipher configuration string",
                            {"net.ssl.sslCipherConfig"},
                            {"sslCipherConfig"})
        .hidden();

    options->addOptionChaining(
        "net.tls.disabledProtocols",
        "tlsDisabledProtocols",
        moe::String,
        "Comma separated list of TLS protocols to disable [TLS1_0,TLS1_1,TLS1_2]",
        {"net.ssl.disabledProtocols"},
        {"sslDisabledProtocols"});

    options->addOptionChaining("net.tls.weakCertificateValidation",
                               "tlsWeakCertificateValidation",
                               moe::Switch,
                               "Allow client to connect without presenting a certificate",
                               {"net.ssl.weakCertificateValidation"},
                               {"sslWeakCertificateValidation"});

    // Alias for --tlsWeakCertificateValidation.
    options->addOptionChaining("net.tls.allowConnectionsWithoutCertificates",
                               "tlsAllowConnectionsWithoutCertificates",
                               moe::Switch,
                               "Allow client to connect without presenting a certificate",
                               {"net.ssl.allowConnectionsWithoutCertificates"},
                               {"sslAllowConnectionsWithoutCertificates"});

    options->addOptionChaining("net.tls.allowInvalidHostnames",
                               "tlsAllowInvalidHostnames",
                               moe::Switch,
                               "Allow server certificates to provide non-matching hostnames",
                               {"net.ssl.allowInvalidHostnames"},
                               {"sslAllowInvalidHostnames"});

    options->addOptionChaining("net.tls.allowInvalidCertificates",
                               "tlsAllowInvalidCertificates",
                               moe::Switch,
                               "Allow connections to servers with invalid certificates",
                               {"net.ssl.allowInvalidCertificates"},
                               {"sslAllowInvalidCertificates"});

    options->addOptionChaining("net.tls.FIPSMode",
                               "tlsFIPSMode",
                               moe::Switch,
                               "Activate FIPS 140-2 mode at startup",
                               {"net.ssl.FIPSMode"},
                               {"sslFIPSMode"});

#ifdef MONGO_CONFIG_SSL_CERTIFICATE_SELECTORS
    options
        ->addOptionChaining("net.tls.certificateSelector",
                            "tlsCertificateSelector",
                            moe::String,
                            "TLS Certificate in system store",
                            {"net.ssl.certificateSelector"},
                            {"sslCertificateSelector"})
        .incompatibleWith("net.tls.PEMKeyFile")
        .incompatibleWith("net.tls.PEMKeyPassword");

    options
        ->addOptionChaining("net.tls.clusterCertificateSelector",
                            "tlsClusterCertificateSelector",
                            moe::String,
                            "SSL/TLS Certificate in system store for internal TLS authentication",
                            {"net.ssl.clusterCertificateSelector"},
                            {"sslClusterCertificateSelector"})
        .incompatibleWith("net.tls.clusterFile")
        .incompatibleWith("net.tls.clusterFilePassword");
#endif

    return Status::OK();
}

Status addSSLClientOptions(moe::OptionSection* options) {
    options->addOptionChaining(
        "tls", "tls", moe::Switch, "use TLS for all connections", {"ssl"}, {"ssl"});

    options
        ->addOptionChaining("tls.CAFile",
                            "tlsCAFile",
                            moe::String,
                            "Certificate Authority file for TLS",
                            {"ssl.CAFile"},
                            {"sslCAFile"})
        .requires("tls");

    options
        ->addOptionChaining("tls.PEMKeyFile",
                            "tlsPEMKeyFile",
                            moe::String,
                            "PEM certificate/key file for TLS",
                            {"ssl.PEMKeyFile"},
                            {"sslPEMKeyFile"})
        .requires("tls");

    options
        ->addOptionChaining("tls.PEMKeyPassword",
                            "tlsPEMKeyPassword",
                            moe::String,
                            "Password for key in PEM file for TLS",
                            {"ssl.PEMKeyPassword"},
                            {"sslPEMKeyPassword"})
        .requires("tls");

    options
        ->addOptionChaining("tls.CRLFile",
                            "tlsCRLFile",
                            moe::String,
                            "Certificate Revocation List file for TLS",
                            {"ssl.CRLFile"},
                            {"sslCRLFile"})
        .requires("tls")
        .requires("tls.CAFile");

    options
        ->addOptionChaining("net.tls.allowInvalidHostnames",
                            "tlsAllowInvalidHostnames",
                            moe::Switch,
                            "Allow connections to servers with non-matching hostnames",
                            {"net.ssl.allowInvalidHostnames"},
                            {"sslAllowInvalidHostnames"})
        .requires("tls");

    options
        ->addOptionChaining("tls.allowInvalidCertificates",
                            "tlsAllowInvalidCertificates",
                            moe::Switch,
                            "Allow connections to servers with invalid certificates",
                            {"ssl.allowInvalidCertificates"},
                            {"sslAllowInvalidCertificates"})
        .requires("tls");

    options->addOptionChaining("tls.FIPSMode",
                               "tlsFIPSMode",
                               moe::Switch,
                               "Activate FIPS 140-2 mode at startup",
                               {"ssl.FIPSMode"},
                               {"sslFIPSMode"});

#ifdef MONGO_CONFIG_SSL_CERTIFICATE_SELECTORS
    options
        ->addOptionChaining("tls.certificateSelector",
                            "tlsCertificateSelector",
                            moe::String,
                            "TLS Certificate in system store",
                            {"ssl.certificateSelector"},
                            {"sslCertificateSelector"})
        .incompatibleWith("tls.PEMKeyFile")
        .incompatibleWith("tls.PEMKeyPassword");
#endif

    options->addOptionChaining(
        "tls.disabledProtocols",
        "tlsDisabledProtocols",
        moe::String,
        "Comma separated list of TLS protocols to disable [TLS1_0,TLS1_1,TLS1_2]",
        {"ssl.disabledProtocols"},
        {"sslDisabledProtocols"});

    return Status::OK();
}

Status validateSSLServerOptions(const moe::Environment& params) {
#ifdef _WIN32
    if (params.count("install") || params.count("reinstall")) {
        if (params.count("net.tls.PEMKeyFile") &&
            !boost::filesystem::path(params["net.tls.PEMKeyFile"].as<string>()).is_absolute()) {
            return Status(ErrorCodes::BadValue,
                          "PEMKeyFile requires an absolute file path with Windows services");
        }

        if (params.count("net.tls.clusterFile") &&
            !boost::filesystem::path(params["net.tls.clusterFile"].as<string>()).is_absolute()) {
            return Status(ErrorCodes::BadValue,
                          "clusterFile requires an absolute file path with Windows services");
        }

        if (params.count("net.tls.CAFile") &&
            !boost::filesystem::path(params["net.tls.CAFile"].as<string>()).is_absolute()) {
            return Status(ErrorCodes::BadValue,
                          "CAFile requires an absolute file path with Windows services");
        }

        if (params.count("net.tls.CRLFile") &&
            !boost::filesystem::path(params["net.tls.CRLFile"].as<string>()).is_absolute()) {
            return Status(ErrorCodes::BadValue,
                          "CRLFile requires an absolute file path with Windows services");
        }
    }
#endif

    return Status::OK();
}

Status canonicalizeSSLServerOptions(moe::Environment* params) {
    if (params->count("net.tls.tlsOnNormalPorts") &&
        (*params)["net.tls.tlsOnNormalPorts"].as<bool>() == true) {
        Status ret = params->set("net.tls.mode", moe::Value(std::string("requireTLS")));
        if (!ret.isOK()) {
            return ret;
        }
        ret = params->remove("net.tls.tlsOnNormalPorts");
        if (!ret.isOK()) {
            return ret;
        }
    }

    return Status::OK();
}

Status storeSSLServerOptions(const moe::Environment& params) {
    if (params.count("net.tls.mode")) {
        std::string sslModeParam = params["net.tls.mode"].as<string>();
        if (sslModeParam == "disabled") {
            sslGlobalParams.sslMode.store(SSLParams::SSLMode_disabled);
        } else if (sslModeParam == "allowTLS") {
            sslGlobalParams.sslMode.store(SSLParams::SSLMode_allowSSL);
        } else if (sslModeParam == "preferTLS") {
            sslGlobalParams.sslMode.store(SSLParams::SSLMode_preferSSL);
        } else if (sslModeParam == "requireTLS") {
            sslGlobalParams.sslMode.store(SSLParams::SSLMode_requireSSL);
        } else {
            return Status(ErrorCodes::BadValue, "unsupported value for tlsMode " + sslModeParam);
        }
    } else if (params.count("net.ssl.mode")) {
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

    if (params.count("net.tls.PEMKeyFile")) {
        sslGlobalParams.sslPEMKeyFile =
            boost::filesystem::absolute(params["net.tls.PEMKeyFile"].as<string>()).generic_string();
    }

    if (params.count("net.tls.PEMKeyPassword")) {
        sslGlobalParams.sslPEMKeyPassword = params["net.tls.PEMKeyPassword"].as<string>();
    }

    if (params.count("net.tls.clusterFile")) {
        sslGlobalParams.sslClusterFile =
            boost::filesystem::absolute(params["net.tls.clusterFile"].as<string>())
                .generic_string();
    }

    if (params.count("net.tls.clusterPassword")) {
        sslGlobalParams.sslClusterPassword = params["net.tls.clusterPassword"].as<string>();
    }

    if (params.count("net.tls.CAFile")) {
        sslGlobalParams.sslCAFile =
            boost::filesystem::absolute(params["net.tls.CAFile"].as<std::string>())
                .generic_string();
    }

    if (params.count("net.tls.CRLFile")) {
        sslGlobalParams.sslCRLFile =
            boost::filesystem::absolute(params["net.tls.CRLFile"].as<std::string>())
                .generic_string();
    }

    if (params.count("net.tls.tlsCipherConfig")) {
        warning()
            << "net.tls.tlsCipherConfig is deprecated. It will be removed in a future release.";
        if (!sslGlobalParams.sslCipherConfig.empty()) {
            return Status(ErrorCodes::BadValue,
                          "net.tls.tlsCipherConfig is incompatible with the openTLSCipherConfig "
                          "setParameter");
        }
        sslGlobalParams.sslCipherConfig = params["net.tls.tlsCipherConfig"].as<string>();
    }

    if (params.count("net.tls.disabledProtocols")) {
        const auto status = storeDisabledProtocols(params["net.tls.disabledProtocols"].as<string>(),
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

    if (params.count("net.tls.weakCertificateValidation")) {
        sslGlobalParams.sslWeakCertificateValidation =
            params["net.tls.weakCertificateValidation"].as<bool>();
    } else if (params.count("net.tls.allowConnectionsWithoutCertificates")) {
        sslGlobalParams.sslWeakCertificateValidation =
            params["net.tls.allowConnectionsWithoutCertificates"].as<bool>();
    }

    if (params.count("net.tls.allowInvalidHostnames")) {
        sslGlobalParams.sslAllowInvalidHostnames =
            params["net.tls.allowInvalidHostnames"].as<bool>();
    }

    if (params.count("net.tls.allowInvalidCertificates")) {
        sslGlobalParams.sslAllowInvalidCertificates =
            params["net.tls.allowInvalidCertificates"].as<bool>();
    }

    if (params.count("net.tls.FIPSMode")) {
        sslGlobalParams.sslFIPSMode = params["net.tls.FIPSMode"].as<bool>();
    }

#ifdef MONGO_CONFIG_SSL_CERTIFICATE_SELECTORS
    if (params.count("net.tls.certificateSelector")) {
        const auto status =
            parseCertificateSelector(&sslGlobalParams.sslCertificateSelector,
                                     "net.tls.certificateSelector",
                                     params["net.tls.certificateSelector"].as<std::string>());
        if (!status.isOK()) {
            return status;
        }
    }

    if (params.count("net.tls.clusterCertificateSelector")) {
        const auto status = parseCertificateSelector(
            &sslGlobalParams.sslClusterCertificateSelector,
            "net.tls.clusterCertificateSelector",
            params["net.tls.clusterCertificateSelector"].as<std::string>());
        if (!status.isOK()) {
            return status;
        }
    }
#endif

    int clusterAuthMode = serverGlobalParams.clusterAuthMode.load();
    auto a = sslGlobalParams.sslMode.load();
    auto b = sslGlobalParams.sslMode.load();
    if (a == b) {
    }
    if (sslGlobalParams.sslMode.load() != SSLParams::SSLMode_disabled) {
        bool usingCertifiateSelectors = params.count("net.tls.certificateSelector");
        if (sslGlobalParams.sslPEMKeyFile.size() == 0 && !usingCertifiateSelectors) {
            return Status(ErrorCodes::BadValue,
                          "need tlsPEMKeyFile or certificateSelector when TLS is enabled");
        }
        if (!sslGlobalParams.sslCRLFile.empty() && sslGlobalParams.sslCAFile.empty()) {
            return Status(ErrorCodes::BadValue, "need tlsCAFile with tlsCRLFile");
        }

        std::string sslCANotFoundError(
            "No TLS certificate validation can be performed since"
            " no CA file has been provided; please specify an"
            " tlsCAFile parameter");

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
               params.count("net.tls.disabledProtocols") ||
#ifdef MONGO_CONFIG_SSL_CERTIFICATE_SELECTORS
               params.count("net.tls.certificateSelector") ||
               params.count("net.tls.clusterCertificateSelector") ||
#endif
               sslGlobalParams.sslWeakCertificateValidation) {
        return Status(ErrorCodes::BadValue,
                      "need to enable TLS via the sslMode/tlsMode flag when "
                      "using TLS configuration parameters");
    }
    if (clusterAuthMode == ServerGlobalParams::ClusterAuthMode_sendKeyFile ||
        clusterAuthMode == ServerGlobalParams::ClusterAuthMode_sendX509 ||
        clusterAuthMode == ServerGlobalParams::ClusterAuthMode_x509) {
        if (sslGlobalParams.sslMode.load() == SSLParams::SSLMode_disabled) {
            return Status(ErrorCodes::BadValue, "need to enable TLS via the tlsMode flag");
        }
    }
    if (sslGlobalParams.sslMode.load() == SSLParams::SSLMode_allowSSL) {
        // allowSSL and x509 is valid only when we are transitioning to auth.
        if (clusterAuthMode == ServerGlobalParams::ClusterAuthMode_sendX509 ||
            (clusterAuthMode == ServerGlobalParams::ClusterAuthMode_x509 &&
             !serverGlobalParams.transitionToAuth)) {
            return Status(ErrorCodes::BadValue,
                          "cannot have x.509 cluster authentication in allowTLS mode");
        }
    }
    return Status::OK();
}

Status storeSSLClientOptions(const moe::Environment& params) {
    if (params.count("tls") && params["tls"].as<bool>() == true) {
        sslGlobalParams.sslMode.store(SSLParams::SSLMode_requireSSL);
    }

    if (params.count("tls.PEMKeyFile")) {
        sslGlobalParams.sslPEMKeyFile = params["tls.PEMKeyFile"].as<std::string>();
    }

    if (params.count("tls.PEMKeyPassword")) {
        sslGlobalParams.sslPEMKeyPassword = params["tls.PEMKeyPassword"].as<std::string>();
    }

    if (params.count("tls.CAFile")) {
        sslGlobalParams.sslCAFile = params["tls.CAFile"].as<std::string>();
    }

    if (params.count("tls.CRLFile")) {
        sslGlobalParams.sslCRLFile = params["tls.CRLFile"].as<std::string>();
    }


    if (params.count("net.tls.allowInvalidHostnames")) {
        sslGlobalParams.sslAllowInvalidHostnames =
            params["net.tls.allowInvalidHostnames"].as<bool>();
    }

    if (params.count("tls.allowInvalidCertificates")) {
        sslGlobalParams.sslAllowInvalidCertificates = true;
    }

    if (params.count("tls.FIPSMode")) {
        sslGlobalParams.sslFIPSMode = true;
    }

    if (params.count("tls.disabledProtocols")) {
        const auto status =
            storeDisabledProtocols(params["tls.disabledProtocols"].as<std::string>());
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
    if (params.count("tls.certificateSelector")) {
        const auto status =
            parseCertificateSelector(&sslGlobalParams.sslCertificateSelector,
                                     "tls.certificateSelector",
                                     params["tls.certificateSelector"].as<std::string>());
        if (!status.isOK()) {
            return status;
        }
    }
#endif
    return Status::OK();
}

}  // namespace mongo
