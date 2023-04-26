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

#include "mongo/util/net/ssl_manager.h"

#include <asio.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <fstream>
#include <memory>
#include <string>
#include <tuple>
#include <vector>
#include <winhttp.h>

#include "mongo/base/init.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/config.h"
#include "mongo/db/connection_health_metrics_parameter_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/transport/ssl_connection_context.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/hex.h"
#include "mongo/util/net/private/ssl_expiration.h"
#include "mongo/util/net/sockaddr.h"
#include "mongo/util/net/socket_exception.h"
#include "mongo/util/net/ssl.hpp"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/net/ssl_parameters_gen.h"
#include "mongo/util/net/ssl_peer_info.h"
#include "mongo/util/net/ssl_types.h"
#include "mongo/util/net/ssl_util.h"
#include "mongo/util/str.h"
#include "mongo/util/text.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {

using transport::SSLConnectionContext;

extern SSLManagerCoordinator* theSSLManagerCoordinator;

namespace {

// This failpoint is a no-op on Windows.
MONGO_FAIL_POINT_DEFINE(disableStapling);

/**
 * Free a Certificate Context.
 */
struct CERTFree {
    void operator()(const CERT_CONTEXT* p) noexcept {
        if (p) {
            ::CertFreeCertificateContext(p);
        }
    }
};

using UniqueCertificate = std::unique_ptr<const CERT_CONTEXT, CERTFree>;

/**
 * Free a CRL Handle
 */
struct CryptCRLFree {
    void operator()(const CRL_CONTEXT* p) noexcept {
        if (p) {
            ::CertFreeCRLContext(p);
        }
    }
};

using UniqueCRL = std::unique_ptr<const CRL_CONTEXT, CryptCRLFree>;


/**
 * Free a Certificate Chain Context
 */
struct CryptCertChainFree {
    void operator()(const CERT_CHAIN_CONTEXT* p) noexcept {
        if (p) {
            ::CertFreeCertificateChain(p);
        }
    }
};

using UniqueCertChain = std::unique_ptr<const CERT_CHAIN_CONTEXT, CryptCertChainFree>;


/**
 * A simple generic class to manage Windows handle like things. Behaves similiar to std::unique_ptr.
 *
 * Only supports move.
 */
template <typename HandleT, class Deleter>
class AutoHandle {
public:
    AutoHandle() : _handle(0) {}
    AutoHandle(HandleT handle) : _handle(handle) {}
    AutoHandle(AutoHandle<HandleT, Deleter>&& handle) : _handle(handle._handle) {
        handle._handle = 0;
    }

    ~AutoHandle() {
        if (_handle != 0) {
            Deleter()(_handle);
        }
    }

    AutoHandle(const AutoHandle&) = delete;

    /**
     * Take ownership of the handle.
     */
    AutoHandle& operator=(const HandleT other) {
        _handle = other;
        return *this;
    }

    AutoHandle& operator=(const AutoHandle<HandleT, Deleter>& other) = delete;

    AutoHandle& operator=(AutoHandle<HandleT, Deleter>&& other) {
        _handle = other._handle;
        other._handle = 0;
        return *this;
    }

    operator HandleT() {
        return _handle;
    }

private:
    HandleT _handle;
};

/**
 * Free a HCRYPTPROV  Handle
 */
struct CryptProviderFree {
    void operator()(HCRYPTPROV const h) noexcept {
        if (h) {
            ::CryptReleaseContext(h, 0);
        }
    }
};

using UniqueCryptProvider = AutoHandle<HCRYPTPROV, CryptProviderFree>;

/**
 * Free a HCRYPTKEY  Handle
 */
struct CryptKeyFree {
    void operator()(HCRYPTKEY const h) noexcept {
        if (h) {
            ::CryptDestroyKey(h);
        }
    }
};

using UniqueCryptKey = AutoHandle<HCRYPTKEY, CryptKeyFree>;

/**
 * Free a CERTSTORE Handle
 */
struct CertStoreFree {
    void operator()(HCERTSTORE const p) noexcept {
        if (p) {
            // For leak detection, add CERT_CLOSE_STORE_CHECK_FLAG
            // Currently, we open very few cert stores and let the certs live beyond the cert store
            // so the leak detection flag is not useful.
            ::CertCloseStore(p, 0);
        }
    }
};

using UniqueCertStore = AutoHandle<HCERTSTORE, CertStoreFree>;

/**
 * Free a HCERTCHAINENGINE Handle
 */
struct CertChainEngineFree {
    void operator()(HCERTCHAINENGINE const p) noexcept {
        if (p) {
            ::CertFreeCertificateChainEngine(p);
        }
    }
};

using UniqueCertChainEngine = AutoHandle<HCERTCHAINENGINE, CertChainEngineFree>;

/**
 * The lifetime of a private key of a certificate loaded from a PEM is bound to the CryptContext's
 * lifetime
 * so we treat the certificate and cryptcontext as a pair.
 */
using UniqueCertificateWithPrivateKey = std::tuple<UniqueCertificate, UniqueCryptProvider>;


StatusWith<stdx::unordered_set<RoleName>> parsePeerRoles(PCCERT_CONTEXT cert) {
    PCERT_EXTENSION extension = CertFindExtension(mongodbRolesOID.identifier.c_str(),
                                                  cert->pCertInfo->cExtension,
                                                  cert->pCertInfo->rgExtension);

    stdx::unordered_set<RoleName> roles;

    if (!extension) {
        return roles;
    }

    return parsePeerRoles(
        ConstDataRange(reinterpret_cast<char*>(extension->Value.pbData),
                       reinterpret_cast<char*>(extension->Value.pbData) + extension->Value.cbData));
}

/**
 * Manage state for a SSL Connection. Used by the Socket class.
 */
class SSLConnectionWindows : public SSLConnectionInterface {
public:
    SCHANNEL_CRED* _cred;
    Socket* socket;
    asio::ssl::detail::engine _engine;

    std::vector<char> _tempBuffer;

    SSLConnectionWindows(SCHANNEL_CRED* cred, Socket* sock, const char* initialBytes, int len);

    ~SSLConnectionWindows();
};


class SSLManagerWindows : public SSLManagerInterface {
public:
    explicit SSLManagerWindows(const SSLParams& params, bool isServer);

    /**
     * Initializes an OpenSSL context according to the provided settings. Only settings which are
     * acceptable on non-blocking connections are set.
     */
    Status initSSLContext(SCHANNEL_CRED* cred,
                          const SSLParams& params,
                          ConnectionDirection direction) final;

    void registerOwnedBySSLContext(
        std::weak_ptr<const transport::SSLConnectionContext> ownedByContext) final;

    SSLConnectionInterface* connect(Socket* socket) final;

    SSLConnectionInterface* accept(Socket* socket, const char* initialBytes, int len) final;

    SSLPeerInfo parseAndValidatePeerCertificateDeprecated(const SSLConnectionInterface* conn,
                                                          const std::string& remoteHost,
                                                          const HostAndPort& hostForLogging) final;

    Future<SSLPeerInfo> parseAndValidatePeerCertificate(PCtxtHandle ssl,
                                                        boost::optional<std::string> sni,
                                                        const std::string& remoteHost,
                                                        const HostAndPort& hostForLogging,
                                                        const ExecutorPtr& reactor) final;

    Status stapleOCSPResponse(SCHANNEL_CRED* cred, bool asyncOCSPStaple) final;

    const SSLConfiguration& getSSLConfiguration() const final {
        return _sslConfiguration;
    }

    int SSL_read(SSLConnectionInterface* conn, void* buf, int num) final;

    int SSL_write(SSLConnectionInterface* conn, const void* buf, int num) final;

    int SSL_shutdown(SSLConnectionInterface* conn) final;

    SSLInformationToLog getSSLInformationToLog() const final;

    void stopJobs() final;

private:
    Status _loadCertificates(const SSLParams& params);

    void _handshake(SSLConnectionWindows* conn, bool client);

    Status _validateCertificate(PCCERT_CONTEXT cert,
                                SSLX509Name* subjectName,
                                Date_t* serverCertificateExpirationDate);

    struct CAEngine {
        CERT_CHAIN_ENGINE_CONFIG machineConfig;
        UniqueCertChainEngine machine;
        CERT_CHAIN_ENGINE_CONFIG userConfig;
        UniqueCertChainEngine user;
        UniqueCertStore CAstore;
        bool hasCRL;
    };

    Status _initChainEngines(CAEngine* engine);

private:
    bool _weakValidation;
    bool _allowInvalidCertificates;
    bool _allowInvalidHostnames;
    bool _suppressNoCertificateWarning;
    SSLConfiguration _sslConfiguration;

    SCHANNEL_CRED _clientCred;
    SCHANNEL_CRED _serverCred;

    UniqueCertificateWithPrivateKey _pemCertificate;
    UniqueCertificateWithPrivateKey _clusterPEMCertificate;
    std::array<PCCERT_CONTEXT, 1> _clientCertificates;
    std::array<PCCERT_CONTEXT, 1> _serverCertificates;

    /* _clientEngine represents the CA to use when acting as a client
     * and validating remotes during outbound connections.
     * This comes from, in order, --tlsCAFile, or the system CA.
     */
    CAEngine _clientEngine;

    /* _serverEngine represents the CA to use when acting as a server
     * and validating remotes during inbound connections.
     * This comes from --tlsClusterCAFile, if available,
     * otherwise it inherits from _clientEngine.
     */
    CAEngine _serverEngine;

    UniqueCertificate _sslCertificate;
    UniqueCertificate _sslClusterCertificate;

