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


#include "mongo/base/init.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/config.h"
#include "mongo/db/connection_health_metrics_parameter_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/ssl_connection_context.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/hex.h"
#include "mongo/util/net/private/ssl_expiration.h"
#include "mongo/util/net/sockaddr.h"
#include "mongo/util/net/socket_exception.h"
#include "mongo/util/net/ssl.hpp"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/net/ssl_parameters_gen.h"
#include "mongo/util/net/ssl_peer_info.h"
#include "mongo/util/net/ssl_types.h"
#include "mongo/util/net/ssl_util.h"
#include "mongo/util/str.h"
#include "mongo/util/text.h"

#include <atomic>
#include <fstream>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include <asio.hpp>
#include <ncrypt.h>
#include <winhttp.h>

#include <boost/algorithm/string/replace.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {

using transport::SSLConnectionContext;

namespace {

// This failpoint is a no-op on Windows.
MONGO_FAIL_POINT_DEFINE(disableStapling);

// Bitmask representing all TLS protocol bits supported by SChannel.
constexpr uint32_t kAllTLSProtocols =
    SP_PROT_TLS1_0 | SP_PROT_TLS1_1 | SP_PROT_TLS1_2 | SP_PROT_TLS1_3;

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
 * Free a HCRYPTPROV Handle (legacy CAPI, used when acquiring keys from the Windows cert store).
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
 * Free an NCrypt handle (provider or key).
 */
struct NcryptFree {
    void operator()(NCRYPT_HANDLE const h) noexcept {
        if (h) {
            ::NCryptFreeObject(h);
        }
    }
};

using UniqueNcryptProvider = AutoHandle<NCRYPT_PROV_HANDLE, NcryptFree>;
using UniqueNcryptKey = AutoHandle<NCRYPT_KEY_HANDLE, NcryptFree>;

/**
 * Permanently delete a named NCrypt key container from persistent storage.
 * NCryptDeleteKey both removes the on-disk container and releases the in-memory handle, so
 * NCryptFreeObject must NOT be called separately.
 */
struct NcryptKeyDeleter {
    void operator()(NCRYPT_KEY_HANDLE const h) noexcept {
        if (h) {
            ::NCryptDeleteKey(h, 0);
        }
    }
};

using UniqueNcryptKeyWithDeletion = AutoHandle<NCRYPT_KEY_HANDLE, NcryptKeyDeleter>;

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
 * A certificate loaded from a PEM file with its CNG private key stored in a named key container.
 * The certificate context's CERT_KEY_PROV_INFO_PROP_ID references the container by name and
 * provider; Schannel opens the container by name at handshake time rather than enumerating all
 * containers in the KSP.  When this struct is destroyed, NCryptDeleteKey removes the named
 * container from persistent storage, cleaning up the ephemeral key material.
 */
struct CertificateWithKey {
    UniqueCertificate cert;
    UniqueNcryptKeyWithDeletion key;

    CertificateWithKey() = default;
    CertificateWithKey(UniqueCertificate c, UniqueNcryptKeyWithDeletion k)
        : cert(std::move(c)), key(std::move(k)) {}

    // AutoHandle's move assignment deliberately skips freeing the old handle (it is designed
    // for "give away ownership" patterns).  For certificate rotation we need the old named key
    // container to be deleted, so we save the old handle, transfer the new one, then delete.
    CertificateWithKey& operator=(CertificateWithKey&& other) noexcept {
        if (this != &other) {
            cert = std::move(other.cert);
            NCRYPT_KEY_HANDLE oldHandle = static_cast<NCRYPT_KEY_HANDLE>(key);
            key = std::move(other.key);
            if (oldHandle) {
                NcryptKeyDeleter()(oldHandle);
            }
        }
        return *this;
    }
    CertificateWithKey(CertificateWithKey&&) = default;
    CertificateWithKey(const CertificateWithKey&) = delete;
    CertificateWithKey& operator=(const CertificateWithKey&) = delete;

    explicit operator bool() const {
        return static_cast<bool>(cert);
    }
    PCCERT_CONTEXT get() const {
        return cert.get();
    }
    const CERT_CONTEXT* operator->() const {
        return cert.get();
    }
};
using UniqueCertificateWithPrivateKey = CertificateWithKey;


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
    SCH_CREDENTIALS* _cred;
    Socket* socket;
    asio::ssl::detail::engine _engine;

    std::vector<char> _tempBuffer;

    SSLConnectionWindows(SCH_CREDENTIALS* cred, Socket* sock, const char* initialBytes, int len);

    void* getConnection() final {
        return _engine.native_handle();
    }

    ~SSLConnectionWindows();
};


class SSLManagerWindows : public SSLManagerInterface {
public:
    explicit SSLManagerWindows(const SSLParams& params,
                               const boost::optional<TransientSSLParams>& transientSSLParams,
                               bool isServer);

    ~SSLManagerWindows() override;

