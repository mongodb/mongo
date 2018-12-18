
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/platform/basic.h"

#include "mongo/util/net/ssl_options.h"

#include <boost/filesystem/operations.hpp>

#include "mongo/base/status.h"
#include "mongo/config.h"
#include "mongo/db/server_options.h"
#include "mongo/util/log.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/text.h"

#if MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_OPENSSL
#include <openssl/ssl.h>
#endif

namespace moe = mongo::optionenvironment;
using std::string;

// Export these to the process space for the sake of ssl_options_test.cpp
// but don't provide a header because we don't want to encourage use from elsewhere.
namespace mongo {
Status storeTLSLogVersion(const std::string& loggedProtocols) {
    // The tlsLogVersion field is composed of a comma separated list of protocols to
    // log. First, tokenize the field.
    const auto tokens = StringSplitter::split(loggedProtocols, ",");

    // All universally accepted tokens, and their corresponding enum representation.
    const std::map<std::string, SSLParams::Protocols> validConfigs{
        {"TLS1_0", SSLParams::Protocols::TLS1_0},
        {"TLS1_1", SSLParams::Protocols::TLS1_1},
        {"TLS1_2", SSLParams::Protocols::TLS1_2},
        {"TLS1_3", SSLParams::Protocols::TLS1_3},
    };

    // Map the tokens to their enum values, and push them onto the list of logged protocols.
    for (const std::string& token : tokens) {
        auto mappedToken = validConfigs.find(token);
        if (mappedToken != validConfigs.end()) {
            sslGlobalParams.tlsLogVersions.push_back(mappedToken->second);
            continue;
        }

        return Status(ErrorCodes::BadValue, "Unrecognized tlsLogVersions '" + token + "'");
    }

    return Status::OK();
}

namespace {

bool gImplicitDisableTLS10 = false;

// storeSSLServerOptions depends on serverGlobalParams.clusterAuthMode
// and IDL based storage actions, and therefore must run later.
MONGO_STARTUP_OPTIONS_POST(SSLServerOptions)(InitializerContext*) {
    auto& params = moe::startupOptionsParsed;

    if (params.count("net.tls.mode")) {
        std::string sslModeParam = params["net.tls.mode"].as<string>();
        auto swMode = SSLParams::tlsModeParse(sslModeParam);
        if (swMode.isOK()) {
            sslGlobalParams.sslMode.store(swMode.getValue());
        } else {
            return {ErrorCodes::BadValue, "unsupported value for tlsMode " + sslModeParam};
        }
    } else if (params.count("net.ssl.mode")) {
        std::string sslModeParam = params["net.ssl.mode"].as<string>();
        auto swMode = SSLParams::sslModeParse(sslModeParam);
        if (swMode.isOK()) {
            sslGlobalParams.sslMode.store(swMode.getValue());
        } else {
            return {ErrorCodes::BadValue, "unsupported value for sslMode " + sslModeParam};
        }
    }

    if (params.count("net.tls.certificateKeyFile")) {
        sslGlobalParams.sslPEMKeyFile =
            boost::filesystem::absolute(params["net.tls.certificateKeyFile"].as<string>())
                .generic_string();
    }

    if (params.count("net.tls.clusterFile")) {
        sslGlobalParams.sslClusterFile =
            boost::filesystem::absolute(params["net.tls.clusterFile"].as<string>())
                .generic_string();
    }

    if (params.count("net.tls.CAFile")) {
        sslGlobalParams.sslCAFile =
            boost::filesystem::absolute(params["net.tls.CAFile"].as<std::string>())
                .generic_string();
    }

    if (params.count("net.tls.clusterCAFile")) {
        sslGlobalParams.sslClusterCAFile =
            boost::filesystem::absolute(params["net.tls.clusterCAFile"].as<std::string>())
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
            return {ErrorCodes::BadValue,
                    "net.tls.tlsCipherConfig is incompatible with the openTLSCipherConfig "
                    "setParameter"};
        }
        sslGlobalParams.sslCipherConfig = params["net.tls.tlsCipherConfig"].as<string>();
    }

    if (params.count("net.tls.disabledProtocols")) {
        const auto status =
            storeSSLDisabledProtocols(params["net.tls.disabledProtocols"].as<string>(),
                                      SSLDisabledProtocolsMode::kAcceptNegativePrefix);
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
        gImplicitDisableTLS10 = true;
        sslGlobalParams.sslDisabledProtocols.push_back(SSLParams::Protocols::TLS1_0);
#endif
    }

