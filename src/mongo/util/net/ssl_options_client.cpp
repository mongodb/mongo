/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/util/net/ssl_options.h"

#include <boost/filesystem/operations.hpp>

#include "mongo/base/status.h"
#include "mongo/config.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"

#if MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_OPENSSL
#include <openssl/ssl.h>
#endif

using namespace mongo;
namespace moe = mongo::optionenvironment;
using std::string;

namespace {
MONGO_GENERAL_STARTUP_OPTIONS_REGISTER(SSLClientOptions)(InitializerContext*) {
    auto& options = moe::startupOptions;

    options.addOptionChaining(
        "tls", "tls", moe::Switch, "use TLS for all connections", {"ssl"}, {"ssl"});

    options
        .addOptionChaining("tls.CAFile",
                           "tlsCAFile",
                           moe::String,
                           "Certificate Authority file for TLS",
                           {"ssl.CAFile"},
                           {"sslCAFile"})
        .requires("tls");

    options
        .addOptionChaining("tls.PEMKeyFile",
                           "tlsPEMKeyFile",
                           moe::String,
                           "PEM certificate/key file for TLS",
                           {"ssl.PEMKeyFile"},
                           {"sslPEMKeyFile"})
        .requires("tls");

    options
        .addOptionChaining("tls.PEMKeyPassword",
                           "tlsPEMKeyPassword",
                           moe::String,
                           "Password for key in PEM file for TLS",
                           {"ssl.PEMKeyPassword"},
                           {"sslPEMKeyPassword"})
        .requires("tls");

    options
        .addOptionChaining("tls.CRLFile",
                           "tlsCRLFile",
                           moe::String,
                           "Certificate Revocation List file for TLS",
                           {"ssl.CRLFile"},
                           {"sslCRLFile"})
        .requires("tls")
        .requires("tls.CAFile");

    options
        .addOptionChaining("net.tls.allowInvalidHostnames",
                           "tlsAllowInvalidHostnames",
                           moe::Switch,
                           "Allow connections to servers with non-matching hostnames",
                           {"net.ssl.allowInvalidHostnames"},
                           {"sslAllowInvalidHostnames"})
        .requires("tls");

    options
        .addOptionChaining("tls.allowInvalidCertificates",
                           "tlsAllowInvalidCertificates",
                           moe::Switch,
                           "Allow connections to servers with invalid certificates",
                           {"ssl.allowInvalidCertificates"},
                           {"sslAllowInvalidCertificates"})
        .requires("tls");

    options.addOptionChaining("tls.FIPSMode",
                              "tlsFIPSMode",
                              moe::Switch,
                              "Activate FIPS 140-2 mode at startup",
                              {"ssl.FIPSMode"},
                              {"sslFIPSMode"});

#ifdef MONGO_CONFIG_SSL_CERTIFICATE_SELECTORS
    options
        .addOptionChaining("tls.certificateSelector",
                           "tlsCertificateSelector",
                           moe::String,
                           "TLS Certificate in system store",
                           {"ssl.certificateSelector"},
                           {"sslCertificateSelector"})
        .incompatibleWith("tls.PEMKeyFile")
        .incompatibleWith("tls.PEMKeyPassword");
#endif

    options.addOptionChaining(
        "tls.disabledProtocols",
        "tlsDisabledProtocols",
        moe::String,
        "Comma separated list of TLS protocols to disable [TLS1_0,TLS1_1,TLS1_2]",
        {"ssl.disabledProtocols"},
        {"sslDisabledProtocols"});

    return Status::OK();
}

MONGO_STARTUP_OPTIONS_STORE(SSLClientOptions)(InitializerContext*) {
    const auto& params = moe::startupOptionsParsed;

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
            storeSSLDisabledProtocols(params["tls.disabledProtocols"].as<std::string>());
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

}  // namespace