    /**
     * Initializes an OpenSSL context according to the provided settings. Only settings which are
     * acceptable on non-blocking connections are set.
     */
    Status initSSLContext(SCH_CREDENTIALS* cred,
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

    Status stapleOCSPResponse(SCH_CREDENTIALS* cred, bool asyncOCSPStaple) final;

    const SSLConfiguration& getSSLConfiguration() const final {
        return _sslConfiguration;
    }

    int SSL_read(SSLConnectionInterface* conn, void* buf, int num) final;

    int SSL_write(SSLConnectionInterface* conn, const void* buf, int num) final;

    int SSL_shutdown(SSLConnectionInterface* conn) final;

    SSLInformationToLog getSSLInformationToLog() const final;

    void stopJobs() final;

    SSLManagerMode getSSLManagerMode() const final;

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
    // If set, this parameters are used to create new transient SSL connection.
    const boost::optional<TransientSSLParams> _transientSSLParams;

    TLS_PARAMETERS _clientTLSCred;
    SCH_CREDENTIALS _clientCred;
    TLS_PARAMETERS _serverTLSCred;
    SCH_CREDENTIALS _serverCred;

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

    // Thumbprints of intermediate CA certs we added to the Windows system "CA" store.
    // Each entry is the CERT_SHA1_HASH_PROP_ID value: a SHA1 hash of the raw DER-encoded cert
    // bytes, computed by Windows CryptoAPI as a content fingerprint.  This is independent of
    // the certificate's own signature algorithm (which may be SHA256, etc.).
    // Schannel TLS 1.3 (SCH_CREDENTIALS) only searches system stores when building the
    // Certificate message chain, so we install intermediate CAs there and remove them on
    // destruction.
    std::vector<std::array<BYTE, 20>> _addedIntermediateCAThumbprints;
};

GlobalInitializerRegisterer sslManagerInitializer(
    "SSLManager",
    [](InitializerContext*) {
        if (!isSSLServer || (sslGlobalParams.sslMode.load() != SSLParams::SSLMode_disabled)) {
            theSSLManagerCoordinator = new SSLManagerCoordinator();
        }
    },
    [](DeinitializerContext*) {
        if (theSSLManagerCoordinator) {
            delete theSSLManagerCoordinator;
            theSSLManagerCoordinator = nullptr;
        }
    },
    {"EndStartupOptionHandling"},
    {});

SSLConnectionWindows::SSLConnectionWindows(SCH_CREDENTIALS* cred,
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
    return std::make_shared<SSLManagerWindows>(params, transientSSLParams, isServer);
}

std::shared_ptr<SSLManagerInterface> SSLManagerInterface::create(const SSLParams& params,
                                                                 bool isServer) {
    return std::make_shared<SSLManagerWindows>(
        params, boost::optional<TransientSSLParams>{}, isServer);
}

namespace {

StatusWith<std::vector<std::string>> getSubjectAlternativeNames(PCCERT_CONTEXT cert);

SSLManagerWindows::SSLManagerWindows(const SSLParams& params,
                                     const boost::optional<TransientSSLParams>& transientSSLParams,
                                     bool isServer)
    : _weakValidation(params.sslWeakCertificateValidation),
      _allowInvalidCertificates(params.sslAllowInvalidCertificates),
      _allowInvalidHostnames(params.sslAllowInvalidHostnames),
      _suppressNoCertificateWarning(params.suppressNoTLSPeerCertificateWarning),
      _transientSSLParams(transientSSLParams) {
    memset(&_clientTLSCred, 0, sizeof(_clientTLSCred));
    memset(&_serverTLSCred, 0, sizeof(_serverTLSCred));
    _clientCred.pTlsParameters = &_clientTLSCred;
    _serverCred.pTlsParameters = &_serverTLSCred;

    if (MONGO_unlikely(getSSLManagerMode() == SSLManagerMode::TransientWithOverride)) {
        uassert(ErrorCodes::InvalidSSLConfiguration,
                "New transient connections are only supported from client-to-server",
                !isServer);

        // Transient connections have priority over global SSL params.
        const auto& tlsParams = transientSSLParams->getTLSCredentials();

        _allowInvalidCertificates = tlsParams->tlsAllowInvalidCertificates;
        _allowInvalidHostnames = tlsParams->tlsAllowInvalidHostnames;
    }

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

SSLManagerWindows::~SSLManagerWindows() {
    if (_addedIntermediateCAThumbprints.empty()) {
        return;
    }
    HCERTSTORE hSystemCAStore = CertOpenSystemStoreW(NULL, L"CA");
    if (!hSystemCAStore) {
        return;
    }
    for (auto& thumbprint : _addedIntermediateCAThumbprints) {
        CRYPT_HASH_BLOB hashBlob = {20, const_cast<BYTE*>(thumbprint.data())};
        PCCERT_CONTEXT pCert = CertFindCertificateInStore(
            hSystemCAStore, X509_ASN_ENCODING, 0, CERT_FIND_SHA1_HASH, &hashBlob, NULL);
        if (pCert) {
            CertDeleteCertificateFromStore(pCert);  // consumes pCert
        }
    }
    CertCloseStore(hSystemCAStore, 0);
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

                LOGV2_DEBUG(
                    7998036, 0, "TLS SSL_read: recv returned encrypted data", "bytes"_attr = ret);

                conn->_engine.put_input(asio::const_buffer(conn->_tempBuffer.data(), ret));

                continue;
            }
            case asio::ssl::detail::engine::want_output:
            case asio::ssl::detail::engine::want_output_and_retry: {
                // TLS 1.3 post-handshake response (e.g. KeyUpdate acknowledgement):
                // send the queued output, then retry the read if needed.
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
                LOGV2_FATAL(23283, "Unexpected ASIO state", "state"_attr = static_cast<int>(want));
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
            blob.data(), blob.size(), CRYPT_STRING_BASE64HEADER, NULL, &decodeLen, NULL, NULL)) {
        auto ec = lastSystemError();
        if (ec != systemError(ERROR_MORE_DATA)) {
            return Status(ErrorCodes::InvalidSSLConfiguration,
                          str::stream() << "CryptStringToBinary failed to get size of key: "
                                        << errorMessage(ec));
        }
    }

