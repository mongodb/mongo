
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/util/net/ssl_manager.h"

#include <asio.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <fstream>
#include <string>
#include <tuple>
#include <vector>
#include <winhttp.h>

#include "mongo/base/init.h"
#include "mongo/base/initializer_context.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/hex.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/private/ssl_expiration.h"
#include "mongo/util/net/socket_exception.h"
#include "mongo/util/net/ssl.hpp"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/net/ssl_types.h"
#include "mongo/util/text.h"
#include "mongo/util/uuid.h"

namespace mongo {

namespace {

SimpleMutex sslManagerMtx;
SSLManagerInterface* theSSLManager = NULL;

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

// TODO(SERVER-41045): If SNI functionality is needed on Windows, this is where one would implement
// it.
boost::optional<std::string> getSNIServerName_impl() {
    return boost::none;
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

    std::string getSNIServerName() const final {
        return getSNIServerName_impl().value_or("");
    };
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

    SSLConnectionInterface* connect(Socket* socket) final;

    SSLConnectionInterface* accept(Socket* socket, const char* initialBytes, int len) final;

    SSLPeerInfo parseAndValidatePeerCertificateDeprecated(const SSLConnectionInterface* conn,
                                                          const std::string& remoteHost,
                                                          const HostAndPort& hostForLogging) final;

    StatusWith<SSLPeerInfo> parseAndValidatePeerCertificate(
        PCtxtHandle ssl, const std::string& remoteHost, const HostAndPort& hostForLogging) final;


    const SSLConfiguration& getSSLConfiguration() const final {
        return _sslConfiguration;
    }

    int SSL_read(SSLConnectionInterface* conn, void* buf, int num) final;

    int SSL_write(SSLConnectionInterface* conn, const void* buf, int num) final;

    int SSL_shutdown(SSLConnectionInterface* conn) final;

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
};

MONGO_INITIALIZER(SSLManager)(InitializerContext*) {
    stdx::lock_guard<SimpleMutex> lck(sslManagerMtx);
    if (!isSSLServer || (sslGlobalParams.sslMode.load() != SSLParams::SSLMode_disabled)) {
        theSSLManager = new SSLManagerWindows(sslGlobalParams, isSSLServer);
    }

    return Status::OK();
}

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

std::unique_ptr<SSLManagerInterface> SSLManagerInterface::create(const SSLParams& params,
                                                                 bool isServer) {
    return stdx::make_unique<SSLManagerWindows>(params, isServer);
}

SSLManagerInterface* getSSLManager() {
    stdx::lock_guard<SimpleMutex> lck(sslManagerMtx);
    if (theSSLManager)
        return theSSLManager;
    return NULL;
}

namespace {

SSLManagerWindows::SSLManagerWindows(const SSLParams& params, bool isServer)
    : _weakValidation(params.sslWeakCertificateValidation),
      _allowInvalidCertificates(params.sslAllowInvalidCertificates),
      _allowInvalidHostnames(params.sslAllowInvalidHostnames),
      _suppressNoCertificateWarning(params.suppressNoTLSPeerCertificateWarning) {

    if (params.sslFIPSMode) {
        BOOLEAN enabled = FALSE;
        BCryptGetFipsAlgorithmMode(&enabled);
        if (!enabled) {
            severe() << "FIPS modes is not enabled on the operating system.";
            fassertFailedNoTrace(50744);
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
            uassertStatusOK(
                _validateCertificate(_serverCertificates[0],
                                     &_sslConfiguration.serverSubjectName,
                                     &_sslConfiguration.serverCertificateExpirationDate));
        }

        // Monitor the server certificate's expiration
        static CertificateExpirationMonitor task =
            CertificateExpirationMonitor(_sslConfiguration.serverCertificateExpirationDate);
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
    BOOL ret = CertCreateCertificateChainEngine(chainEngineConfig, &chainEngine);
    if (!ret) {
        DWORD gle = GetLastError();
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "CertCreateCertificateChainEngine failed: "
                                    << errnoWithDescription(gle));
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
                severe() << "Unexpected ASIO state: " << static_cast<int>(want);
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
                severe() << "Unexpected ASIO state: " << static_cast<int>(want);
                MONGO_UNREACHABLE;
        }
    }
}

int SSLManagerWindows::SSL_shutdown(SSLConnectionInterface* conn) {
    MONGO_UNREACHABLE;
    return 0;
}

StatusWith<std::string> readFile(StringData fileName) {
    std::ifstream pemFile(fileName.toString(), std::ios::binary);
    if (!pemFile.is_open()) {
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "Failed to open PEM file: " << fileName);
    }

