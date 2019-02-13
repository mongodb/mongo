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

#include "mongo/platform/basic.h"

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