    std::vector<BYTE> binaryBlobBuf;
    binaryBlobBuf.resize(decodeLen);

    if (!CryptStringToBinaryA(blob.data(),
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

        pos = (blobBuf.data() + blobBuf.size()) - buffer.data();

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

// Returns true if pCert has CA:TRUE in its Basic Constraints extension.
static bool isCACertificate(PCCERT_CONTEXT pCert) {
    // Check Basic Constraints v2 (most common for modern certs)
    PCERT_EXTENSION pExt = CertFindExtension(
        szOID_BASIC_CONSTRAINTS2, pCert->pCertInfo->cExtension, pCert->pCertInfo->rgExtension);
    if (pExt) {
        CERT_BASIC_CONSTRAINTS2_INFO* pInfo = nullptr;
        DWORD cbInfo = 0;
        if (CryptDecodeObjectEx(X509_ASN_ENCODING,
                                szOID_BASIC_CONSTRAINTS2,
                                pExt->Value.pbData,
                                pExt->Value.cbData,
                                CRYPT_DECODE_ALLOC_FLAG,
                                nullptr,
                                &pInfo,
                                &cbInfo)) {
            bool isCA = (pInfo->fCA != FALSE);
            LocalFree(pInfo);
            return isCA;
        }
    }

    // Fallback: Basic Constraints v1 (older certs)
    pExt = CertFindExtension(
        szOID_BASIC_CONSTRAINTS, pCert->pCertInfo->cExtension, pCert->pCertInfo->rgExtension);
    if (pExt) {
        CERT_BASIC_CONSTRAINTS_INFO* pInfo = nullptr;
        DWORD cbInfo = 0;
        if (CryptDecodeObjectEx(X509_ASN_ENCODING,
                                szOID_BASIC_CONSTRAINTS,
                                pExt->Value.pbData,
                                pExt->Value.cbData,
                                CRYPT_DECODE_ALLOC_FLAG,
                                nullptr,
                                &pInfo,
                                &cbInfo)) {
            bool isCA = (pInfo->SubjectType.cbData > 0 &&
                         (pInfo->SubjectType.pbData[0] & CERT_CA_SUBJECT_FLAG));
            LocalFree(pInfo);
            return isCA;
        }
    }
    return false;
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
        buf.find("CERTIFICATE", (publicKeyBlob.data() + publicKeyBlob.size()) - buf.data());
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

    // Use CertCreateCertificateContext rather than CertAddEncodedCertificateToStore.
    // The latter routes through CertAddCertificateContextToStore, which allocates a property-table
    // slot for CERT_NCRYPT_KEY_HANDLE_TRANSFER_PROP_ID (propId 99) without initialising it,
    // leaving pbData = NULL with cbData != 0.  CertCreateCertificateContext never copies
    // properties, so the table is clean and the property can be set safely.
    PCCERT_CONTEXT cert =
        CertCreateCertificateContext(X509_ASN_ENCODING, certBuf.data(), certBuf.size());
    if (!cert) {
        auto ec = lastSystemError();
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "CertCreateCertificateContext failed: " << errorMessage(ec));
    }

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


    // Generate a container name that is unique per import to avoid cross-process and
    // intra-process contention on the MS_KEY_STORAGE_PROVIDER.
    //
    // Why uniqueness matters:
    //   If we used a deterministic name (e.g. the cert thumbprint), concurrent processes
    //   loading the same certificate (common in Evergreen CI where many test tasks run in
    //   parallel on the same host) would all try to NCryptImportKey into the same named
    //   container.  NCryptImportKey with NCRYPT_OVERWRITE_KEY_FLAG blocks until it can
    //   acquire an exclusive lock on the container — if another process has the container
    //   open, the call hangs indefinitely, causing the "HIT EVERGREEN TIMEOUT" failures.
    //
    // Container name format: "mongod_<PID>_<counter>" where counter is a process-global
    // atomic that increments on every call.  This guarantees uniqueness within a process
    // (across certificate rotations) and across processes (different PIDs), with no locking.
    //
    // Why NCRYPT_OVERWRITE_KEY_FLAG is safe here:
    //   PIDs are unique among *running* processes, so no two live processes share a
    //   container name.  The only scenario in which a "mongod_<PID>_<N>" container
    //   already exists is when a previous process with the same recycled PID was
    //   force-killed (e.g. by the Evergreen hang-analyser) before NCryptDeleteKey ran,
    //   leaving a stale orphan on disk.  Because that process is dead, no handle is open
    //   on the container, so NCRYPT_OVERWRITE_KEY_FLAG overwrites it instantly without
    //   blocking.  Without this flag, NCryptImportKey would return NTE_EXISTS
    //   (0x8009000F) and the server would fail to start.
    static std::atomic<uint64_t> keyContainerCounter{0};
    wchar_t containerName[64] = {};
    _snwprintf_s(containerName,
                 static_cast<int>(std::size(containerName)),
                 _TRUNCATE,
                 L"mongod_%lu_%llu",
                 static_cast<unsigned long>(GetCurrentProcessId()),
                 static_cast<unsigned long long>(keyContainerCounter.fetch_add(1)));

    LOGV2_DEBUG(7998007,
                2,
                "Importing private key into CNG key storage provider",
                "containerName"_attr = toUtf8String(containerName));

    // Open the Microsoft Software Key Storage Provider (CNG). SCH_CREDENTIALS (version 5),
    // required for TLS 1.3, needs a CNG key rather than a legacy CAPI key.
    NCRYPT_PROV_HANDLE hProvider = 0;
    SECURITY_STATUS ss = NCryptOpenStorageProvider(&hProvider, MS_KEY_STORAGE_PROVIDER, 0);
    if (ss != ERROR_SUCCESS) {
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "NCryptOpenStorageProvider failed with error: " << ss);
    }
    UniqueNcryptProvider providerGuard(hProvider);

    // Import the private key into a named container.
    //
    // Use CERT_KEY_PROV_INFO_PROP_ID (propId 2) to associate the key with the certificate rather
    // than CERT_NCRYPT_KEY_HANDLE_PROP_ID (propId 78) or CERT_NCRYPT_KEY_HANDLE_TRANSFER_PROP_ID
    // (propId 99).  Setting propId 78 or 99 triggers an internal CertGetCertificateContextProperty
    // call that, if the handle is not yet cached, enumerates all key containers in the KSP to find
    // a matching public key — an unnecessary and potentially slow operation.  Setting propId 2
    // stores only a name/provider blob; when Schannel needs the private key for the TLS handshake
    // it resolves propId 78 from propId 2 by opening the named container directly (NCryptOpenKey
    // by name), which is both simpler and more predictable.
    //
    // NCRYPT_OVERWRITE_KEY_FLAG is safe here because each container name is PID-scoped:
    // no two *running* processes share a name, so the flag only ever overwrites orphaned
    // containers left by previously force-killed processes (see comment above).
    NCryptBuffer nameBuffer = {};
    nameBuffer.cbBuffer = static_cast<ULONG>((wcslen(containerName) + 1) * sizeof(wchar_t));
    nameBuffer.BufferType = NCRYPTBUFFER_PKCS_KEY_NAME;
    nameBuffer.pvBuffer = containerName;
    NCryptBufferDesc paramList = {};
    paramList.ulVersion = NCRYPTBUFFER_VERSION;
    paramList.cBuffers = 1;
    paramList.pBuffers = &nameBuffer;

    NCRYPT_KEY_HANDLE hKey = 0;
    ss = NCryptImportKey(hProvider,
                         NULL,
                         LEGACY_RSAPRIVATE_BLOB,
                         &paramList,
                         &hKey,
                         privateKey.data(),
                         static_cast<ULONG>(privateKey.size()),
                         NCRYPT_SILENT_FLAG | NCRYPT_OVERWRITE_KEY_FLAG);
    if (ss != ERROR_SUCCESS) {
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "NCryptImportKey failed with error: " << ss);
    }
    // keyGuard calls NCryptDeleteKey on destruction, removing the named container from disk.
    UniqueNcryptKeyWithDeletion keyGuard(hKey);