    // Weak pointer to verify that this manager is still owned by this context.
    // Will be used if stapling is implemented.
    synchronized_value<std::weak_ptr<const SSLConnectionContext>> _ownedByContext;
};

GlobalInitializerRegisterer sslManagerInitializer(
    "SSLManager",
    [](InitializerContext*) {
        if (!isSSLServer || (sslGlobalParams.sslMode.load() != SSLParams::SSLMode_disabled)) {
            theSSLManagerCoordinator = new SSLManagerCoordinator();
        }
        return Status::OK();
    },
    [](DeinitializerContext* context) {
        if (theSSLManagerCoordinator) {
            delete theSSLManagerCoordinator;
            theSSLManagerCoordinator = nullptr;
        }

        return Status::OK();
    },
    {"EndStartupOptionHandling"},
    {});

SSLConnectionWindows::SSLConnectionWindows(SCHANNEL_CRED* cred,
                                           Socket* sock,
                                           const char* initialBytes,
                                           int len)
    : _cred(cred), socket(sock), _engine(_cred, removeFQDNRoot(socket->remoteAddr().hostOrIp())) {

    _tempBuffer.resize(17 * 1024);

    if (len > 0) {
        _engine.put_input(asio::const_buffer(initialBytes, len));
    }
}

SSLConnectionWindows::~SSLConnectionWindows() {}

}  // namespace

// Global variable indicating if this is a server or a client instance
bool isSSLServer = false;

std::shared_ptr<SSLManagerInterface> SSLManagerInterface::create(
    const SSLParams& params,
    const boost::optional<TransientSSLParams>& transientSSLParams,
    bool isServer) {
    return std::make_shared<SSLManagerWindows>(params, isServer);
}

std::shared_ptr<SSLManagerInterface> SSLManagerInterface::create(const SSLParams& params,
                                                                 bool isServer) {
    return std::make_shared<SSLManagerWindows>(params, isServer);
}

namespace {

StatusWith<std::vector<std::string>> getSubjectAlternativeNames(PCCERT_CONTEXT cert);

SSLManagerWindows::SSLManagerWindows(const SSLParams& params, bool isServer)
    : _weakValidation(params.sslWeakCertificateValidation),
      _allowInvalidCertificates(params.sslAllowInvalidCertificates),
      _allowInvalidHostnames(params.sslAllowInvalidHostnames),
      _suppressNoCertificateWarning(params.suppressNoTLSPeerCertificateWarning) {

    if (params.sslFIPSMode) {
        BOOLEAN enabled = FALSE;
        BCryptGetFipsAlgorithmMode(&enabled);
        if (!enabled) {
            LOGV2_FATAL_NOTRACE(50744, "FIPS modes is not enabled on the operating system.");
        }
    }

    uassertStatusOK(_loadCertificates(params));

    uassertStatusOK(initSSLContext(&_clientCred, params, ConnectionDirection::kOutgoing));

    // Certificates may not have been loaded. This typically occurs in unit tests.
    if (_clientCertificates[0] != nullptr) {
        uassertStatusOK(_validateCertificate(
            _clientCertificates[0], &_sslConfiguration.clientSubjectName, NULL));
    }

    // SSL server specific initialization
    if (isServer) {
        uassertStatusOK(initSSLContext(&_serverCred, params, ConnectionDirection::kIncoming));

        if (_serverCertificates[0] != nullptr) {
            SSLX509Name subjectName;
            uassertStatusOK(
                _validateCertificate(_serverCertificates[0],
                                     &subjectName,
                                     &_sslConfiguration.serverCertificateExpirationDate));
            uassertStatusOK(_sslConfiguration.setServerSubjectName(std::move(subjectName)));

            auto swSans = getSubjectAlternativeNames(_serverCertificates[0]);
            const bool hasSan = swSans.isOK() && (0 != swSans.getValue().size());
            if (!hasSan) {
                LOGV2_WARNING_OPTIONS(
                    551192,
                    {logv2::LogTag::kStartupWarnings},
                    "Server certificate has no compatible Subject Alternative Name. "
                    "This may prevent TLS clients from connecting");
            }
        }

        // Monitor the server certificate's expiration
        CertificateExpirationMonitor::get()->updateExpirationDeadline(
            _sslConfiguration.serverCertificateExpirationDate);
    }

    uassertStatusOK(_initChainEngines(&_serverEngine));
    uassertStatusOK(_initChainEngines(&_clientEngine));
}

StatusWith<UniqueCertChainEngine> initChainEngine(CERT_CHAIN_ENGINE_CONFIG* chainEngineConfig,
                                                  HCERTSTORE certStore,
                                                  DWORD flags) {
    memset(chainEngineConfig, 0, sizeof(*chainEngineConfig));
    chainEngineConfig->cbSize = sizeof(*chainEngineConfig);

    // If the user specified a CA file, then we need to restrict our trusted roots to this store.
    // This means that the CA file overrules the Windows cert store.
    if (certStore) {
        chainEngineConfig->hExclusiveRoot = certStore;
    }
    chainEngineConfig->dwFlags = flags;

    HCERTCHAINENGINE chainEngine;
    if (!CertCreateCertificateChainEngine(chainEngineConfig, &chainEngine)) {
        auto ec = lastSystemError();
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream()
                          << "CertCreateCertificateChainEngine failed: " << errorMessage(ec));
    }

    return {chainEngine};
}

Status SSLManagerWindows::_initChainEngines(CAEngine* engine) {
    auto swMachine = initChainEngine(
        &engine->machineConfig, engine->CAstore, CERT_CHAIN_USE_LOCAL_MACHINE_STORE);

    if (!swMachine.isOK()) {
        return swMachine.getStatus();
    }

    engine->machine = std::move(swMachine.getValue());

    auto swUser = initChainEngine(&engine->userConfig, engine->CAstore, 0);

    if (!swUser.isOK()) {
        return swUser.getStatus();
    }

    engine->user = std::move(swUser.getValue());

    return Status::OK();
}

int SSLManagerWindows::SSL_read(SSLConnectionInterface* connInterface, void* buf, int num) {
    SSLConnectionWindows* conn = static_cast<SSLConnectionWindows*>(connInterface);

    while (true) {
        size_t bytes_transferred;
        asio::error_code ec;
        asio::ssl::detail::engine::want want =
            conn->_engine.read(asio::mutable_buffer(buf, num), ec, bytes_transferred);
        if (ec) {
            throwSocketError(SocketErrorKind::RECV_ERROR, ec.message());
        }

        switch (want) {
            case asio::ssl::detail::engine::want_input_and_retry: {
                // ASIO wants more data before it can continue:
                // 1. fetch some from the network
                // 2. give it to ASIO
                // 3. retry
                int ret = recv(conn->socket->rawFD(),
                               conn->_tempBuffer.data(),
                               conn->_tempBuffer.size(),
                               portRecvFlags);
                if (ret == SOCKET_ERROR) {
                    conn->socket->handleRecvError(ret, num);
                }

                conn->_engine.put_input(asio::const_buffer(conn->_tempBuffer.data(), ret));

                continue;
            }
            case asio::ssl::detail::engine::want_nothing: {
                // ASIO wants nothing, return to caller with anything transfered.
                return bytes_transferred;
            }
            default:
                LOGV2_FATAL(23282,
                            "Unexpected ASIO state: {static_cast_int_want}",
                            "static_cast_int_want"_attr = static_cast<int>(want));
                MONGO_UNREACHABLE;
        }
    }
}

int SSLManagerWindows::SSL_write(SSLConnectionInterface* connInterface, const void* buf, int num) {
    SSLConnectionWindows* conn = static_cast<SSLConnectionWindows*>(connInterface);

    while (true) {
        size_t bytes_transferred;
        asio::error_code ec;
        asio::ssl::detail::engine::want want =
            conn->_engine.write(asio::const_buffer(buf, num), ec, bytes_transferred);
        if (ec) {
            throwSocketError(SocketErrorKind::SEND_ERROR, ec.message());
        }

        switch (want) {
            case asio::ssl::detail::engine::want_output:
            case asio::ssl::detail::engine::want_output_and_retry: {
                // ASIO wants us to send data out:
                // 1. get data from ASIO
                // 2. give it to the network
                // 3. retry if needed

                asio::mutable_buffer outBuf = conn->_engine.get_output(
                    asio::mutable_buffer(conn->_tempBuffer.data(), conn->_tempBuffer.size()));

                int ret = send(conn->socket->rawFD(),
                               reinterpret_cast<const char*>(outBuf.data()),
                               outBuf.size(),
                               portSendFlags);
                if (ret == SOCKET_ERROR) {
                    conn->socket->handleSendError(ret, "");
                }

                if (want == asio::ssl::detail::engine::want_output_and_retry) {
                    continue;
                }

                return bytes_transferred;
            }
            default:
                LOGV2_FATAL(23283,
                            "Unexpected ASIO state: {state}",
                            "Unexpected ASIO state",
                            "state"_attr = static_cast<int>(want));
                MONGO_UNREACHABLE;
        }
    }
}

int SSLManagerWindows::SSL_shutdown(SSLConnectionInterface* conn) {
    MONGO_UNREACHABLE;
    return 0;
}

// Decode a base-64 PEM blob with headers into a binary blob
StatusWith<std::vector<BYTE>> decodePEMBlob(StringData blob) {
    DWORD decodeLen{0};

    if (!CryptStringToBinaryA(
            blob.rawData(), blob.size(), CRYPT_STRING_BASE64HEADER, NULL, &decodeLen, NULL, NULL)) {
        auto ec = lastSystemError();
        if (ec != systemError(ERROR_MORE_DATA)) {
            return Status(ErrorCodes::InvalidSSLConfiguration,
                          str::stream() << "CryptStringToBinary failed to get size of key: "
                                        << errorMessage(ec));
        }
    }

    std::vector<BYTE> binaryBlobBuf;
    binaryBlobBuf.resize(decodeLen);

    if (!CryptStringToBinaryA(blob.rawData(),
                              blob.size(),
                              CRYPT_STRING_BASE64HEADER,
                              binaryBlobBuf.data(),
                              &decodeLen,
                              NULL,
                              NULL)) {
        auto ec = lastSystemError();
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream()
                          << "CryptStringToBinary failed to read key: " << errorMessage(ec));
    }

    return std::move(binaryBlobBuf);
}