    std::string buf((std::istreambuf_iterator<char>(pemFile)), std::istreambuf_iterator<char>());

    pemFile.close();

    return buf;
}

// Find a specific kind of PEM blob marked by BEGIN and END in a string
StatusWith<StringData> findPEMBlob(StringData blob,
                                   StringData type,
                                   size_t position = 0,
                                   bool allowEmpty = false) {
    std::string header = str::stream() << "-----BEGIN " << type << "-----";
    std::string trailer = str::stream() << "-----END " << type << "-----";

    size_t headerPosition = blob.find(header, position);
    if (headerPosition == std::string::npos) {
        if (allowEmpty) {
            return StringData();
        } else {
            return Status(ErrorCodes::InvalidSSLConfiguration,
                          str::stream() << "Failed to find PEM blob header: " << header);
        }
    }

    size_t trailerPosition = blob.find(trailer, headerPosition);
    if (trailerPosition == std::string::npos) {
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "Failed to find PEM blob trailer: " << trailer);
    }

    trailerPosition += trailer.size();

    return StringData(blob.rawData() + headerPosition, trailerPosition - headerPosition);
}

// Decode a base-64 PEM blob with headers into a binary blob
StatusWith<std::vector<BYTE>> decodePEMBlob(StringData blob) {
    DWORD decodeLen{0};

    BOOL ret = CryptStringToBinaryA(
        blob.rawData(), blob.size(), CRYPT_STRING_BASE64HEADER, NULL, &decodeLen, NULL, NULL);
    if (!ret) {
        DWORD gle = GetLastError();
        if (gle != ERROR_MORE_DATA) {
            return Status(ErrorCodes::InvalidSSLConfiguration,
                          str::stream() << "CryptStringToBinary failed to get size of key: "
                                        << errnoWithDescription(gle));
        }
    }

    std::vector<BYTE> binaryBlobBuf;
    binaryBlobBuf.resize(decodeLen);

    ret = CryptStringToBinaryA(blob.rawData(),
                               blob.size(),
                               CRYPT_STRING_BASE64HEADER,
                               binaryBlobBuf.data(),
                               &decodeLen,
                               NULL,
                               NULL);
    if (!ret) {
        DWORD gle = GetLastError();
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "CryptStringToBinary failed to read key: "
                                    << errnoWithDescription(gle));
    }

    return std::move(binaryBlobBuf);
}

StatusWith<std::vector<BYTE>> decodeObject(const char* structType,
                                           const BYTE* data,
                                           size_t length) {
    DWORD decodeLen{0};

    BOOL ret =
        CryptDecodeObjectEx(X509_ASN_ENCODING, structType, data, length, 0, NULL, NULL, &decodeLen);
    if (!ret) {
        DWORD gle = GetLastError();
        if (gle != ERROR_MORE_DATA) {
            return Status(ErrorCodes::InvalidSSLConfiguration,
                          str::stream() << "CryptDecodeObjectEx failed to get size of object: "
                                        << errnoWithDescription(gle));
        }
    }

    std::vector<BYTE> binaryBlobBuf;
    binaryBlobBuf.resize(decodeLen);

    ret = CryptDecodeObjectEx(
        X509_ASN_ENCODING, structType, data, length, 0, NULL, binaryBlobBuf.data(), &decodeLen);
    if (!ret) {
        DWORD gle = GetLastError();
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "CryptDecodeObjectEx failed to read object: "
                                    << errnoWithDescription(gle));
    }

    return std::move(binaryBlobBuf);
}