    // Point the cert context at the named container via CERT_KEY_PROV_INFO_PROP_ID.
    // This is a plain blob write; it never dereferences a key handle or enumerates the KSP.
    CRYPT_KEY_PROV_INFO provInfo = {};
    provInfo.pwszContainerName = containerName;
    provInfo.pwszProvName = const_cast<LPWSTR>(MS_KEY_STORAGE_PROVIDER);
    provInfo.dwProvType = 0;  // 0 = CNG provider, not legacy CAPI
    provInfo.dwKeySpec = AT_KEYEXCHANGE;
    if (!CertSetCertificateContextProperty(
            certHolder.get(), CERT_KEY_PROV_INFO_PROP_ID, 0, &provInfo)) {
        auto ec = lastSystemError();
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "CertSetCertificateContextProperty "
                                       "(CERT_KEY_PROV_INFO_PROP_ID) failed: "
                                    << errorMessage(ec));
    }

    // Add the extra certificates into the same certificate store as the certificate.
    uassertStatusOK(addCertificatesToStore(certHolder->hCertStore, extraCertificates));

    return CertificateWithKey{std::move(certHolder), std::move(keyGuard)};
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

        pos += blobBuf.size();

        auto swCert = decodePEMBlob(blobBuf);
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
                          str::stream() << "CertAddCRLContextToStore Failed: " << errorMessage(ec));
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