StatusWith<std::vector<BYTE>> decodeObject(const char* structType,
                                           const BYTE* data,
                                           size_t length) {
    DWORD decodeLen{0};

    if (!CryptDecodeObjectEx(
            X509_ASN_ENCODING, structType, data, length, 0, NULL, NULL, &decodeLen)) {
        auto ec = lastSystemError();
        if (ec != systemError(ERROR_MORE_DATA)) {
            return Status(ErrorCodes::InvalidSSLConfiguration,
                          str::stream() << "CryptDecodeObjectEx failed to get size of object: "
                                        << errorMessage(ec));
        }
    }

    std::vector<BYTE> binaryBlobBuf;
    binaryBlobBuf.resize(decodeLen);

    if (!CryptDecodeObjectEx(X509_ASN_ENCODING,
                             structType,
                             data,
                             length,
                             0,
                             NULL,
                             binaryBlobBuf.data(),
                             &decodeLen)) {
        auto ec = lastSystemError();
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream()
                          << "CryptDecodeObjectEx failed to read object: " << errorMessage(ec));
    }

    return std::move(binaryBlobBuf);
}

StatusWith<std::vector<UniqueCertificate>> readCAPEMBuffer(StringData buffer) {
    std::vector<UniqueCertificate> certs;

    // Search the buffer for the various strings that make up a PEM file
    size_t pos = 0;
    bool found_one = false;

    while (pos < buffer.size()) {
        auto swBlob = ssl_util::findPEMBlob(buffer, "CERTIFICATE"_sd, pos, pos != 0);

        // We expect to find at least one certificate
        if (!swBlob.isOK()) {
            if (found_one) {
                return Status::OK();
            }

            return swBlob.getStatus();
        }

        found_one = true;

        auto blobBuf = swBlob.getValue();

        if (blobBuf.empty()) {
            return {std::move(certs)};
        }

        pos = (blobBuf.rawData() + blobBuf.size()) - buffer.rawData();

        auto swCert = decodePEMBlob(blobBuf);
        if (!swCert.isOK()) {
            return swCert.getStatus();
        }

        auto certBuf = swCert.getValue();

        PCCERT_CONTEXT cert =
            CertCreateCertificateContext(X509_ASN_ENCODING, certBuf.data(), certBuf.size());
        if (!cert) {
            auto ec = lastSystemError();
            return Status(ErrorCodes::InvalidSSLConfiguration,
                          str::stream() << "CertCreateCertificateContext failed to decode cert: "
                                        << errorMessage(ec));
        }

        certs.emplace_back(cert);
    }

    return {std::move(certs)};
}

Status addCertificatesToStore(HCERTSTORE certStore, std::vector<UniqueCertificate>& certificates) {
    for (auto& cert : certificates) {
        if (!CertAddCertificateContextToStore(certStore, cert.get(), CERT_STORE_ADD_NEW, NULL)) {
            auto ec = lastSystemError();
            return Status(ErrorCodes::InvalidSSLConfiguration,
                          str::stream()
                              << "CertAddCertificateContextToStore Failed  " << errorMessage(ec));
        }
    }

    return Status::OK();
}

// Read a Certificate PEM file with a private key from disk
StatusWith<UniqueCertificateWithPrivateKey> readCertPEMFile(StringData fileName,
                                                            StringData password) {
    auto swBuf = ssl_util::readPEMFile(fileName);
    if (!swBuf.isOK()) {
        return swBuf.getStatus();
    }

    std::string buf = std::move(swBuf.getValue());

    size_t encryptedPrivateKey = buf.find("-----BEGIN ENCRYPTED PRIVATE KEY-----");
    if (encryptedPrivateKey != std::string::npos) {
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "Encrypted private keys are not supported, use the Windows "
                                       "certificate store instead: "
                                    << fileName);
    }

    // Search the buffer for the various strings that make up a PEM file
    auto swPublicKeyBlob = ssl_util::findPEMBlob(buf, "CERTIFICATE"_sd);
    if (!swPublicKeyBlob.isOK()) {
        return swPublicKeyBlob.getStatus();
    }

    auto publicKeyBlob = swPublicKeyBlob.getValue();

    // Multiple certificates in a PEM file are not supported since these certs need to be in the ca
    // file.
    auto secondPublicKeyBlobPosition =
        buf.find("CERTIFICATE", (publicKeyBlob.rawData() + publicKeyBlob.size()) - buf.data());
    std::vector<UniqueCertificate> extraCertificates;
    if (secondPublicKeyBlobPosition != std::string::npos) {
        // Read in extra certificates
        StringData extraCertificatesBuffer =
            StringData(buf).substr(secondPublicKeyBlobPosition - ("-----BEGIN "_sd).size());

        auto swExtraCertificates = readCAPEMBuffer(extraCertificatesBuffer);
        if (!swExtraCertificates.isOK()) {
            return swExtraCertificates.getStatus();
        }

        extraCertificates = std::move(swExtraCertificates.getValue());
    }

    auto swCert = decodePEMBlob(publicKeyBlob);
    if (!swCert.isOK()) {
        return swCert.getStatus();
    }

    auto certBuf = swCert.getValue();

    PCCERT_CONTEXT cert =
        CertCreateCertificateContext(X509_ASN_ENCODING, certBuf.data(), certBuf.size());

    if (!cert) {
        auto ec = lastSystemError();
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "CertCreateCertificateContext failed to decode cert: "
                                    << errorMessage(ec));
    }

    UniqueCertificate tempCertHolder(cert);

    HCERTSTORE store = CertOpenStore(
        CERT_STORE_PROV_MEMORY, 0, NULL, CERT_STORE_DEFER_CLOSE_UNTIL_LAST_FREE_FLAG, NULL);
    if (!store) {
        auto ec = lastSystemError();
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream()
                          << "CertOpenStore failed to create memory store: " << errorMessage(ec));
    }

    UniqueCertStore storeHolder(store);

    // Add the newly created certificate to the memory store, this makes a copy
    if (!CertAddCertificateContextToStore(store, cert, CERT_STORE_ADD_NEW, NULL)) {
        auto ec = lastSystemError();
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "CertAddCertificateContextToStore Memory Failed  "
                                    << errorMessage(ec));
    }

    // Get the certificate from the store so we attach the private key to the cert in the store
    cert = CertEnumCertificatesInStore(store, NULL);

    UniqueCertificate certHolder(cert);

    std::vector<uint8_t> privateKey;

    // PEM files can have either private key format
    // Also the private key can either come before or after the certificate
    auto swPrivateKeyBlob = ssl_util::findPEMBlob(buf, "RSA PRIVATE KEY"_sd);
    // We expect to find at least one certificate
    if (!swPrivateKeyBlob.isOK()) {
        // A "PRIVATE KEY" is actually a PKCS #8 PrivateKeyInfo ASN.1 type.
        swPrivateKeyBlob = ssl_util::findPEMBlob(buf, "PRIVATE KEY"_sd);
        if (!swPrivateKeyBlob.isOK()) {
            return swPrivateKeyBlob.getStatus();
        }

        auto privateKeyBlob = swPrivateKeyBlob.getValue();

        auto swPrivateKeyBuf = decodePEMBlob(privateKeyBlob);
        if (!swPrivateKeyBuf.isOK()) {
            return swPrivateKeyBuf.getStatus();
        }

        auto privateKeyBuf = swPrivateKeyBuf.getValue();

        auto swPrivateKey =
            decodeObject(PKCS_PRIVATE_KEY_INFO, privateKeyBuf.data(), privateKeyBuf.size());
        if (!swPrivateKey.isOK()) {
            return swPrivateKey.getStatus();
        }

        CRYPT_PRIVATE_KEY_INFO* privateKeyInfo =
            reinterpret_cast<CRYPT_PRIVATE_KEY_INFO*>(swPrivateKey.getValue().data());

        if (strcmp(privateKeyInfo->Algorithm.pszObjId, szOID_RSA_RSA) != 0) {
            return Status(ErrorCodes::InvalidSSLConfiguration,
                          str::stream() << "Non-RSA private keys are not supported, use the "
                                           "Windows certificate store instead");
        }

        auto swPrivateKey2 = decodeObject(PKCS_RSA_PRIVATE_KEY,
                                          privateKeyInfo->PrivateKey.pbData,
                                          privateKeyInfo->PrivateKey.cbData);
        if (!swPrivateKey2.isOK()) {
            return swPrivateKey2.getStatus();
        }

        privateKey = swPrivateKey2.getValue();
    } else {
        auto privateKeyBlob = swPrivateKeyBlob.getValue();

        auto swPrivateKeyBuf = decodePEMBlob(privateKeyBlob);
        if (!swPrivateKeyBuf.isOK()) {
            return swPrivateKeyBuf.getStatus();
        }

        auto privateKeyBuf = swPrivateKeyBuf.getValue();

        auto swPrivateKey =
            decodeObject(PKCS_RSA_PRIVATE_KEY, privateKeyBuf.data(), privateKeyBuf.size());
        if (!swPrivateKey.isOK()) {
            return swPrivateKey.getStatus();
        }

        privateKey = swPrivateKey.getValue();
    }


    HCRYPTPROV hProv;
    std::wstring wstr;

    // Create the right Crypto context depending on whether we running in a server or outside.
    // See https://msdn.microsoft.com/en-us/library/windows/desktop/aa375195(v=vs.85).aspx
    if (isSSLServer) {
        // Generate a unique name for each key container
        // Use the the log file if possible
        if (!serverGlobalParams.logpath.empty()) {
            static AtomicWord<int> counter{0};
            std::string keyContainerName = str::stream()
                << serverGlobalParams.logpath << counter.fetchAndAdd(1);
            wstr = toNativeString(keyContainerName.c_str());
        } else {
            auto us = UUID::gen().toString();
            wstr = toNativeString(us.c_str());
        }

        // Use a new key container for the key. We cannot use the default container since the
        // default
        // container is shared across processes owned by the same user.
        // Note: Server side Schannel requires CRYPT_VERIFYCONTEXT off
        if (!CryptAcquireContextW(&hProv,
                                  wstr.c_str(),
                                  MS_ENHANCED_PROV,
                                  PROV_RSA_FULL,
                                  CRYPT_NEWKEYSET | CRYPT_SILENT)) {
            auto ec = lastSystemError();
            if (ec == systemError(NTE_EXISTS)) {
                if (!CryptAcquireContextW(
                        &hProv, wstr.c_str(), MS_ENHANCED_PROV, PROV_RSA_FULL, CRYPT_SILENT)) {
                    auto ec = lastSystemError();
                    return Status(ErrorCodes::InvalidSSLConfiguration,
                                  str::stream()
                                      << "CryptAcquireContextW failed " << errorMessage(ec));
                }

            } else {
                return Status(ErrorCodes::InvalidSSLConfiguration,
                              str::stream() << "CryptAcquireContextW failed " << errorMessage(ec));
            }
        }
    } else {
        // Use a transient key container for the key
        if (!CryptAcquireContextW(&hProv,
                                  NULL,
                                  MS_ENHANCED_PROV,
                                  PROV_RSA_FULL,
                                  CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
            auto ec = lastSystemError();
            return Status(ErrorCodes::InvalidSSLConfiguration,
                          str::stream() << "CryptAcquireContextW failed  " << errorMessage(ec));
        }
    }
    UniqueCryptProvider cryptProvider(hProv);

    HCRYPTKEY hkey;
    if (!CryptImportKey(hProv, privateKey.data(), privateKey.size(), 0, 0, &hkey)) {
        auto ec = lastSystemError();
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "CryptImportKey failed  " << errorMessage(ec));
    }
    UniqueCryptKey keyHolder(hkey);

    if (isSSLServer) {
        // Server-side SChannel requires a different way of attaching the private key to the
        // certificate
        CRYPT_KEY_PROV_INFO keyProvInfo;
        memset(&keyProvInfo, 0, sizeof(keyProvInfo));
        keyProvInfo.pwszContainerName = const_cast<wchar_t*>(wstr.c_str());
        keyProvInfo.pwszProvName = const_cast<wchar_t*>(MS_ENHANCED_PROV);
        keyProvInfo.dwFlags = CERT_SET_KEY_PROV_HANDLE_PROP_ID | CERT_SET_KEY_CONTEXT_PROP_ID;
        keyProvInfo.dwProvType = PROV_RSA_FULL;
        keyProvInfo.dwKeySpec = AT_KEYEXCHANGE;

        if (!CertSetCertificateContextProperty(
                certHolder.get(), CERT_KEY_PROV_INFO_PROP_ID, 0, &keyProvInfo)) {
            auto ec = lastSystemError();
            return Status(ErrorCodes::InvalidSSLConfiguration,
                          str::stream()
                              << "CertSetCertificateContextProperty Failed  " << errorMessage(ec));
        }
    }

    // NOTE: This is used to set the certificate for client side SChannel
    if (!CertSetCertificateContextProperty(
            cert, CERT_KEY_PROV_HANDLE_PROP_ID, 0, (const void*)hProv)) {
        auto ec = lastSystemError();
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream()
                          << "CertSetCertificateContextProperty failed  " << errorMessage(ec));
    }

    // Add the extra certificates into the same certificate store as the certificate
    addCertificatesToStore(certHolder->hCertStore, extraCertificates);

    return UniqueCertificateWithPrivateKey{std::move(certHolder), std::move(cryptProvider)};
}