StatusWith<std::vector<UniqueCertificate>> readCAPEMBuffer(StringData buffer) {
    std::vector<UniqueCertificate> certs;

    // Search the buffer for the various strings that make up a PEM file
    size_t pos = 0;
    bool found_one = false;

    while (pos < buffer.size()) {
        auto swBlob = findPEMBlob(buffer, "CERTIFICATE"_sd, pos, pos != 0);

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
        if (cert == NULL) {
            DWORD gle = GetLastError();
            return Status(ErrorCodes::InvalidSSLConfiguration,
                          str::stream() << "CertCreateCertificateContext failed to decode cert: "
                                        << errnoWithDescription(gle));
        }

        certs.emplace_back(cert);
    }

    return {std::move(certs)};
}

Status addCertificatesToStore(HCERTSTORE certStore, std::vector<UniqueCertificate>& certificates) {
    for (auto& cert : certificates) {
        BOOL ret =
            CertAddCertificateContextToStore(certStore, cert.get(), CERT_STORE_ADD_NEW, NULL);

        if (!ret) {
            DWORD gle = GetLastError();
            return Status(ErrorCodes::InvalidSSLConfiguration,
                          str::stream() << "CertAddCertificateContextToStore Failed  "
                                        << errnoWithDescription(gle));
        }
    }

    return Status::OK();
}

