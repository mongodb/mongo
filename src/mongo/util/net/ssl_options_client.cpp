// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/status.h"
#include "mongo/config.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"

#if MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_OPENSSL
#include <openssl/ssl.h>
#endif

using namespace mongo;

namespace {

MONGO_STARTUP_OPTIONS_STORE(SSLClientOptions)(InitializerContext*) {
    const auto& params = mongo::optionenvironment::startupOptionsParsed;

    if (params.count("tls") && params["tls"].as<bool>() == true) {
        sslGlobalParams.sslMode.store(SSLParams::SSLMode_requireSSL);
    }

    if (params.count("tls.disabledProtocols")) {
        uassertStatusOK(
            storeSSLDisabledProtocols(params["tls.disabledProtocols"].as<std::string>()));
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
#if (MONGO_CONFIG_SSL_PROVIDER != MONGO_CONFIG_SSL_PROVIDER_OPENSSL) || \
    (OPENSSL_VERSION_NUMBER >= 0x1000106f) /* 1.0.1f */
        // Disables TLS 1.1 as well for clients which support TLS 1.2 and later.
        sslGlobalParams.sslDisabledProtocols.push_back(SSLParams::Protocols::TLS1_1);
#endif
    }

#ifdef MONGO_CONFIG_SSL_CERTIFICATE_SELECTORS
    if (params.count("tls.certificateSelector")) {
        uassertStatusOK(
            parseCertificateSelector(&sslGlobalParams.sslCertificateSelector,
                                     "tls.certificateSelector",
                                     params["tls.certificateSelector"].as<std::string>()));
    }
#endif
}

}  // namespace