Status readCAPEMFile(HCERTSTORE certStore, StringData fileName) {

    auto swBuf = ssl_util::readPEMFile(fileName);
    if (!swBuf.isOK()) {
        return swBuf.getStatus();
    }

    std::string buf = std::move(swBuf.getValue());


    auto swCerts = readCAPEMBuffer(buf);
    if (!swCerts.isOK()) {
        return swCerts.getStatus();
    }

    auto certs = std::move(swCerts.getValue());

    return addCertificatesToStore(certStore, certs);
}

Status readCRLPEMFile(HCERTSTORE certStore, StringData fileName) {

    auto swBuf = ssl_util::readPEMFile(fileName);
    if (!swBuf.isOK()) {
        return swBuf.getStatus();
    }

    std::string buf = std::move(swBuf.getValue());

    // Search the buffer for the various strings that make up a PEM file
    size_t pos = 0;
    bool found_one = false;

    while (pos < buf.size()) {
        auto swBlob = ssl_util::findPEMBlob(buf, "X509 CRL"_sd, pos, pos != 0);

        // We expect to find at least one CRL
        if (!swBlob.isOK()) {
            if (found_one) {
                return Status::OK();
            }

            return swBlob.getStatus();
        }

        found_one = true;

        auto blobBuf = swBlob.getValue();

        if (blobBuf.empty()) {
            return Status::OK();
        }

        pos = (blobBuf.rawData() + blobBuf.size()) - buf.data();

        auto swCert = decodePEMBlob(buf);
        if (!swCert.isOK()) {
            return swCert.getStatus();
        }

        auto certBuf = swCert.getValue();

        PCCRL_CONTEXT crl = CertCreateCRLContext(X509_ASN_ENCODING, certBuf.data(), certBuf.size());
        if (!crl) {
            auto ec = lastSystemError();
            return Status(ErrorCodes::InvalidSSLConfiguration,
                          str::stream()
                              << "CertCreateCRLContext failed to decode crl: " << errorMessage(ec));
        }

        UniqueCRL crlHolder(crl);

        if (!CertAddCRLContextToStore(certStore, crl, CERT_STORE_ADD_NEW, NULL)) {
            auto ec = lastSystemError();
            return Status(ErrorCodes::InvalidSSLConfiguration,
                          str::stream() << "CertAddCRLContextToStore Failed  " << errorMessage(ec));
        }
    }

    return Status::OK();
}

StatusWith<UniqueCertStore> readCertChains(StringData caFile, StringData crlFile) {
    UniqueCertStore certStore = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, NULL, 0, NULL);
    if (!certStore) {
        auto ec = lastSystemError();
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "CertOpenStore Failed  " << errorMessage(ec));
    }

    auto status = readCAPEMFile(certStore, caFile);
    if (!status.isOK()) {
        return status;
    }

    if (!crlFile.empty()) {
        auto status = readCRLPEMFile(certStore, crlFile);
        if (!status.isOK()) {
            return status;
        }
    }

    return std::move(certStore);
}

bool hasCertificateSelector(SSLParams::CertificateSelector selector) {
    return !selector.subject.empty() || !selector.thumbprint.empty();
}

StatusWith<UniqueCertificate> loadCertificateSelectorFromStore(
    SSLParams::CertificateSelector selector, DWORD storeType, StringData storeName) {

    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM,
                                     0,
                                     NULL,
                                     storeType | CERT_STORE_DEFER_CLOSE_UNTIL_LAST_FREE_FLAG |
                                         CERT_STORE_READONLY_FLAG,
                                     L"My");
    if (!store) {
        auto ec = lastSystemError();
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "CertOpenStore failed to open store 'My' from '" << storeName
                                    << "': " << errorMessage(ec));
    }

    UniqueCertStore storeHolder(store);

    if (!selector.subject.empty()) {
        std::wstring wstr = toNativeString(selector.subject.c_str());

        PCCERT_CONTEXT cert = CertFindCertificateInStore(storeHolder,
                                                         X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                                                         0,
                                                         CERT_FIND_SUBJECT_STR,
                                                         wstr.c_str(),
                                                         NULL);
        if (!cert) {
            auto ec = lastSystemError();
            return Status(
                ErrorCodes::InvalidSSLConfiguration,
                str::stream()
                    << "CertFindCertificateInStore failed to find cert with subject name '"
                    << selector.subject.c_str() << "' in 'My' store in '" << storeName
                    << "': " << errorMessage(ec));
        }

        return UniqueCertificate(cert);
    } else {
        CRYPT_HASH_BLOB hashBlob = {static_cast<DWORD>(selector.thumbprint.size()),
                                    selector.thumbprint.data()};

        PCCERT_CONTEXT cert = CertFindCertificateInStore(storeHolder,
                                                         X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                                                         0,
                                                         CERT_FIND_HASH,
                                                         &hashBlob,
                                                         NULL);
        if (!cert) {
            auto ec = lastSystemError();
            return Status(ErrorCodes::InvalidSSLConfiguration,
                          str::stream()
                              << "CertFindCertificateInStore failed to find cert with thumbprint '"
                              << hexblob::encode(selector.thumbprint.data(),
                                                 selector.thumbprint.size())
                              << "' in 'My' store in '" << storeName << "': " << errorMessage(ec));
        }

        return UniqueCertificate(cert);
    }
}

StatusWith<UniqueCertificate> loadCertificateSelectorFromAllStores(
    SSLParams::CertificateSelector selector) {
    auto swSelectMachine = loadCertificateSelectorFromStore(
        selector, CERT_SYSTEM_STORE_LOCAL_MACHINE, "Local Machine");
    if (!swSelectMachine.isOK()) {
        auto swSelectUser = loadCertificateSelectorFromStore(
            selector, CERT_SYSTEM_STORE_CURRENT_USER, "Current User");
        if (swSelectUser.isOK()) {
            return swSelectUser;
        }
    }

    return swSelectMachine;
}