bool hasCertificateSelector(const SSLParams::CertificateSelector selector) {
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

SSLManagerWindows::SSLManagerMode SSLManagerWindows::getSSLManagerMode() const {
    if (!_transientSSLParams) {
        return SSLManagerMode::Normal;
    } else if (_transientSSLParams->createNewConnection()) {
        return SSLManagerMode::TransientWithOverride;
    } else {
        return SSLManagerMode::TransientNoOverride;
    }
}

Status SSLManagerWindows::_loadCertificates(const SSLParams& params) {
    _clientCertificates[0] = nullptr;
    _serverCertificates[0] = nullptr;

    SSLManagerMode managerMode = getSSLManagerMode();
    auto sslConfig = parseSSLCoreParams(params, _transientSSLParams);

    const auto [crlfile, certificateSelector, clusterCertificateSelector] =
        [&]() -> std::tuple<const std::string&,
                            const SSLParams::CertificateSelector*,
                            const SSLParams::CertificateSelector*> {
        if (MONGO_unlikely(_transientSSLParams)) {
            const auto& tlsParams = _transientSSLParams->getTLSCredentials();
            return {tlsParams->tlsCRLFile,
                    &tlsParams->tlsCertificateSelector,
                    &tlsParams->tlsClusterCertificateSelector};
        } else {
            return {params.sslCRLFile,
                    &params.sslCertificateSelector,
                    &params.sslClusterCertificateSelector};
        }
    }();

    // Load the normal PEM file
    if (!sslConfig.clientPEM.empty()) {
        auto swCertificate = readCertPEMFile(sslConfig.clientPEM, sslConfig.clientPassword);
        if (!swCertificate.isOK()) {
            return swCertificate.getStatus();
        }

        _pemCertificate = std::move(swCertificate.getValue());
    }

    // Load the cluster PEM file, only applies to server side code
    if (!params.sslClusterFile.empty() && managerMode != SSLManagerMode::TransientWithOverride) {
        auto swCertificate = readCertPEMFile(params.sslClusterFile, params.sslClusterPassword);
        if (!swCertificate.isOK()) {
            return swCertificate.getStatus();
        }

        _clusterPEMCertificate = std::move(swCertificate.getValue());
    }

    if (_pemCertificate) {
        _clientCertificates[0] = _pemCertificate.get();
        _serverCertificates[0] = _pemCertificate.get();
    }

    if (_clusterPEMCertificate) {
        _clientCertificates[0] = _clusterPEMCertificate.get();
    }

    // If the user has specified --setParameter tlsUseSystemCA=true, then no params.sslCAFile nor
    // params.sslClusterCAFile will be defined, and the SSL Manager will fall back to the System CA.
    if (!sslConfig.cafile.empty()) {

        auto swChain = readCertChains(sslConfig.cafile, crlfile);
        if (!swChain.isOK()) {
            return swChain.getStatus();
        }

        // Dump the CA cert chain into the memory store for the client cert. This ensures Windows
        // can build a complete chain to send to the remote side.
        if (_pemCertificate) {
            auto status = readCAPEMFile(_pemCertificate.get()->hCertStore, sslConfig.cafile);
            if (!status.isOK()) {
                return status;
            }
        }

        _clientEngine.CAstore = std::move(swChain.getValue());
    }
    _clientEngine.hasCRL = !crlfile.empty();

    const auto serverCAFile =
        (params.sslClusterCAFile.empty() || managerMode == SSLManagerMode::TransientWithOverride)
        ? sslConfig.cafile
        : params.sslClusterCAFile;
    if (!serverCAFile.empty()) {
        auto swChain = readCertChains(serverCAFile, params.sslCRLFile);
        if (!swChain.isOK()) {
            return swChain.getStatus();
        }

        // Dump the CA cert chain into the memory store for the cluster cert. This ensures Windows
        // can build a complete chain to send to the remote side.
        if (_clusterPEMCertificate) {
            auto status = readCAPEMFile(_clusterPEMCertificate.get()->hCertStore, sslConfig.cafile);
            if (!status.isOK()) {
                return status;
            }
        }

        _serverEngine.CAstore = std::move(swChain.getValue());
    }
    _serverEngine.hasCRL = !crlfile.empty();

    // Schannel TLS 1.3 (SCH_CREDENTIALS) only searches Windows system stores when building the
    // Certificate message chain — it ignores the cert's hCertStore even with
    // SCH_CRED_MEMORY_STORE_CERT set.  Install any intermediate CA certs (non-self-signed) into the
    // user-level system "CA" store so Schannel can find them.  We track the SHA1 thumbprints so we
    // can remove the certs in the destructor.
    auto addIntermediateCAsToSystemStore = [&](HCERTSTORE sourceCertStore) {
        if (!sourceCertStore) {
            return;
        }
        HCERTSTORE hSystemCAStore = CertOpenSystemStoreW(NULL, L"CA");
        if (!hSystemCAStore) {
            LOGV2_DEBUG(
                7998018, 2, "addIntermediateCAsToSystemStore: failed to open user CA system store");
            return;
        }
        DWORD addedCount = 0;
        PCCERT_CONTEXT pCert = NULL;
        while ((pCert = CertEnumCertificatesInStore(sourceCertStore, pCert)) != NULL) {
            // Skip self-signed (root) certs — only install intermediate CAs.
            if (CertCompareCertificateName(
                    X509_ASN_ENCODING, &pCert->pCertInfo->Issuer, &pCert->pCertInfo->Subject)) {
                continue;
            }
            // Skip leaf certs (certs without CA:TRUE in Basic Constraints).
            if (!isCACertificate(pCert)) {
                continue;
            }
            char subjectBuf[256] = {};
            CertNameToStrA(X509_ASN_ENCODING,
                           &pCert->pCertInfo->Subject,
                           CERT_X500_NAME_STR,
                           subjectBuf,
                           sizeof(subjectBuf));
            PCCERT_CONTEXT pNewCert = NULL;
            BOOL added = CertAddCertificateContextToStore(
                hSystemCAStore, pCert, CERT_STORE_ADD_NEW, &pNewCert);
            LOGV2_DEBUG(7998011,
                        2,
                        "addIntermediateCAsToSystemStore: intermediate CA cert",
                        "subject"_attr = subjectBuf,
                        "addedToSystemStore"_attr = (bool)added);
            if (added && pNewCert) {
                ++addedCount;
                std::array<BYTE, 20> sha1{};
                DWORD cbHash = 20;
                if (CertGetCertificateContextProperty(
                        pNewCert, CERT_SHA1_HASH_PROP_ID, sha1.data(), &cbHash)) {
                    _addedIntermediateCAThumbprints.push_back(sha1);
                }
                CertFreeCertificateContext(pNewCert);
            }
        }
        LOGV2_DEBUG(7998019,
                    2,
                    "addIntermediateCAsToSystemStore: complete",
                    "addedCount"_attr = addedCount);
        CertCloseStore(hSystemCAStore, 0);
    };
    if (_pemCertificate) {
        addIntermediateCAsToSystemStore(_pemCertificate.get()->hCertStore);
    }
    if (_clusterPEMCertificate) {
        addIntermediateCAsToSystemStore(_clusterPEMCertificate.get()->hCertStore);
    }

    // The hCertStore path above only picks up certs explicitly added via addCertificatesToStore.
    // Read ALL certs from each PEM key file directly so that any intermediate CA embedded in the
    // key file (after the leaf cert) is also installed into the system "CA" store.  Schannel TLS
    // 1.3 needs the intermediate CA in the system store to include it in the Certificate message.
    auto addIntermediateCAFromPEMFile = [&](StringData pemFile) {
        if (pemFile.empty()) {
            return;
        }
        auto swBuf = ssl_util::readPEMFile(pemFile);
        if (!swBuf.isOK()) {
            LOGV2_DEBUG(7998020,
                        2,
                        "addIntermediateCAFromPEMFile: failed to read PEM file",
                        "file"_attr = pemFile,
                        "error"_attr = swBuf.getStatus().reason());
            return;
        }
        auto swCerts = readCAPEMBuffer(swBuf.getValue());
        if (!swCerts.isOK()) {
            LOGV2_DEBUG(7998021,
                        2,
                        "addIntermediateCAFromPEMFile: failed to parse PEM certs",
                        "file"_attr = pemFile,
                        "error"_attr = swCerts.getStatus().reason());
            return;
        }
        auto& certs = swCerts.getValue();
        LOGV2_DEBUG(7998022,
                    2,
                    "addIntermediateCAFromPEMFile: parsed certs from key file",
                    "file"_attr = pemFile,
                    "count"_attr = certs.size());
        if (certs.empty()) {
            return;
        }
        HCERTSTORE hTempStore = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, NULL, 0, NULL);
        if (!hTempStore) {
            return;
        }
        for (auto& cert : certs) {
            CertAddCertificateContextToStore(
                hTempStore, cert.get(), CERT_STORE_ADD_REPLACE_EXISTING, NULL);
        }
        addIntermediateCAsToSystemStore(hTempStore);
        CertCloseStore(hTempStore, 0);
    };
    addIntermediateCAFromPEMFile(sslConfig.clientPEM);
    addIntermediateCAFromPEMFile(sslConfig.cafile);
    if (managerMode != SSLManagerMode::TransientWithOverride) {
        addIntermediateCAFromPEMFile(params.sslClusterFile);
    }

    if (hasCertificateSelector(*certificateSelector)) {
        auto swCert = loadAndValidateCertificateSelector(*certificateSelector);
        if (!swCert.isOK()) {
            return swCert.getStatus();
        }
        _sslCertificate = std::move(swCert.getValue());
    }

    if (hasCertificateSelector(*clusterCertificateSelector) &&
        managerMode != SSLManagerMode::TransientWithOverride) {
        auto swCert = loadAndValidateCertificateSelector(*clusterCertificateSelector);
        if (!swCert.isOK()) {
            return swCert.getStatus();
        }
        _sslClusterCertificate = std::move(swCert.getValue());
    }

    if (_sslCertificate || _sslClusterCertificate) {
        if (!sslConfig.cafile.empty()) {
            LOGV2_WARNING(23271,
                          "Mixing certs from the system certificate store and PEM files. This may "
                          "produce unexpected results.");
        }
    }

    if (_sslCertificate) {
        if (!_clientCertificates[0]) {
            _clientCertificates[0] = _sslCertificate.get();
        }
        _serverCertificates[0] = _sslCertificate.get();
    }

    if (_sslClusterCertificate) {
        _clientCertificates[0] = _sslClusterCertificate.get();
    }

    return Status::OK();
}