    if (params.count("net.tls.logVersions")) {
        const auto status = storeTLSLogVersion(params["net.tls.logVersions"].as<string>());
        if (!status.isOK()) {
            return status;
        }
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

    const int clusterAuthMode = serverGlobalParams.clusterAuthMode.load();
    if (sslGlobalParams.sslMode.load() != SSLParams::SSLMode_disabled) {
        bool usingCertifiateSelectors = params.count("net.tls.certificateSelector");
        if (sslGlobalParams.sslPEMKeyFile.size() == 0 && !usingCertifiateSelectors) {
            return {ErrorCodes::BadValue,
                    "need tlsCertificateKeyFile or certificateSelector when TLS is enabled"};
        }
        if (!sslGlobalParams.sslCRLFile.empty() && sslGlobalParams.sslCAFile.empty()) {
            return {ErrorCodes::BadValue, "need tlsCAFile with tlsCRLFile"};
        }

        std::string sslCANotFoundError(
            "No TLS certificate validation can be performed since"
            " no CA file has been provided; please specify an"
            " tlsCAFile parameter");

        // When using cetificate selectors, we use the local system certificate store for verifying
        // X.509 certificates for auth instead of relying on a CA file.
        if (sslGlobalParams.sslCAFile.empty() && !usingCertifiateSelectors &&
            clusterAuthMode == ServerGlobalParams::ClusterAuthMode_x509) {
            return {ErrorCodes::BadValue, sslCANotFoundError};
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
        return {ErrorCodes::BadValue,
                "need to enable TLS via the sslMode/tlsMode flag when "
                "using TLS configuration parameters"};
    }

    if (clusterAuthMode == ServerGlobalParams::ClusterAuthMode_sendKeyFile ||
        clusterAuthMode == ServerGlobalParams::ClusterAuthMode_sendX509 ||
        clusterAuthMode == ServerGlobalParams::ClusterAuthMode_x509) {
        if (sslGlobalParams.sslMode.load() == SSLParams::SSLMode_disabled) {
            return {ErrorCodes::BadValue, "need to enable TLS via the tlsMode flag"};
        }
    }

    if (sslGlobalParams.sslMode.load() == SSLParams::SSLMode_allowSSL) {
        // allowSSL and x509 is valid only when we are transitioning to auth.
        if (clusterAuthMode == ServerGlobalParams::ClusterAuthMode_sendX509 ||
            (clusterAuthMode == ServerGlobalParams::ClusterAuthMode_x509 &&
             !serverGlobalParams.transitionToAuth)) {
            return {ErrorCodes::BadValue,
                    "cannot have x.509 cluster authentication in allowTLS mode"};
        }
    }
    return Status::OK();
}

// Alias --tlsOnNormalPorts as --tlsMode=requireTLS
Status canonicalizeSSLServerOptions(moe::Environment* params) {
    if (params->count("net.tls.tlsOnNormalPorts") &&
        (*params)["net.tls.tlsOnNormalPorts"].as<bool>() == true) {
        // Must remove the old setting before adding the new one
        // since as soon as we add it, the incompatibleWith validation will run.
        auto ret = params->remove("net.tls.tlsOnNormalPorts");
        if (!ret.isOK()) {
            return ret;
        }

        ret = params->set("net.tls.mode", moe::Value(std::string("requireTLS")));
        if (!ret.isOK()) {
            return ret;
        }
    }

    return Status::OK();
}

MONGO_STARTUP_OPTIONS_VALIDATE(SSLServerOptions)(InitializerContext*) {
    auto status = canonicalizeSSLServerOptions(&moe::startupOptionsParsed);
    if (!status.isOK()) {
        return status;
    }

#ifdef _WIN32
    const auto& params = moe::startupOptionsParsed;

    if (params.count("install") || params.count("reinstall")) {
        if (params.count("net.tls.certificateKeyFile") &&
            !boost::filesystem::path(params["net.tls.certificateKeyFile"].as<string>())
                 .is_absolute()) {
            return {ErrorCodes::BadValue,
                    "PEMKeyFile requires an absolute file path with Windows services"};
        }

        if (params.count("net.tls.clusterFile") &&
            !boost::filesystem::path(params["net.tls.clusterFile"].as<string>()).is_absolute()) {
            return {ErrorCodes::BadValue,
                    "clusterFile requires an absolute file path with Windows services"};
        }

        if (params.count("net.tls.CAFile") &&
            !boost::filesystem::path(params["net.tls.CAFile"].as<string>()).is_absolute()) {
            return {ErrorCodes::BadValue,
                    "CAFile requires an absolute file path with Windows services"};
        }

        if (params.count("net.tls.CRLFile") &&
            !boost::filesystem::path(params["net.tls.CRLFile"].as<string>()).is_absolute()) {
            return {ErrorCodes::BadValue,
                    "CRLFile requires an absolute file path with Windows services"};
        }
    }
#endif

    return Status::OK();
}

// This warning must be deferred until after
// ServerLogRedirection has started up so that
// it goes to the right place.
MONGO_INITIALIZER_WITH_PREREQUISITES(ImplicitDisableTLS10Warning, ("ServerLogRedirection"))
(InitializerContext*) {
    if (gImplicitDisableTLS10) {
        log() << "Automatically disabling TLS 1.0, to force-enable TLS 1.0 "
                 "specify --sslDisabledProtocols 'none'";
    }
    return Status::OK();
}

}  // namespace
}  // namespace mongo