StatusWith<UniqueCertificate> loadAndValidateCertificateSelector(
    SSLParams::CertificateSelector selector) {
    auto swCert = loadCertificateSelectorFromAllStores(selector);
    if (!swCert.isOK()) {
        return swCert;
    }

    // Try to grab the private key from the certificate to verify the certificate has a private key
    // attached to it.
    DWORD dwKeySpec;
    BOOL freeProvider;
    HCRYPTPROV hCryptProv;
    if (!CryptAcquireCertificatePrivateKey(swCert.getValue().get(),
                                           CRYPT_ACQUIRE_ALLOW_NCRYPT_KEY_FLAG,
                                           NULL,
                                           &hCryptProv,
                                           &dwKeySpec,
                                           &freeProvider)) {
        auto ec = lastSystemError();
        if (ec == systemError(CRYPT_E_NO_KEY_PROPERTY)) {
            return Status(ErrorCodes::InvalidSSLConfiguration,
                          str::stream()
                              << "Could not find private key attached to the selected certificate");
        } else if (ec == systemError(NTE_BAD_KEYSET)) {
            return Status(ErrorCodes::InvalidSSLConfiguration,
                          str::stream() << "Could not read private key attached to the selected "
                                           "certificate, ensure it exists and check the private "
                                           "key permissions");
        } else {
            return Status(ErrorCodes::InvalidSSLConfiguration,
                          str::stream()
                              << "CryptAcquireCertificatePrivateKey failed  " << errorMessage(ec));
        }
    }

    if (freeProvider) {
        UniqueCryptProvider prov(hCryptProv);
    }

    return std::move(swCert.getValue());
}

Status SSLManagerWindows::_loadCertificates(const SSLParams& params) {
    _clientCertificates[0] = nullptr;
    _serverCertificates[0] = nullptr;

    // Load the normal PEM file
    if (!params.sslPEMKeyFile.empty()) {
        auto swCertificate = readCertPEMFile(params.sslPEMKeyFile, params.sslPEMKeyPassword);
        if (!swCertificate.isOK()) {
            return swCertificate.getStatus();
        }

        _pemCertificate = std::move(swCertificate.getValue());
    }

    // Load the cluster PEM file, only applies to server side code
    if (!params.sslClusterFile.empty()) {
        auto swCertificate = readCertPEMFile(params.sslClusterFile, params.sslClusterPassword);
        if (!swCertificate.isOK()) {
            return swCertificate.getStatus();
        }

        _clusterPEMCertificate = std::move(swCertificate.getValue());
    }

    if (std::get<0>(_pemCertificate)) {
        _clientCertificates[0] = std::get<0>(_pemCertificate).get();
        _serverCertificates[0] = std::get<0>(_pemCertificate).get();
    }

    if (std::get<0>(_clusterPEMCertificate)) {
        _clientCertificates[0] = std::get<0>(_clusterPEMCertificate).get();
    }

    if (!params.sslCAFile.empty()) {
        // SChannel always has a CA even when the user does not specify one
        // The openssl implementations uses this to decide if it wants to do certificate validation
        // on the server side.
        _sslConfiguration.hasCA = true;

        auto swChain = readCertChains(params.sslCAFile, params.sslCRLFile);
        if (!swChain.isOK()) {
            return swChain.getStatus();
        }

        // Dump the CA cert chain into the memory store for the client cert. This ensures Windows
        // can build a complete chain to send to the remote side.
        if (std::get<0>(_pemCertificate)) {
            auto status =
                readCAPEMFile(std::get<0>(_pemCertificate).get()->hCertStore, params.sslCAFile);
            if (!status.isOK()) {
                return status;
            }
        }

        _clientEngine.CAstore = std::move(swChain.getValue());
    }
    _clientEngine.hasCRL = !params.sslCRLFile.empty();

    const auto serverCAFile =
        params.sslClusterCAFile.empty() ? params.sslCAFile : params.sslClusterCAFile;
    if (!serverCAFile.empty()) {
        auto swChain = readCertChains(serverCAFile, params.sslCRLFile);
        if (!swChain.isOK()) {
            return swChain.getStatus();
        }

        // Dump the CA cert chain into the memory store for the cluster cert. This ensures Windows
        // can build a complete chain to send to the remote side.
        if (std::get<0>(_clusterPEMCertificate)) {
            auto status =
                readCAPEMFile(std::get<0>(_clusterPEMCertificate).get()->hCertStore, serverCAFile);
            if (!status.isOK()) {
                return status;
            }
        }

        _serverEngine.CAstore = std::move(swChain.getValue());
    }
    _serverEngine.hasCRL = !params.sslCRLFile.empty();

    if (hasCertificateSelector(params.sslCertificateSelector)) {
        auto swCert = loadAndValidateCertificateSelector(params.sslCertificateSelector);
        if (!swCert.isOK()) {
            return swCert.getStatus();
        }
        _sslCertificate = std::move(swCert.getValue());
    }

    if (hasCertificateSelector(params.sslClusterCertificateSelector)) {
        auto swCert = loadAndValidateCertificateSelector(params.sslClusterCertificateSelector);
        if (!swCert.isOK()) {
            return swCert.getStatus();
        }
        _sslClusterCertificate = std::move(swCert.getValue());
    }

    if (_sslCertificate || _sslClusterCertificate) {
        if (!params.sslCAFile.empty()) {
            LOGV2_WARNING(23271,
                          "Mixing certs from the system certificate store and PEM files. This may "
                          "produced unexpected results.");
        }

        _sslConfiguration.hasCA = true;
    }

    if (_sslCertificate) {
        _clientCertificates[0] = _sslCertificate.get();
        _serverCertificates[0] = _sslCertificate.get();
    }

    if (_sslClusterCertificate) {
        _clientCertificates[0] = _sslClusterCertificate.get();
    }

    return Status::OK();
}

Status SSLManagerWindows::initSSLContext(SCHANNEL_CRED* cred,
                                         const SSLParams& params,
                                         ConnectionDirection direction) {

    memset(cred, 0, sizeof(*cred));
    cred->dwVersion = SCHANNEL_CRED_VERSION;
    cred->dwFlags = SCH_USE_STRONG_CRYPTO;  // Use strong crypto;


    uint32_t supportedProtocols = 0;

    if (direction == ConnectionDirection::kIncoming) {
        supportedProtocols = SP_PROT_TLS1_SERVER | SP_PROT_TLS1_0_SERVER | SP_PROT_TLS1_1_SERVER |
            SP_PROT_TLS1_2_SERVER;

        cred->hRootStore = _serverEngine.CAstore;
        cred->dwFlags = cred->dwFlags          // flags
            | SCH_CRED_REVOCATION_CHECK_CHAIN  // Check certificate revocation
            | SCH_CRED_SNI_CREDENTIAL          // Pass along SNI creds
            | SCH_CRED_MEMORY_STORE_CERT       // Read intermediate certificates from memory
                                               // store associated with client certificate.
            | SCH_CRED_NO_SYSTEM_MAPPER        // Do not map certificate to user account
            | SCH_CRED_DISABLE_RECONNECTS;     // Do not support reconnects

    } else {
        supportedProtocols = SP_PROT_TLS1_CLIENT | SP_PROT_TLS1_0_CLIENT | SP_PROT_TLS1_1_CLIENT |
            SP_PROT_TLS1_2_CLIENT;

        cred->hRootStore = _clientEngine.CAstore;
        cred->dwFlags = cred->dwFlags           // Flags
            | SCH_CRED_REVOCATION_CHECK_CHAIN   // Check certificate revocation
            | SCH_CRED_NO_SERVERNAME_CHECK      // Do not validate server name against cert
            | SCH_CRED_NO_DEFAULT_CREDS         // No Default Certificate
            | SCH_CRED_MEMORY_STORE_CERT        // Read intermediate certificates from memory
                                                // store associated with client certificate.
            | SCH_CRED_MANUAL_CRED_VALIDATION;  // Validate Certificate Manually
    }

    // Set the supported TLS protocols. Allow --sslDisabledProtocols to disable selected ciphers.
    for (const SSLParams::Protocols& protocol : params.sslDisabledProtocols) {
        if (protocol == SSLParams::Protocols::TLS1_0) {
            supportedProtocols &= ~(SP_PROT_TLS1_0_CLIENT | SP_PROT_TLS1_0_SERVER);
        } else if (protocol == SSLParams::Protocols::TLS1_1) {
            supportedProtocols &= ~(SP_PROT_TLS1_1_CLIENT | SP_PROT_TLS1_1_SERVER);
        } else if (protocol == SSLParams::Protocols::TLS1_2) {
            supportedProtocols &= ~(SP_PROT_TLS1_2_CLIENT | SP_PROT_TLS1_2_SERVER);
        }
    }

    cred->grbitEnabledProtocols = supportedProtocols;
    if (supportedProtocols == 0) {
        return {ErrorCodes::InvalidSSLConfiguration,
                "All supported TLS protocols have been disabled."};
    }

    if (params.sslCipherConfig != kSSLCipherConfigDefault) {
        LOGV2_WARNING(
            23272,
            "sslCipherConfig parameter is not supported with Windows SChannel and is ignored.");
    }

    if (direction == ConnectionDirection::kOutgoing) {
        if (_clientCertificates[0] && !params.tlsWithholdClientCertificate) {
            cred->cCreds = 1;
            cred->paCred = _clientCertificates.data();
        }
    } else {
        cred->cCreds = 1;
        cred->paCred = _serverCertificates.data();
    }

    return Status::OK();
}

void SSLManagerWindows::registerOwnedBySSLContext(
    std::weak_ptr<const SSLConnectionContext> ownedByContext) {
    _ownedByContext = ownedByContext;
}

SSLConnectionInterface* SSLManagerWindows::connect(Socket* socket) {
    std::unique_ptr<SSLConnectionWindows> sslConn =
        std::make_unique<SSLConnectionWindows>(&_clientCred, socket, nullptr, 0);

    _handshake(sslConn.get(), true);
    return sslConn.release();
}