// Read a Certificate PEM file with a private key from disk
StatusWith<UniqueCertificateWithPrivateKey> readCertPEMFile(StringData fileName,
                                                            StringData password) {
    auto swBuf = readFile(fileName);
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
    auto swPublicKeyBlob = findPEMBlob(buf, "CERTIFICATE"_sd);
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

    if (cert == NULL) {
        DWORD gle = GetLastError();
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "CertCreateCertificateContext failed to decode cert: "
                                    << errnoWithDescription(gle));
    }

    UniqueCertificate tempCertHolder(cert);

    HCERTSTORE store = CertOpenStore(
        CERT_STORE_PROV_MEMORY, 0, NULL, CERT_STORE_DEFER_CLOSE_UNTIL_LAST_FREE_FLAG, NULL);
    if (store == NULL) {
        DWORD gle = GetLastError();
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "CertOpenStore failed to create memory store: "
                                    << errnoWithDescription(gle));
    }

    UniqueCertStore storeHolder(store);

    // Add the newly created certificate to the memory store, this makes a copy
    BOOL ret = CertAddCertificateContextToStore(store, cert, CERT_STORE_ADD_NEW, NULL);

    if (!ret) {
        DWORD gle = GetLastError();
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "CertAddCertificateContextToStore Memory Failed  "
                                    << errnoWithDescription(gle));
    }

    // Get the certificate from the store so we attach the private key to the cert in the store
    cert = CertEnumCertificatesInStore(store, NULL);

    UniqueCertificate certHolder(cert);

    std::vector<uint8_t> privateKey;

    // PEM files can have either private key format
    // Also the private key can either come before or after the certificate
    auto swPrivateKeyBlob = findPEMBlob(buf, "RSA PRIVATE KEY"_sd);
    // We expect to find at least one certificate
    if (!swPrivateKeyBlob.isOK()) {
        // A "PRIVATE KEY" is actually a PKCS #8 PrivateKeyInfo ASN.1 type.
        swPrivateKeyBlob = findPEMBlob(buf, "PRIVATE KEY"_sd);
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
            std::string keyContainerName = str::stream() << serverGlobalParams.logpath
                                                         << counter.fetchAndAdd(1);
            wstr = toNativeString(keyContainerName.c_str());
        } else {
            auto us = UUID::gen().toString();
            wstr = toNativeString(us.c_str());
        }

        // Use a new key container for the key. We cannot use the default container since the
        // default
        // container is shared across processes owned by the same user.
        // Note: Server side Schannel requires CRYPT_VERIFYCONTEXT off
        ret = CryptAcquireContextW(
            &hProv, wstr.c_str(), MS_ENHANCED_PROV, PROV_RSA_FULL, CRYPT_NEWKEYSET | CRYPT_SILENT);
        if (!ret) {
            DWORD gle = GetLastError();

            if (gle == NTE_EXISTS) {

                ret = CryptAcquireContextW(
                    &hProv, wstr.c_str(), MS_ENHANCED_PROV, PROV_RSA_FULL, CRYPT_SILENT);
                if (!ret) {
                    DWORD gle = GetLastError();
                    return Status(ErrorCodes::InvalidSSLConfiguration,
                                  str::stream() << "CryptAcquireContextW failed "
                                                << errnoWithDescription(gle));
                }

            } else {
                return Status(ErrorCodes::InvalidSSLConfiguration,
                              str::stream() << "CryptAcquireContextW failed "
                                            << errnoWithDescription(gle));
            }
        }
    } else {
        // Use a transient key container for the key
        ret = CryptAcquireContextW(
            &hProv, NULL, MS_ENHANCED_PROV, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT | CRYPT_SILENT);
        if (!ret) {
            DWORD gle = GetLastError();
            return Status(ErrorCodes::InvalidSSLConfiguration,
                          str::stream() << "CryptAcquireContextW failed  "
                                        << errnoWithDescription(gle));
        }
    }
    UniqueCryptProvider cryptProvider(hProv);

    HCRYPTKEY hkey;
    ret = CryptImportKey(hProv, privateKey.data(), privateKey.size(), 0, 0, &hkey);
    if (!ret) {
        DWORD gle = GetLastError();
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "CryptImportKey failed  " << errnoWithDescription(gle));
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
            DWORD gle = GetLastError();
            return Status(ErrorCodes::InvalidSSLConfiguration,
                          str::stream() << "CertSetCertificateContextProperty Failed  "
                                        << errnoWithDescription(gle));
        }
    }

    // NOTE: This is used to set the certificate for client side SChannel
    ret = CertSetCertificateContextProperty(
        cert, CERT_KEY_PROV_HANDLE_PROP_ID, 0, (const void*)hProv);
    if (!ret) {
        DWORD gle = GetLastError();
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "CertSetCertificateContextProperty failed  "
                                    << errnoWithDescription(gle));
    }

    // Add the extra certificates into the same certificate store as the certificate
    addCertificatesToStore(certHolder->hCertStore, extraCertificates);

    return UniqueCertificateWithPrivateKey{std::move(certHolder), std::move(cryptProvider)};
}

Status readCAPEMFile(HCERTSTORE certStore, StringData fileName) {

    auto swBuf = readFile(fileName);
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

    auto swBuf = readFile(fileName);
    if (!swBuf.isOK()) {
        return swBuf.getStatus();
    }

    std::string buf = std::move(swBuf.getValue());

    // Search the buffer for the various strings that make up a PEM file
    size_t pos = 0;
    bool found_one = false;

    while (pos < buf.size()) {
        auto swBlob = findPEMBlob(buf, "X509 CRL"_sd, pos, pos != 0);

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
        if (crl == NULL) {
            DWORD gle = GetLastError();
            return Status(ErrorCodes::InvalidSSLConfiguration,
                          str::stream() << "CertCreateCRLContext failed to decode crl: "
                                        << errnoWithDescription(gle));
        }

        UniqueCRL crlHolder(crl);

        BOOL ret = CertAddCRLContextToStore(certStore, crl, CERT_STORE_ADD_NEW, NULL);

        if (!ret) {
            DWORD gle = GetLastError();
            return Status(ErrorCodes::InvalidSSLConfiguration,
                          str::stream() << "CertAddCRLContextToStore Failed  "
                                        << errnoWithDescription(gle));
        }
    }

    return Status::OK();
}