Status SSLManagerWindows::initSSLContext(SCH_CREDENTIALS* cred,
                                         const SSLParams& params,
                                         ConnectionDirection direction) {

    auto* tlsParams = cred->pTlsParameters;
    *tlsParams = {};
    // SCH_USE_STRONG_CRYPTO is a legacy flag intended for SCHANNEL_CRED (version 4) and must not
    // be set on SCH_CREDENTIALS (version 5).  On Windows Server 2022 and later it causes
    // AcquireCredentialsHandle to fail with SEC_E_NO_LSA.  Protocol/cipher strength is already
    // controlled via TLS_PARAMETERS.grbitDisabledProtocols below.
    *cred = {.dwVersion = SCH_CREDENTIALS_VERSION, .pTlsParameters = tlsParams};

    const auto [disabledProtocols, cipherConfig] =
        [&]() -> std::pair<const std::vector<SSLParams::Protocols>*, const std::string&> {
        if (MONGO_unlikely(_transientSSLParams)) {
            const auto& tlsParams = _transientSSLParams->getTLSCredentials();
            return {&tlsParams->tlsDisabledProtocols, tlsParams->tlsCipherConfig};
        } else {
            return {&params.sslDisabledProtocols, params.sslCipherConfig};
        }
    }();

    if (direction == ConnectionDirection::kIncoming) {
        cred->hRootStore = _serverEngine.CAstore;
        cred->dwFlags = cred->dwFlags          // flags
            | SCH_CRED_REVOCATION_CHECK_CHAIN  // Check certificate revocation
            | SCH_CRED_SNI_CREDENTIAL          // Pass along SNI creds
            | SCH_CRED_MEMORY_STORE_CERT       // Read intermediate certificates from memory
                                               // store associated with client certificate.
            | SCH_CRED_NO_SYSTEM_MAPPER        // Do not map certificate to user account
            | SCH_CRED_DISABLE_RECONNECTS;     // Do not support reconnects

    } else {
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
    uint32_t disabledProtocolsFlag = 0;
    for (const SSLParams::Protocols& protocol : *disabledProtocols) {
        if (protocol == SSLParams::Protocols::TLS1_0) {
            disabledProtocolsFlag |= SP_PROT_TLS1_0;
        } else if (protocol == SSLParams::Protocols::TLS1_1) {
            disabledProtocolsFlag |= SP_PROT_TLS1_1;
        } else if (protocol == SSLParams::Protocols::TLS1_2) {
            disabledProtocolsFlag |= SP_PROT_TLS1_2;
        } else if (protocol == SSLParams::Protocols::TLS1_3) {
            disabledProtocolsFlag |= SP_PROT_TLS1_3;
        }
    }

    cred->cTlsParameters = 1;
    cred->pTlsParameters->grbitDisabledProtocols = disabledProtocolsFlag;
    if (cred->pTlsParameters->grbitDisabledProtocols == kAllTLSProtocols) {
        return {ErrorCodes::InvalidSSLConfiguration,
                "All supported TLS protocols have been disabled."};
    }

    if (cipherConfig != kSSLCipherConfigDefault) {
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
    uassertStatusOK(initSSLContext(conn->_cred,
                                   getSSLGlobalParams(),
                                   client ? SSLManagerInterface::ConnectionDirection::kOutgoing
                                          : SSLManagerInterface::ConnectionDirection::kIncoming));

    LOGV2_DEBUG(7998009, 2, "TLS handshake starting", "role"_attr = (client ? "client" : "server"));

    int iteration = 0;
    while (true) {
        asio::error_code ec;
        asio::ssl::detail::engine::want want =
            conn->_engine.handshake(client ? asio::ssl::stream_base::handshake_type::client
                                           : asio::ssl::stream_base::handshake_type::server,
                                    ec);
        LOGV2_DEBUG(7998010,
                    2,
                    "TLS handshake iteration",
                    "role"_attr = (client ? "client" : "server"),
                    "iteration"_attr = iteration++,
                    "want"_attr = static_cast<int>(want),
                    "ecMessage"_attr = (ec ? ec.message() : std::string{}));
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
    LPSTR serverUsage[] = {
        const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH),
    };

    // szOID_PKIX_KP_CLIENT_AUTH ("1.3.6.1.5.5.7.3.2") - means the certificate can be used for
    // client authentication
    LPSTR clientUsage[] = {
        const_cast<LPSTR>(szOID_PKIX_KP_CLIENT_AUTH),
    };

    // If remoteHost is not empty, then this is running on the client side, and we want to verify
    // the server cert.
    if (!remoteHost.empty()) {
        certChainPara.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
        certChainPara.RequestedUsage.Usage.cUsageIdentifier = _countof(serverUsage);
        certChainPara.RequestedUsage.Usage.rgpszUsageIdentifier = serverUsage;
    }  // else, this is running on the server side, validate the client cert
    else {
        certChainPara.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
        certChainPara.RequestedUsage.Usage.cUsageIdentifier = _countof(clientUsage);
        certChainPara.RequestedUsage.Usage.rgpszUsageIdentifier = clientUsage;
    }

    certChainPara.dwUrlRetrievalTimeout = gTLSOCSPVerifyTimeoutSecs * 1000;

    // Build a flat memory store combining all candidate intermediate CA certs from:
    //   (a) the certs from the peer's TLS Certificate message (cert->hCertStore),
    //   (b) the current-user "CA" store (populated with intermediate CAs at startup), and
    //   (c) the local-machine "CA" store.
    // This is required because CertGetCertificateChain with hExclusiveRoot set does NOT
    // search system CA stores for intermediate certificates — it only looks in the
    // hAdditionalStore argument and hExclusiveRoot itself.
    //
    // We use a flat MEMORY store (not a collection store) because CertGetCertificateChain
    // does not enumerate sibling stores when the hAdditionalStore argument is a collection
    // store — it only searches the collection store's own in-memory contents.
    auto copyStoreToFlat = [](HCERTSTORE dst, HCERTSTORE src) -> DWORD {
        DWORD count = 0;
        for (PCCERT_CONTEXT pCert = nullptr;
             (pCert = CertEnumCertificatesInStore(src, pCert)) != nullptr;) {
            if (CertAddCertificateContextToStore(dst, pCert, CERT_STORE_ADD_USE_EXISTING, nullptr))
                ++count;
        }
        return count;
    };
    UniqueCertStore additionalStoreHolder;
    HCERTSTORE hAdditionalStore = cert->hCertStore;
    if (HCERTSTORE hFlatStore = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, NULL, 0, NULL)) {
        additionalStoreHolder = hFlatStore;
        DWORD fromPeer = copyStoreToFlat(hFlatStore, cert->hCertStore);
        DWORD fromUserCA = 0;
        if (HCERTSTORE hUserCA = CertOpenSystemStoreW(NULL, L"CA")) {
            fromUserCA = copyStoreToFlat(hFlatStore, hUserCA);
            CertCloseStore(hUserCA, 0);
        }
        DWORD fromMachineCA = 0;
        if (HCERTSTORE hMachineCA =
                CertOpenStore(CERT_STORE_PROV_SYSTEM,
                              0,
                              NULL,
                              CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_READONLY_FLAG,
                              L"CA")) {
            fromMachineCA = copyStoreToFlat(hFlatStore, hMachineCA);
            CertCloseStore(hMachineCA, 0);
        }
        hAdditionalStore = hFlatStore;
        LOGV2_DEBUG(7998017,
                    2,
                    "validatePeerCertificate: flat intermediate-CA store populated",
                    "fromPeerTLSMessage"_attr = fromPeer,
                    "fromUserCAStore"_attr = fromUserCA,
                    "fromMachineCAStore"_attr = fromMachineCA,
                    "total"_attr = fromPeer + fromUserCA + fromMachineCA);
    }

    auto before = Date_t::now();
    PCCERT_CHAIN_CONTEXT chainContext;
    if (!CertGetCertificateChain(certChainEngine,
                                 cert,
                                 NULL,
                                 hAdditionalStore,
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

    LOGV2_DEBUG(7998013,
                2,
                "CertGetCertificateChain result",
                "trustErrorStatus"_attr = unsignedHex(chainContext->TrustStatus.dwErrorStatus),
                "trustInfoStatus"_attr = unsignedHex(chainContext->TrustStatus.dwInfoStatus),
                "chainCount"_attr = chainContext->cChain,
                "chainLength"_attr =
                    (chainContext->cChain > 0 ? chainContext->rgpChain[0]->cElement : 0));

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
                              "SSL peer certificate validation failed",
                              "errorCode"_attr = unsignedHex(certChainPolicyStatus.dwError),
                              "error"_attr =
                                  errorMessage(systemError(certChainPolicyStatus.dwError)));

                if (certChainPolicyStatus.dwError == CERT_E_CN_NO_MATCH) {
                    LOGV2_WARNING(23275,
                                  "The server certificate does not match the host name",
                                  "remoteHost"_attr = remoteHost,
                                  "certificateNames"_attr = certificateNames.str());
                }

                *peerSubjectName = SSLX509Name();
                return Status::OK();
            } else if (allowInvalidHostnames) {
                LOGV2_WARNING(23276,
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
        case SP_PROT_TLS1_3_CLIENT:
        case SP_PROT_TLS1_3_SERVER:
            return TLSVersion::kTLS13;
        default:
            return TLSVersion::kUnknown;
    }
}

Status SSLManagerWindows::stapleOCSPResponse(SCH_CREDENTIALS* cred, bool asyncOCSPStaple) {
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

    SECURITY_STATUS ss = QueryContextAttributes(ssl, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &cert);

    if (ss == SEC_E_NO_CREDENTIALS) {  // no certificate presented by peer
        if (_weakValidation) {
            // do not give warning if "no certificate" warnings are suppressed
            if (!_suppressNoCertificateWarning) {
                LOGV2_WARNING(23277, "No SSL certificate provided by peer");
            }
            return SSLPeerInfo(sni);
        } else {
            auto msg = "no SSL certificate provided by peer; connection rejected";
            LOGV2_ERROR(23280, "No SSL certificate provided by peer; connection rejected");
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

    if (!serverGlobalParams.quiet.load() && gEnableDetailedConnectionHealthMetricLogLines.load()) {
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