SSLConnectionInterface* SSLManagerWindows::accept(Socket* socket,
                                                  const char* initialBytes,
                                                  int len) {
    std::unique_ptr<SSLConnectionWindows> sslConn =
        std::make_unique<SSLConnectionWindows>(&_serverCred, socket, initialBytes, len);

    _handshake(sslConn.get(), false);

    return sslConn.release();
}

void SSLManagerWindows::_handshake(SSLConnectionWindows* conn, bool client) {
    initSSLContext(conn->_cred,
                   getSSLGlobalParams(),
                   client ? SSLManagerInterface::ConnectionDirection::kOutgoing
                          : SSLManagerInterface::ConnectionDirection::kIncoming);

    while (true) {
        asio::error_code ec;
        asio::ssl::detail::engine::want want =
            conn->_engine.handshake(client ? asio::ssl::stream_base::handshake_type::client
                                           : asio::ssl::stream_base::handshake_type::server,
                                    ec);
        if (ec) {
            throwSocketError(SocketErrorKind::RECV_ERROR, ec.message());
        }

        switch (want) {
            case asio::ssl::detail::engine::want_input_and_retry: {
                // ASIO wants more data before it can continue,
                // 1. fetch some from the network
                // 2. give it to ASIO
                // 3. retry
                int ret = recv(conn->socket->rawFD(),
                               conn->_tempBuffer.data(),
                               conn->_tempBuffer.size(),
                               portRecvFlags);
                if (ret == SOCKET_ERROR) {
                    conn->socket->handleRecvError(ret, conn->_tempBuffer.size());
                }

                conn->_engine.put_input(asio::const_buffer(conn->_tempBuffer.data(), ret));

                continue;
            }
            case asio::ssl::detail::engine::want_output:
            case asio::ssl::detail::engine::want_output_and_retry: {
                // ASIO wants us to send data out
                // 1. get data from ASIO
                // 2. give it to the network
                // 3. retry if needed
                asio::mutable_buffer outBuf = conn->_engine.get_output(
                    asio::mutable_buffer(conn->_tempBuffer.data(), conn->_tempBuffer.size()));

                int ret = send(conn->socket->rawFD(),
                               reinterpret_cast<const char*>(outBuf.data()),
                               outBuf.size(),
                               portSendFlags);
                if (ret == SOCKET_ERROR) {
                    conn->socket->handleSendError(ret, "");
                }

                if (want == asio::ssl::detail::engine::want_output_and_retry) {
                    continue;
                }

                // ASIO wants nothing, return to caller since we are done with handshake
                return;
            }
            case asio::ssl::detail::engine::want_nothing: {
                // ASIO wants nothing, return to caller since we are done with handshake
                return;
            }
            default:
                MONGO_UNREACHABLE;
        }
    }
}

unsigned long long FiletimeToULL(FILETIME ft) {
    return *reinterpret_cast<unsigned long long*>(&ft);
}


unsigned long long FiletimeToEpocMillis(FILETIME ft) {
    constexpr auto kOneHundredNanosecondsSinceEpoch = 116444736000000000LL;

    uint64_t ns100 = ((static_cast<int64_t>(ft.dwHighDateTime) << 32) + ft.dwLowDateTime) -
        kOneHundredNanosecondsSinceEpoch;
    return ns100 / 10000;
}

StatusWith<SSLX509Name> blobToName(CERT_NAME_BLOB blob) {
    auto swBlob = decodeObject(X509_NAME, blob.pbData, blob.cbData);

    if (!swBlob.isOK()) {
        return swBlob.getStatus();
    }

    PCERT_NAME_INFO nameInfo = reinterpret_cast<PCERT_NAME_INFO>(swBlob.getValue().data());

    if (!nameInfo->cRDN) {
        return SSLX509Name();
    }

    std::vector<std::vector<SSLX509Name::Entry>> entries;

    // Iterate in reverse order
    for (int64_t i = nameInfo->cRDN - 1; i >= 0; i--) {
        std::vector<SSLX509Name::Entry> rdn;
        for (DWORD j = nameInfo->rgRDN[i].cRDNAttr; j > 0; --j) {
            CERT_RDN_ATTR& rdnAttribute = nameInfo->rgRDN[i].rgRDNAttr[j - 1];

            DWORD needed =
                CertRDNValueToStrW(rdnAttribute.dwValueType, &rdnAttribute.Value, NULL, 0);

            std::wstring wstr;
            wstr.resize(needed - 1);
            DWORD converted = CertRDNValueToStrW(rdnAttribute.dwValueType,
                                                 &rdnAttribute.Value,
                                                 const_cast<wchar_t*>(wstr.data()),
                                                 needed);
            invariant(needed == converted);

            // The value of rdnAttribute.dwValueType is not actually the asn1 type id, it's
            // a Microsoft-specific value. We convert the types for a valid directory string
            // here so other non-windows parts of the SSL stack can safely compare SSLX509Name's
            // later.
            int asn1Type = rdnAttribute.dwValueType & CERT_RDN_TYPE_MASK;
            switch (asn1Type) {
                case CERT_RDN_UTF8_STRING:
                case CERT_RDN_UNICODE_STRING:  // This is the same value as CERT_RDN_BMP_STRING
                    asn1Type = kASN1UTF8String;
                    break;
                case CERT_RDN_PRINTABLE_STRING:
                    asn1Type = kASN1PrintableString;
                    break;
                case CERT_RDN_TELETEX_STRING:
                    asn1Type = kASN1TeletexString;
                    break;
                case CERT_RDN_UNIVERSAL_STRING:
                    asn1Type = kASN1UniversalString;
                    break;
                case CERT_RDN_OCTET_STRING:
                    asn1Type = kASN1OctetString;
                    break;
                case CERT_RDN_IA5_STRING:
                    asn1Type = kASN1IA5String;
                    break;
            }

            rdn.emplace_back(rdnAttribute.pszObjId, asn1Type, toUtf8String(wstr));
        }
        entries.push_back(std::move(rdn));
    }

    return SSLX509Name(std::move(entries));
}

// MongoDB wants RFC 2253 (LDAP) formatted DN names for auth purposes
StatusWith<SSLX509Name> getCertificateSubjectName(PCCERT_CONTEXT cert) {
    return blobToName(cert->pCertInfo->Subject);
}

Status SSLManagerWindows::_validateCertificate(PCCERT_CONTEXT cert,
                                               SSLX509Name* subjectName,
                                               Date_t* serverCertificateExpirationDate) {

    auto swCert = getCertificateSubjectName(cert);

    if (!swCert.isOK()) {
        return swCert.getStatus();
    }

    *subjectName = swCert.getValue();

    if (serverCertificateExpirationDate != nullptr) {
        FILETIME currentTime;
        GetSystemTimeAsFileTime(&currentTime);
        unsigned long long currentTimeLong = FiletimeToULL(currentTime);

        if ((FiletimeToULL(cert->pCertInfo->NotBefore) > currentTimeLong) ||
            (currentTimeLong > FiletimeToULL(cert->pCertInfo->NotAfter))) {
            LOGV2_FATAL_NOTRACE(50755, "The provided SSL certificate is expired or not yet valid.");
        }

        *serverCertificateExpirationDate =
            Date_t::fromMillisSinceEpoch(FiletimeToEpocMillis(cert->pCertInfo->NotAfter));
    }

    uassertStatusOK(subjectName->normalizeStrings());
    return Status::OK();
}

SSLPeerInfo SSLManagerWindows::parseAndValidatePeerCertificateDeprecated(
    const SSLConnectionInterface* conn,
    const std::string& remoteHost,
    const HostAndPort& hostForLogging) {

    auto swPeerSubjectName =
        parseAndValidatePeerCertificate(
            const_cast<SSLConnectionWindows*>(static_cast<const SSLConnectionWindows*>(conn))
                ->_engine.native_handle(),
            boost::none,
            remoteHost,
            hostForLogging,
            nullptr)
            .getNoThrow();
    // We can't use uassertStatusOK here because we need to throw a SocketException.
    if (!swPeerSubjectName.isOK()) {
        throwSocketError(SocketErrorKind::CONNECT_ERROR, swPeerSubjectName.getStatus().reason());
    }
    return swPeerSubjectName.getValue();
}

// Get a list of subject alternative names to assist the user in diagnosing certificate verification
// errors.
StatusWith<std::vector<std::string>> getSubjectAlternativeNames(PCCERT_CONTEXT cert) {

    std::vector<std::string> names;
    PCERT_EXTENSION extension = CertFindExtension(
        szOID_SUBJECT_ALT_NAME2, cert->pCertInfo->cExtension, cert->pCertInfo->rgExtension);

    if (extension == nullptr) {
        return names;
    }

    auto swBlob =
        decodeObject(szOID_SUBJECT_ALT_NAME2, extension->Value.pbData, extension->Value.cbData);

    if (!swBlob.isOK()) {
        return swBlob.getStatus();
    }

    CERT_ALT_NAME_INFO* altNames = reinterpret_cast<CERT_ALT_NAME_INFO*>(swBlob.getValue().data());
    for (size_t i = 0; i < altNames->cAltEntry; i++) {
        if (altNames->rgAltEntry[i].dwAltNameChoice == CERT_ALT_NAME_DNS_NAME) {
            auto san = toUtf8String(altNames->rgAltEntry[i].pwszDNSName);
            names.push_back(san);
            auto swCIDRSan = CIDR::parse(san);
            if (swCIDRSan.isOK()) {
                LOGV2_WARNING(23273,
                              "You have an IP Address in the DNS Name field on your "
                              "certificate. This formulation is depreceated.");
            }
        } else if (altNames->rgAltEntry[i].dwAltNameChoice == CERT_ALT_NAME_IP_ADDRESS) {
            auto ipAddrStruct = altNames->rgAltEntry[i].IPAddress;
            struct sockaddr_storage ss;
            memset(&ss, 0, sizeof(ss));
            if (ipAddrStruct.cbData == 4) {
                struct sockaddr_in* sa = reinterpret_cast<struct sockaddr_in*>(&ss);
                sa->sin_family = AF_INET;
                memcpy(&(sa->sin_addr), ipAddrStruct.pbData, ipAddrStruct.cbData);
            } else if (ipAddrStruct.cbData == 16) {
                struct sockaddr_in6* sa = reinterpret_cast<struct sockaddr_in6*>(&ss);
                sa->sin6_family = AF_INET6;
                memcpy(&(sa->sin6_addr), ipAddrStruct.pbData, ipAddrStruct.cbData);
            }
            names.push_back(
                SockAddr(reinterpret_cast<sockaddr*>(&ss), sizeof(struct sockaddr_storage))
                    .getAddr());
        }
    }
    return names;
}