StatusWith<UniqueCertStore> readCertChains(StringData caFile, StringData crlFile) {
    UniqueCertStore certStore = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, NULL, 0, NULL);
    if (certStore == nullptr) {
        DWORD gle = GetLastError();
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "CertOpenStore Failed  " << errnoWithDescription(gle));
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
    if (store == NULL) {
        DWORD gle = GetLastError();
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "CertOpenStore failed to open store 'My' from '" << storeName
                                    << "': "
                                    << errnoWithDescription(gle));
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
        if (cert == NULL) {
            DWORD gle = GetLastError();
            return Status(
                ErrorCodes::InvalidSSLConfiguration,
                str::stream()
                    << "CertFindCertificateInStore failed to find cert with subject name '"
                    << selector.subject.c_str()
                    << "' in 'My' store in '"
                    << storeName
                    << "': "
                    << errnoWithDescription(gle));
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
        if (cert == NULL) {
            DWORD gle = GetLastError();
            return Status(ErrorCodes::InvalidSSLConfiguration,
                          str::stream()
                              << "CertFindCertificateInStore failed to find cert with thumbprint '"
                              << toHex(selector.thumbprint.data(), selector.thumbprint.size())
                              << "' in 'My' store in '"
                              << storeName
                              << "': "
                              << errnoWithDescription(gle));
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
    BOOL ret = CryptAcquireCertificatePrivateKey(swCert.getValue().get(),
                                                 CRYPT_ACQUIRE_ALLOW_NCRYPT_KEY_FLAG,
                                                 NULL,
                                                 &hCryptProv,
                                                 &dwKeySpec,
                                                 &freeProvider);
    if (!ret) {
        DWORD gle = GetLastError();
        if (gle == CRYPT_E_NO_KEY_PROPERTY) {
            return Status(ErrorCodes::InvalidSSLConfiguration,
                          str::stream()
                              << "Could not find private key attached to the selected certificate");
        } else if (gle == NTE_BAD_KEYSET) {
            return Status(ErrorCodes::InvalidSSLConfiguration,
                          str::stream() << "Could not read private key attached to the selected "
                                           "certificate, ensure it exists and check the private "
                                           "key permissions");
        } else {
            return Status(ErrorCodes::InvalidSSLConfiguration,
                          str::stream() << "CryptAcquireCertificatePrivateKey failed  "
                                        << errnoWithDescription(gle));
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

        _clientEngine.CAstore = std::move(swChain.getValue());
        _clientEngine.hasCRL = !params.sslCRLFile.empty();
    }

    const auto serverCAFile =
        params.sslClusterCAFile.empty() ? params.sslCAFile : params.sslClusterCAFile;
    if (!serverCAFile.empty()) {
        auto swChain = readCertChains(serverCAFile, params.sslCRLFile);
        if (!swChain.isOK()) {
            return swChain.getStatus();
        }

        _serverEngine.CAstore = std::move(swChain.getValue());
        _serverEngine.hasCRL = !params.sslCRLFile.empty();
    }

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
            warning() << "Mixing certs from the system certificate store and PEM files. This may "
                         "produced unexpected results.";
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
            | SCH_CRED_SNI_ENABLE_OCSP         // Enable OCSP
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
            | SCH_CRED_MEMORY_STORE_CERT        // Read intermediate certificates from memory store
                                                // associated with client certificate.
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

    if (!params.sslCipherConfig.empty()) {
        warning()
            << "sslCipherConfig parameter is not supported with Windows SChannel and is ignored.";
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

SSLConnectionInterface* SSLManagerWindows::connect(Socket* socket) {
    std::unique_ptr<SSLConnectionWindows> sslConn =
        stdx::make_unique<SSLConnectionWindows>(&_clientCred, socket, nullptr, 0);

    _handshake(sslConn.get(), true);
    return sslConn.release();
}

SSLConnectionInterface* SSLManagerWindows::accept(Socket* socket,
                                                  const char* initialBytes,
                                                  int len) {
    std::unique_ptr<SSLConnectionWindows> sslConn =
        stdx::make_unique<SSLConnectionWindows>(&_serverCred, socket, initialBytes, len);

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
    return ns100 / 1000;
}

// MongoDB wants RFC 2253 (LDAP) formatted DN names for auth purposes
StatusWith<SSLX509Name> getCertificateSubjectName(PCCERT_CONTEXT cert) {

    auto swBlob =
        decodeObject(X509_NAME, cert->pCertInfo->Subject.pbData, cert->pCertInfo->Subject.cbData);

    if (!swBlob.isOK()) {
        return swBlob.getStatus();
    }

    PCERT_NAME_INFO nameInfo = reinterpret_cast<PCERT_NAME_INFO>(swBlob.getValue().data());

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
            rdn.emplace_back(rdnAttribute.pszObjId, rdnAttribute.dwValueType, toUtf8String(wstr));
        }
        entries.push_back(std::move(rdn));
    }

    return SSLX509Name(std::move(entries));
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
            severe() << "The provided SSL certificate is expired or not yet valid.";
            fassertFailedNoTrace(50755);
        }

        *serverCertificateExpirationDate =
            Date_t::fromMillisSinceEpoch(FiletimeToEpocMillis(cert->pCertInfo->NotAfter));
    }

    return Status::OK();
}

