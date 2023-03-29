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

#pragma once

#include <boost/optional.hpp>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/client/connection_string.h"
#include "mongo/config.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/auth/role_name.h"

namespace mongo {

#if (MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_WINDOWS) || \
    (MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_APPLE)
#define MONGO_CONFIG_SSL_CERTIFICATE_SELECTORS 1
#endif

namespace optionenvironment {
class OptionSection;
class Environment;
}  // namespace optionenvironment

constexpr auto kSSLCipherConfigDefault = "HIGH:!EXPORT:!aNULL@STRENGTH"_sd;

struct SSLParams {
    using TLSCATrusts = std::map<SHA256Block, std::set<RoleName>>;

    enum class Protocols { TLS1_0, TLS1_1, TLS1_2, TLS1_3 };
    AtomicWord<int> sslMode;        // --tlsMode - the TLS operation mode, see enum SSLModes
    std::string sslPEMTempDHParam;  // --setParameter OpenSSLDiffieHellmanParameters=file : PEM file
                                    // with DH parameters.
    std::string sslPEMKeyFile;      // --tlsCertificateKeyFile
    std::string sslPEMKeyPassword;  // --tlsCertificateKeyFilePassword
    std::string sslClusterFile;     // --tlsInternalKeyFile
    std::string sslClusterPassword;             // --tlsInternalKeyPassword
    std::string sslCAFile;                      // --tlsCAFile
    std::string sslClusterCAFile;               // --tlsClusterCAFile
    std::string sslCRLFile;                     // --tlsCRLFile
    std::string sslCipherConfig;                // --tlsCipherConfig
    std::string sslCipherSuiteConfig;           // --tlsCipherSuiteConfig
    std::string clusterAuthX509ExtensionValue;  // --tlsClusterAuthX509ExtensionValue
    std::string clusterAuthX509Attributes;      // --tlsClusterAuthX509Attributes

    boost::optional<TLSCATrusts> tlsCATrusts;  // --setParameter tlsCATrusts

    struct CertificateSelector {
        std::string subject;
        std::vector<uint8_t> thumbprint;
        bool empty() const {
            return subject.empty() && thumbprint.empty();
        }
    };
#ifdef MONGO_CONFIG_SSL_CERTIFICATE_SELECTORS
    CertificateSelector sslCertificateSelector;         // --sslCertificateSelector
    CertificateSelector sslClusterCertificateSelector;  // --sslClusterCertificateSelector
#endif

    std::vector<Protocols> sslDisabledProtocols;  // --sslDisabledProtocols
    std::vector<Protocols> tlsLogVersions;        // --tlsLogVersion
    bool sslWeakCertificateValidation = false;    // --sslWeakCertificateValidation
    bool sslFIPSMode = false;                     // --sslFIPSMode
    bool sslAllowInvalidCertificates = false;     // --sslAllowInvalidCertificates
    bool sslAllowInvalidHostnames = false;        // --sslAllowInvalidHostnames
    bool disableNonSSLConnectionLogging =
        false;  // --setParameter disableNonSSLConnectionLogging=true
    bool disableNonSSLConnectionLoggingSet = false;
    bool suppressNoTLSPeerCertificateWarning =
        false;  // --setParameter suppressNoTLSPeerCertificateWarning
    bool tlsWithholdClientCertificate = false;  // --setParameter tlsWithholdClientCertificate

    SSLParams() : sslCipherConfig(kSSLCipherConfigDefault) {
        sslMode.store(SSLMode_disabled);
    }

    enum SSLModes : int {
        /**
         * Make unencrypted outgoing connections and do not accept incoming SSL-connections.
         */
        SSLMode_disabled,

        /**
         * Make unencrypted outgoing connections and accept both unencrypted and SSL-connections.
         */
        SSLMode_allowSSL,

        /**
         * Make outgoing SSL-connections and accept both unecrypted and SSL-connections.
         */
        SSLMode_preferSSL,

        /**
         * Make outgoing SSL-connections and only accept incoming SSL-connections.
         */
        SSLMode_requireSSL
    };

    static StatusWith<SSLModes> sslModeParse(StringData strMode);
    static StatusWith<SSLModes> tlsModeParse(StringData strMode);
    static std::string sslModeFormat(int mode);
    static std::string tlsModeFormat(int mode);
};

extern SSLParams sslGlobalParams;

// Additional SSL Params that could be used to augment a particular connection
// or have limited lifetime. In all cases, the fields stored here are not appropriate
// to be part of sslGlobalParams.
struct TransientSSLParams {
    ConnectionString targetedClusterConnectionString;
    std::string sslClusterPEMPayload;
};

/**
 * Older versions of mongod/mongos accepted --sslDisabledProtocols values
 * in the form 'noTLS1_0,noTLS1_1'.  kAcceptNegativePrefix allows us to
 * continue accepting this format on mongod/mongos while only supporting
 * the "standard" TLS1_X format in the shell.
 */
enum class SSLDisabledProtocolsMode {
    kStandardFormat,
    kAcceptNegativePrefix,
};

Status storeSSLDisabledProtocols(
    const std::string& disabledProtocols,
    SSLDisabledProtocolsMode mode = SSLDisabledProtocolsMode::kStandardFormat);

/**
 * The global SSL configuration. This should be accessed only after global initialization has
 * completed. If it must be accessed in an initializer, the initializer should have
 * "EndStartupOptionStorage" as a prerequisite.
 */
const SSLParams& getSSLGlobalParams();

Status parseCertificateSelector(SSLParams::CertificateSelector* selector,
                                StringData name,
                                StringData value);

}  // namespace mongo