Status validatePeerCertificate(const std::string& remoteHost,
                               PCCERT_CONTEXT cert,
                               HCERTCHAINENGINE certChainEngine,
                               bool allowInvalidCertificates,
                               bool allowInvalidHostnames,
                               bool hasCRL,
                               SSLX509Name* peerSubjectName) {
    CERT_CHAIN_PARA certChainPara;
    memset(&certChainPara, 0, sizeof(certChainPara));
    certChainPara.cbSize = sizeof(CERT_CHAIN_PARA);

    // szOID_PKIX_KP_SERVER_AUTH ("1.3.6.1.5.5.7.3.1") - means the certificate can be used for
    // server authentication
    LPSTR usage[] = {
        const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH),
    };

    // If remoteHost is not empty, then this is running on the client side, and we want to verify
    // the server cert.
    if (!remoteHost.empty()) {
        certChainPara.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
        certChainPara.RequestedUsage.Usage.cUsageIdentifier = _countof(usage);
        certChainPara.RequestedUsage.Usage.rgpszUsageIdentifier = usage;
    }

    certChainPara.dwUrlRetrievalTimeout = gTLSOCSPVerifyTimeoutSecs * 1000;

    auto before = Date_t::now();
    PCCERT_CHAIN_CONTEXT chainContext;
    if (!CertGetCertificateChain(certChainEngine,
                                 cert,
                                 NULL,
                                 cert->hCertStore,
                                 &certChainPara,
                                 CERT_CHAIN_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT,
                                 NULL,
                                 &chainContext)) {
        auto ec = lastSystemError();
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "CertGetCertificateChain failed: " << errorMessage(ec));
    }

    auto after = Date_t::now();
    auto elapsed = after - before;
    if (elapsed > Seconds(gTLSOCSPSlowResponderWarningSecs)) {
        LOGV2_WARNING(4780400, "OCSP responder was slow to respond", "duration"_attr = elapsed);
    }

    UniqueCertChain certChainHolder(chainContext);

    SSL_EXTRA_CERT_CHAIN_POLICY_PARA sslCertChainPolicy;
    memset(&sslCertChainPolicy, 0, sizeof(sslCertChainPolicy));
    sslCertChainPolicy.cbSize = sizeof(sslCertChainPolicy);

    std::wstring serverName;

    // If remoteHost is empty, then this is running on the server side, and we want to verify the
    // client cert
    if (remoteHost.empty()) {
        sslCertChainPolicy.dwAuthType = AUTHTYPE_CLIENT;
    } else {
        serverName = toNativeString(removeFQDNRoot(remoteHost).c_str());
        sslCertChainPolicy.pwszServerName = const_cast<wchar_t*>(serverName.c_str());
        sslCertChainPolicy.dwAuthType = AUTHTYPE_SERVER;
    }

    CERT_CHAIN_POLICY_PARA chain_policy_para;
    memset(&chain_policy_para, 0, sizeof(chain_policy_para));
    chain_policy_para.cbSize = sizeof(chain_policy_para);
    chain_policy_para.pvExtraPolicyPara = &sslCertChainPolicy;

    if (!hasCRL) {
        chain_policy_para.dwFlags |= CERT_CHAIN_POLICY_IGNORE_ALL_REV_UNKNOWN_FLAGS;
    }

    CERT_CHAIN_POLICY_STATUS certChainPolicyStatus;
    memset(&certChainPolicyStatus, 0, sizeof(certChainPolicyStatus));
    certChainPolicyStatus.cbSize = sizeof(certChainPolicyStatus);


    // This means something really went wrong, this should not happen.
    if (!CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL,
                                          certChainHolder.get(),
                                          &chain_policy_para,
                                          &certChainPolicyStatus)) {
        auto ec = lastSystemError();
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream()
                          << "CertVerifyCertificateChainPolicy failed: " << errorMessage(ec));
    }

    auto swSubjectName = getCertificateSubjectName(cert);
    if (!swSubjectName.isOK()) {
        return swSubjectName.getStatus();
    }
    invariant(peerSubjectName);
    *peerSubjectName = swSubjectName.getValue();

    if (remoteHost.empty()) {
        const auto exprThreshold = tlsX509ExpirationWarningThresholdDays;
        if (exprThreshold > 0) {
            const auto now = Date_t::now();
            const auto expiration =
                Date_t::fromMillisSinceEpoch(FiletimeToEpocMillis(cert->pCertInfo->NotAfter));

            if ((now + Days(exprThreshold)) > expiration) {
                tlsEmitWarningExpiringClientCertificate(*peerSubjectName,
                                                        duration_cast<Days>(expiration - now));
            }
        }
    }

    // This means the certificate chain is not valid.
    // Ignore CRYPT_E_NO_REVOCATION_CHECK since most CAs lack revocation information especially test
    // certificates. Either there needs to be a CRL, a CrlDistributionPoint in the Cert or OCSP and
    // user-generated certs lack this information.
    if (certChainPolicyStatus.dwError != S_OK &&
        certChainPolicyStatus.dwError != CRYPT_E_NO_REVOCATION_CHECK) {
        bool onlyCNError = false;

        // Try again to validate if the cert has any other errors besides a CN mismatch
        if (certChainPolicyStatus.dwError == CERT_E_CN_NO_MATCH && !allowInvalidCertificates) {

            // We know the CNs do not match, are there any other issues?
            sslCertChainPolicy.fdwChecks = SECURITY_FLAG_IGNORE_CERT_CN_INVALID;


            // This means something really went wrong, this should not happen.
            if (!CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL,
                                                  certChainHolder.get(),
                                                  &chain_policy_para,
                                                  &certChainPolicyStatus)) {
                auto ec = lastSystemError();
                return Status(ErrorCodes::InvalidSSLConfiguration,
                              str::stream() << "CertVerifyCertificateChainPolicy2 failed: "
                                            << errorMessage(ec));
            }

            if (certChainPolicyStatus.dwError == S_OK ||
                certChainPolicyStatus.dwError == CRYPT_E_NO_REVOCATION_CHECK) {
                onlyCNError = true;
            }
        }

        // We need to check if the user has a cert where SANs have ip addresses label as DNS Name
        // but only if a CN mismatch is the only error
        if (onlyCNError || allowInvalidCertificates) {
            auto swAltNames = getSubjectAlternativeNames(cert);
            auto swCIDRRemoteHost = CIDR::parse(remoteHost);
            if (swAltNames.isOK() && swCIDRRemoteHost.isOK()) {
                auto remoteHostCIDR = swCIDRRemoteHost.getValue();
                // Parsing the client's hostname
                for (const auto& name : swAltNames.getValue()) {
                    auto swCIDRHost = CIDR::parse(name);
                    // Checking that the client hostname is an IP address
                    // and it equals a SAN on the server cert
                    if (swCIDRHost.isOK() && remoteHostCIDR == swCIDRHost.getValue()) {
                        return Status::OK();
                    }
                }
            }

            // Give the user a hint why the certificate validation failed.
            StringBuilder certificateNames;
            bool hasSAN = false;
            if (swAltNames.isOK() && !swAltNames.getValue().empty()) {
                hasSAN = true;
                for (auto& name : swAltNames.getValue()) {
                    certificateNames << name << " ";
                }
            };

            certificateNames << ", Subject Name: " << *peerSubjectName;

            auto swCN = peerSubjectName->getOID(kOID_CommonName);
            if (hasSAN && swCN.isOK() &&
                hostNameMatchForX509Certificates(remoteHost, swCN.getValue())) {
                certificateNames << " would have matched, but was overridden by SAN";
            }

            str::stream msg;
            msg << "The server certificate does not match the host name. Hostname: " << remoteHost
                << " does not match " << certificateNames.str();

            if (allowInvalidCertificates) {
                LOGV2_WARNING(23274,
                              "SSL peer certificate validation failed ({errorCode}): {error}",
                              "SSL peer certificate validation failed",
                              "errorCode"_attr = unsignedHex(certChainPolicyStatus.dwError),
                              "error"_attr =
                                  errorMessage(systemError(certChainPolicyStatus.dwError)));

                if (certChainPolicyStatus.dwError == CERT_E_CN_NO_MATCH) {
                    LOGV2_WARNING(23275,
                                  "The server certificate does not match the host name. Hostname: "
                                  "{remoteHost} does not match {certificateNames}",
                                  "The server certificate does not match the host name",
                                  "remoteHost"_attr = remoteHost,
                                  "certificateNames"_attr = certificateNames.str());
                }

                *peerSubjectName = SSLX509Name();
                return Status::OK();
            } else if (allowInvalidHostnames) {
                LOGV2_WARNING(23276,
                              "The server certificate does not match the host name. Hostname: "
                              "{remoteHost} does not match {certificateNames}",
                              "The server certificate does not match the host name",
                              "remoteHost"_attr = remoteHost,
                              "certificateNames"_attr = certificateNames.str());
                return Status::OK();
            } else {
                return Status(ErrorCodes::SSLHandshakeFailed, msg);
            }
        } else {
            str::stream msg;
            msg << "SSL peer certificate validation failed: ("
                << unsignedHex(certChainPolicyStatus.dwError) << ")"
                << errorMessage(systemError(certChainPolicyStatus.dwError));


            LOGV2_ERROR(23279,
                        "SSL peer certificate validation failed: ({errorCode}){error}",
                        "SSL peer certificate validation failed",
                        "errorCode"_attr = unsignedHex(certChainPolicyStatus.dwError),
                        "error"_attr = errorMessage(systemError(certChainPolicyStatus.dwError)));
            return Status(ErrorCodes::SSLHandshakeFailed, msg);
        }
    }

    uassertStatusOK(peerSubjectName->normalizeStrings());

    return Status::OK();
}