SSLPeerInfo SSLManagerWindows::parseAndValidatePeerCertificateDeprecated(
    const SSLConnectionInterface* conn,
    const std::string& remoteHost,
    const HostAndPort& hostForLogging) {
    auto swPeerSubjectName = parseAndValidatePeerCertificate(
        const_cast<SSLConnectionWindows*>(static_cast<const SSLConnectionWindows*>(conn))
            ->_engine.native_handle(),
        remoteHost,
        hostForLogging);
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
            names.push_back(toUtf8String(altNames->rgAltEntry[i].pwszDNSName));
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

    PCCERT_CHAIN_CONTEXT chainContext;
    BOOL ret = CertGetCertificateChain(certChainEngine,
                                       cert,
                                       NULL,
                                       cert->hCertStore,
                                       &certChainPara,
                                       CERT_CHAIN_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT,
                                       NULL,
                                       &chainContext);
    if (!ret) {
        DWORD gle = GetLastError();
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "CertGetCertificateChain failed: "
                                    << errnoWithDescription(gle));
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

    CERT_CHAIN_POLICY_STATUS certChainPolicyStatus;
    memset(&certChainPolicyStatus, 0, sizeof(certChainPolicyStatus));
    certChainPolicyStatus.cbSize = sizeof(certChainPolicyStatus);

    ret = CertVerifyCertificateChainPolicy(
        CERT_CHAIN_POLICY_SSL, certChainHolder.get(), &chain_policy_para, &certChainPolicyStatus);

    // This means something really went wrong, this should not happen.
    if (!ret) {
        DWORD gle = GetLastError();
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "CertVerifyCertificateChainPolicy failed: "
                                    << errnoWithDescription(gle));
    }

    auto swSubjectName = getCertificateSubjectName(cert);
    if (!swSubjectName.isOK()) {
        return swSubjectName.getStatus();
    }
    invariant(peerSubjectName);
    *peerSubjectName = swSubjectName.getValue();

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

            ret = CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL,
                                                   certChainHolder.get(),
                                                   &chain_policy_para,
                                                   &certChainPolicyStatus);

            // This means something really went wrong, this should not happen.
            if (!ret) {
                DWORD gle = GetLastError();
                return Status(ErrorCodes::InvalidSSLConfiguration,
                              str::stream() << "CertVerifyCertificateChainPolicy2 failed: "
                                            << errnoWithDescription(gle));
            }

            if (certChainPolicyStatus.dwError == S_OK ||
                certChainPolicyStatus.dwError == CRYPT_E_NO_REVOCATION_CHECK) {
                onlyCNError = true;
            }
        }

        // We need to check if the user has a cert where SANs have ip addresses label as DNS Name
        // but only if a CN mismatch is the only error
        if (onlyCNError || allowInvalidCertificates) {

            // Give the user a hint why the certificate validation failed.
            StringBuilder certificateNames;
            auto swAltNames = getSubjectAlternativeNames(cert);
            if (swAltNames.isOK() && !swAltNames.getValue().empty()) {
                for (auto& name : swAltNames.getValue()) {
                    certificateNames << name << " ";
                }
            };

            certificateNames << ", Subject Name: " << *peerSubjectName;

            str::stream msg;
            msg << "The server certificate does not match the host name. Hostname: " << remoteHost
                << " does not match " << certificateNames.str();

            if (allowInvalidCertificates) {
                warning() << "SSL peer certificate validation failed ("
                          << integerToHex(certChainPolicyStatus.dwError)
                          << "): " << errnoWithDescription(certChainPolicyStatus.dwError);
                warning() << msg.ss.str();
                *peerSubjectName = SSLX509Name();
                return Status::OK();
            } else if (allowInvalidHostnames) {
                warning() << msg.ss.str();
                return Status::OK();
            } else {
                return Status(ErrorCodes::SSLHandshakeFailed, msg);
            }
        } else {
            str::stream msg;
            msg << "SSL peer certificate validation failed: ("
                << integerToHex(certChainPolicyStatus.dwError) << ")"
                << errnoWithDescription(certChainPolicyStatus.dwError);
            error() << msg.ss.str();
            return Status(ErrorCodes::SSLHandshakeFailed, msg);
        }
    }

    return Status::OK();
}