StatusWith<TLSVersion> mapTLSVersion(PCtxtHandle ssl) {
    SecPkgContext_ConnectionInfo connInfo;

    SECURITY_STATUS ss = QueryContextAttributes(ssl, SECPKG_ATTR_CONNECTION_INFO, &connInfo);

    if (ss != SEC_E_OK) {
        return Status(ErrorCodes::SSLHandshakeFailed,
                      str::stream()
                          << "QueryContextAttributes for connection info failed with" << ss);
    }

    switch (connInfo.dwProtocol) {
        case SP_PROT_TLS1_CLIENT:
        case SP_PROT_TLS1_SERVER:
            return TLSVersion::kTLS10;
        case SP_PROT_TLS1_1_CLIENT:
        case SP_PROT_TLS1_1_SERVER:
            return TLSVersion::kTLS11;
        case SP_PROT_TLS1_2_CLIENT:
        case SP_PROT_TLS1_2_SERVER:
            return TLSVersion::kTLS12;
        default:
            return TLSVersion::kUnknown;
    }
}

Status SSLManagerWindows::stapleOCSPResponse(SCHANNEL_CRED* cred, bool asyncOCSPStaple) {
    return Status::OK();
}

void SSLManagerWindows::stopJobs() {}

Future<SSLPeerInfo> SSLManagerWindows::parseAndValidatePeerCertificate(
    PCtxtHandle ssl,
    boost::optional<std::string> sni,
    const std::string& remoteHost,
    const HostAndPort& hostForLogging,
    const ExecutorPtr& reactor) {
    invariant(!sslGlobalParams.tlsCATrusts);

    PCCERT_CONTEXT cert;

    auto tlsVersionStatus = mapTLSVersion(ssl);
    if (!tlsVersionStatus.isOK()) {
        return tlsVersionStatus.getStatus();
    }

    recordTLSVersion(tlsVersionStatus.getValue(), hostForLogging);

    if (!_sslConfiguration.hasCA && isSSLServer)
        return Future<SSLPeerInfo>::makeReady(SSLPeerInfo(sni));

    SECURITY_STATUS ss = QueryContextAttributes(ssl, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &cert);

    if (ss == SEC_E_NO_CREDENTIALS) {  // no certificate presented by peer
        if (_weakValidation) {
            // do not give warning if "no certificate" warnings are suppressed
            if (!_suppressNoCertificateWarning) {
                LOGV2_WARNING(23277,
                              "no SSL certificate provided by peer",
                              "No SSL certificate provided by peer");
            }
            return SSLPeerInfo(sni);
        } else {
            auto msg = "no SSL certificate provided by peer; connection rejected";
            LOGV2_ERROR(23280,
                        "no SSL certificate provided by peer; connection rejected",
                        "No SSL certificate provided by peer; connection rejected");
            return Status(ErrorCodes::SSLHandshakeFailed, msg);
        }
    }

    // Check for unexpected errors
    if (ss != SEC_E_OK) {
        return Status(ErrorCodes::SSLHandshakeFailed,
                      str::stream() << "QueryContextAttributes failed with" << ss);
    }

    UniqueCertificate certHolder(cert);
    SSLX509Name peerSubjectName;

    auto* engine = remoteHost.empty() ? &_serverEngine : &_clientEngine;

    // Validate against the local machine store first since it is easier to manage programmatically.
    Status validateCertMachine = validatePeerCertificate(remoteHost,
                                                         certHolder.get(),
                                                         engine->machine,
                                                         _allowInvalidCertificates,
                                                         _allowInvalidHostnames,
                                                         engine->hasCRL,
                                                         &peerSubjectName);
    if (!validateCertMachine.isOK()) {
        // Validate against the current user store since this is easier for unprivileged users to
        // manage.
        Status validateCertUser = validatePeerCertificate(remoteHost,
                                                          certHolder.get(),
                                                          engine->user,
                                                          _allowInvalidCertificates,
                                                          _allowInvalidHostnames,
                                                          engine->hasCRL,
                                                          &peerSubjectName);
        if (!validateCertUser.isOK()) {
            // Return the local machine status
            return validateCertMachine;
        }
    }

    if (peerSubjectName.empty()) {
        return Future<SSLPeerInfo>::makeReady(SSLPeerInfo(sni));
    }

    SecPkgContext_CipherInfo cipherInfo;
    SECURITY_STATUS ssCipher = QueryContextAttributes(ssl, SECPKG_ATTR_CIPHER_INFO, &cipherInfo);
    if (ssCipher != SEC_E_OK) {
        return Status(ErrorCodes::SSLHandshakeFailed,
                      str::stream()
                          << "QueryContextAttributes for connection info failed with" << ssCipher);
    }
    const auto cipher = std::wstring(cipherInfo.szCipherSuite);

    if (!serverGlobalParams.quiet.load() && gEnableDetailedConnectionHealthMetricLogLines) {
        LOGV2_INFO(6723802,
                   "Accepted TLS connection from peer",
                   "peerSubjectName"_attr = peerSubjectName,
                   "cipher"_attr = toUtf8String(cipher));
    }

    // If this is a server and client and server certificate are the same, log a warning.
    if (remoteHost.empty() && _sslConfiguration.serverSubjectName() == peerSubjectName) {
        LOGV2_WARNING(23278, "Client connecting with server's own TLS certificate");
    }

    // On the server side, parse the certificate for roles
    if (remoteHost.empty()) {
        StatusWith<stdx::unordered_set<RoleName>> swPeerCertificateRoles = parsePeerRoles(cert);
        if (!swPeerCertificateRoles.isOK()) {
            return swPeerCertificateRoles.getStatus();
        }

        return Future<SSLPeerInfo>::makeReady(
            SSLPeerInfo(peerSubjectName, sni, std::move(swPeerCertificateRoles.getValue())));
    } else {
        return Future<SSLPeerInfo>::makeReady(SSLPeerInfo(peerSubjectName));
    }
}

constexpr size_t kSHA1HashBytes = 20;

Status getCertInfo(CertInformationToLog* info, PCCERT_CONTEXT cert) {
    info->subject = uassertStatusOK(getCertificateSubjectName(cert));
    info->issuer = uassertStatusOK(blobToName(cert->pCertInfo->Issuer));

    DWORD bufSize = kSHA1HashBytes;
    info->thumbprint.resize(kSHA1HashBytes);

    if (!CertGetCertificateContextProperty(
            cert, CERT_SHA1_HASH_PROP_ID, info->thumbprint.data(), &bufSize)) {
        auto ec = lastSystemError();
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "getCertInfo failed to get certificate thumbprint: "
                                    << errorMessage(ec));
    }
    info->hexEncodedThumbprint = hexblob::encode(info->thumbprint.data(), info->thumbprint.size());

    info->validityNotBefore =
        Date_t::fromMillisSinceEpoch(FiletimeToEpocMillis(cert->pCertInfo->NotBefore));
    info->validityNotAfter =
        Date_t::fromMillisSinceEpoch(FiletimeToEpocMillis(cert->pCertInfo->NotAfter));

    return Status::OK();
}

Status getCRLInfo(CRLInformationToLog* info, PCCRL_CONTEXT crl) {
    DWORD bufSize = kSHA1HashBytes;
    info->thumbprint.resize(kSHA1HashBytes);

    if (!CertGetCRLContextProperty(
            crl, CERT_SHA1_HASH_PROP_ID, info->thumbprint.data(), &bufSize)) {
        auto ec = lastSystemError();
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream()
                          << "getCRLInfo failed to get CRL thumbprint: " << errorMessage(ec));
    }

    info->validityNotBefore =
        Date_t::fromMillisSinceEpoch(FiletimeToEpocMillis(crl->pCrlInfo->ThisUpdate));
    info->validityNotAfter =
        Date_t::fromMillisSinceEpoch(FiletimeToEpocMillis(crl->pCrlInfo->NextUpdate));

    return Status::OK();
}

SSLInformationToLog SSLManagerWindows::getSSLInformationToLog() const {
    SSLInformationToLog info;

    auto serverCert = _serverCertificates[0];
    if (serverCert != nullptr) {
        uassertStatusOK(getCertInfo(&info.server, serverCert));
    }

    auto clientCert = _clientCertificates[0];
    if (clientCert != nullptr) {
        CertInformationToLog cluster;
        uassertStatusOK(getCertInfo(&cluster, clientCert));
        info.cluster = cluster;
    }

    if (_serverEngine.hasCRL) {
        HCERTSTORE store = const_cast<UniqueCertStore&>(_serverEngine.CAstore);
        DWORD flags = 0;
        auto crl = CertGetCRLFromStore(store, nullptr, nullptr, &flags);
        if (crl != nullptr) {
            UniqueCRL crlHolder(crl);
            CRLInformationToLog crlInfo;
            uassertStatusOK(getCRLInfo(&crlInfo, crl));
            info.crl = crlInfo;
        }
    }

    return info;
}

}  // namespace
}  // namespace mongo