StatusWith<TLSVersion> mapTLSVersion(PCtxtHandle ssl) {
    SecPkgContext_ConnectionInfo connInfo;

    SECURITY_STATUS ss = QueryContextAttributes(ssl, SECPKG_ATTR_CONNECTION_INFO, &connInfo);

    if (ss != SEC_E_OK) {
        return Status(ErrorCodes::SSLHandshakeFailed,
                      str::stream() << "QueryContextAttributes for connection info failed with"
                                    << ss);
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

StatusWith<SSLPeerInfo> SSLManagerWindows::parseAndValidatePeerCertificate(
    PCtxtHandle ssl, const std::string& remoteHost, const HostAndPort& hostForLogging) {
    auto sniName = getSNIServerName_impl();
    invariant(!sslGlobalParams.tlsCATrusts);

    PCCERT_CONTEXT cert;

    auto tlsVersionStatus = mapTLSVersion(ssl);
    if (!tlsVersionStatus.isOK()) {
        return tlsVersionStatus.getStatus();
    }

    recordTLSVersion(tlsVersionStatus.getValue(), hostForLogging);

    if (!_sslConfiguration.hasCA && isSSLServer)
        return SSLPeerInfo(sniName);

    SECURITY_STATUS ss = QueryContextAttributes(ssl, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &cert);

    if (ss == SEC_E_NO_CREDENTIALS) {  // no certificate presented by peer
        if (_weakValidation) {
            // do not give warning if "no certificate" warnings are suppressed
            if (!_suppressNoCertificateWarning) {
                warning() << "no SSL certificate provided by peer";
            }
            return SSLPeerInfo(sniName);
        } else {
            auto msg = "no SSL certificate provided by peer; connection rejected";
            error() << msg;
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
        return SSLPeerInfo(sniName);
    }

    LOG(2) << "Accepted TLS connection from peer: " << peerSubjectName;

    // On the server side, parse the certificate for roles
    if (remoteHost.empty()) {
        StatusWith<stdx::unordered_set<RoleName>> swPeerCertificateRoles = parsePeerRoles(cert);
        if (!swPeerCertificateRoles.isOK()) {
            return swPeerCertificateRoles.getStatus();
        }

        return SSLPeerInfo(peerSubjectName, sniName, std::move(swPeerCertificateRoles.getValue()));
    } else {
        return SSLPeerInfo(peerSubjectName);
    }
}

}  // namespace
}  // namespace mongo
