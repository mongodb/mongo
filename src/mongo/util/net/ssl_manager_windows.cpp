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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/util/net/ssl_manager.h"

#include <asio.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stack>
#include <string>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/base/initializer_context.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/memory.h"
#include "mongo/transport/session.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/private/ssl_expiration.h"
#include "mongo/util/net/sock.h"
#include "mongo/util/net/socket_exception.h"
#include "mongo/util/net/ssl.hpp"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/net/ssl_types.h"
#include "mongo/util/scopeguard.h"
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

}  // namespace

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
        // TODO
        return "";
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
                                                          const std::string& remoteHost) final;

    StatusWith<boost::optional<SSLPeerInfo>> parseAndValidatePeerCertificate(
        PCtxtHandle ssl, const std::string& remoteHost) final;


    const SSLConfiguration& getSSLConfiguration() const final {
        return _sslConfiguration;
    }

    int SSL_read(SSLConnectionInterface* conn, void* buf, int num) final;

    int SSL_write(SSLConnectionInterface* conn, const void* buf, int num) final;

    int SSL_shutdown(SSLConnectionInterface* conn) final;

private:
    bool _weakValidation;
    bool _allowInvalidCertificates;
    bool _allowInvalidHostnames;
    SSLConfiguration _sslConfiguration;

    SCHANNEL_CRED _clientCred;
    SCHANNEL_CRED _serverCred;

    UniqueCertificate _pemCertificate;
    UniqueCertificate _clusterPEMCertificate;
    PCCERT_CONTEXT _clientCertificates[1];
    PCCERT_CONTEXT _serverCertificates[1];

    Status loadCertificates(const SSLParams& params);

    void handshake(SSLConnectionWindows* conn, bool client);
};

// Global variable indicating if this is a server or a client instance
bool isSSLServer = false;

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
    : _cred(cred), socket(sock), _engine(_cred) {

    _tempBuffer.resize(17 * 1024);

    if (len > 0) {
        _engine.put_input(asio::const_buffer(initialBytes, len));
    }
}

SSLConnectionWindows::~SSLConnectionWindows() {}

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

SSLManagerWindows::SSLManagerWindows(const SSLParams& params, bool isServer)
    : _weakValidation(params.sslWeakCertificateValidation),
      _allowInvalidCertificates(params.sslAllowInvalidCertificates),
      _allowInvalidHostnames(params.sslAllowInvalidHostnames) {

    uassertStatusOK(loadCertificates(params));

    uassertStatusOK(initSSLContext(&_clientCred, params, ConnectionDirection::kOutgoing));

    // TODO: validate client certificate

    // SSL server specific initialization
    if (isServer) {
        uassertStatusOK(initSSLContext(&_serverCred, params, ConnectionDirection::kIncoming));

        // TODO: validate server certificate
    }
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
                int ret =
                    recv(conn->socket->rawFD(), reinterpret_cast<char*>(buf), num, portRecvFlags);
                if (ret == SOCKET_ERROR) {
                    conn->socket->handleRecvError(ret, num);
                }

                conn->_engine.put_input(asio::const_buffer(buf, ret));

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
    invariant(false);
    return 0;
}

StatusWith<UniqueCertificate> readPEMFile(StringData fileName, StringData password) {

    std::ifstream pemFile(fileName.toString(), std::ios::binary);
    if (!pemFile.is_open()) {
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "Failed to open PEM file: " << fileName);
    }

    std::string buf((std::istreambuf_iterator<char>(pemFile)), std::istreambuf_iterator<char>());

    pemFile.close();

    // Search the buffer for the various strings that make up a PEM file
    size_t publicKey = buf.find("-----BEGIN CERTIFICATE-----");
    if (publicKey == std::string::npos) {
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "Failed to find Certifiate in: " << fileName);
    }

    // TODO: decode encrypted pem
    // StringData encryptedPrivateKey = buf.find("-----BEGIN ENCRYPTED PRIVATE KEY-----");

    // TODO: check if we need both
    size_t privateKey = buf.find("-----BEGIN RSA PRIVATE KEY-----");
    if (privateKey == std::string::npos) {
        privateKey = buf.find("-----BEGIN PRIVATE KEY-----");
    }

    if (privateKey == std::string::npos) {
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "Failed to find privateKey in: " << fileName);
    }

    CERT_BLOB certBlob;
    certBlob.cbData = buf.size() - publicKey;
    certBlob.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(buf.data() + publicKey));

    PCCERT_CONTEXT cert;
    BOOL ret = CryptQueryObject(CERT_QUERY_OBJECT_BLOB,
                                &certBlob,
                                CERT_QUERY_CONTENT_FLAG_ALL,
                                CERT_QUERY_FORMAT_FLAG_ALL,
                                NULL,
                                NULL,
                                NULL,
                                NULL,
                                NULL,
                                NULL,
                                reinterpret_cast<const void**>(&cert));
    if (!ret) {
        DWORD gle = GetLastError();
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "CryptQueryObject failed to get cert: "
                                    << errnoWithDescription(gle));
    }

    UniqueCertificate certHolder(cert);
    DWORD privateKeyLen{0};

    ret = CryptStringToBinaryA(buf.c_str() + privateKey,
                               0,  // null terminated string
                               CRYPT_STRING_BASE64HEADER | CRYPT_STRING_STRICT,
                               NULL,
                               &privateKeyLen,
                               NULL,
                               NULL);
    if (!ret) {
        DWORD gle = GetLastError();
        if (gle != ERROR_MORE_DATA) {
            return Status(ErrorCodes::InvalidSSLConfiguration,
                          str::stream() << "CryptStringToBinary failed to get size of key: "
                                        << errnoWithDescription(gle));
        }
    }

    std::unique_ptr<BYTE[]> privateKeyBuf = std::make_unique<BYTE[]>(privateKeyLen);
    ret = CryptStringToBinaryA(buf.c_str() + privateKey,
                               0,  // null terminated string
                               CRYPT_STRING_BASE64HEADER | CRYPT_STRING_STRICT,
                               privateKeyBuf.get(),
                               &privateKeyLen,
                               NULL,
                               NULL);
    if (!ret) {
        DWORD gle = GetLastError();
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "CryptStringToBinary failed to read key: "
                                    << errnoWithDescription(gle));
    }


    DWORD privateBlobLen{0};

    ret = CryptDecodeObjectEx(X509_ASN_ENCODING,
                              PKCS_RSA_PRIVATE_KEY,
                              privateKeyBuf.get(),
                              privateKeyLen,
                              CRYPT_DECODE_SHARE_OID_STRING_FLAG,
                              NULL,
                              NULL,
                              &privateBlobLen);
    if (!ret) {
        DWORD gle = GetLastError();
        if (gle != ERROR_MORE_DATA) {
            return Status(ErrorCodes::InvalidSSLConfiguration,
                          str::stream() << "CryptDecodeObjectEx failed to get size of key: "
                                        << errnoWithDescription(gle));
        }
    }

    std::unique_ptr<BYTE[]> privateBlobBuf = std::make_unique<BYTE[]>(privateBlobLen);

    ret = CryptDecodeObjectEx(X509_ASN_ENCODING,
                              PKCS_RSA_PRIVATE_KEY,
                              privateKeyBuf.get(),
                              privateKeyLen,
                              CRYPT_DECODE_SHARE_OID_STRING_FLAG,
                              NULL,
                              privateBlobBuf.get(),
                              &privateBlobLen);
    if (!ret) {
        DWORD gle = GetLastError();
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "CryptDecodeObjectEx failed to read key: "
                                    << errnoWithDescription(gle));
    }

    HCRYPTPROV hProv;
    std::wstring wstr;

    // Create the right Crypto context depending on whether we running in a server or outside.
    // See https://msdn.microsoft.com/en-us/library/windows/desktop/aa375195(v=vs.85).aspx
    if (isSSLServer) {
        // Generate a unique name for our key container
        // Use the the log file if possible
        if (!serverGlobalParams.logpath.empty()) {
            wstr = toNativeString(serverGlobalParams.logpath.c_str());
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
    ret = CryptImportKey(hProv, privateBlobBuf.get(), privateBlobLen, 0, 0, &hkey);
    if (!ret) {
        DWORD gle = GetLastError();
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "CryptImportKey failed  " << errnoWithDescription(gle));
    }
    UniqueCryptKey(hKey);

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

    return std::move(certHolder);
}

Status SSLManagerWindows::loadCertificates(const SSLParams& params) {
    _clientCertificates[0] = nullptr;
    _serverCertificates[0] = nullptr;

    // Load the normal PEM file
    if (!params.sslPEMKeyFile.empty()) {
        auto swCertificate = readPEMFile(params.sslPEMKeyFile, params.sslPEMKeyPassword);
        if (!swCertificate.isOK()) {
            return swCertificate.getStatus();
        }

        _pemCertificate = std::move(swCertificate.getValue());
    }

    // Load the cluster PEM file, only applies to server side code
    if (!params.sslClusterFile.empty()) {
        auto swCertificate = readPEMFile(params.sslClusterFile, params.sslClusterPassword);
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

        cred->dwFlags = cred->dwFlags          // Flags
            | SCH_CRED_REVOCATION_CHECK_CHAIN  // Check certificate revocation
            | SCH_CRED_SNI_CREDENTIAL          // Pass along SNI creds
            | SCH_CRED_SNI_ENABLE_OCSP         // Enable OCSP
            | SCH_CRED_NO_SYSTEM_MAPPER        // Do not map certificate to user account
            | SCH_CRED_DISABLE_RECONNECTS;     // Do not support reconnects
    } else {
        supportedProtocols = SP_PROT_TLS1_CLIENT | SP_PROT_TLS1_0_CLIENT | SP_PROT_TLS1_1_CLIENT |
            SP_PROT_TLS1_2_CLIENT;

        cred->dwFlags = cred->dwFlags           // Flags
            | SCH_CRED_REVOCATION_CHECK_CHAIN   // Check certificate revocation
            | SCH_CRED_NO_SERVERNAME_CHECK      // Do not validate server name against cert
            | SCH_CRED_NO_DEFAULT_CREDS         // No Default Certificate
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

    if (!params.sslCipherConfig.empty()) {
        warning()
            << "sslCipherConfig parameter is not supported with Windows SChannel and is ignored.";
    }

    if (direction == ConnectionDirection::kOutgoing) {
        if (_clientCertificates[0]) {
            cred->cCreds = 1;
            cred->paCred = _clientCertificates;
        }
    } else {
        cred->cCreds = 1;
        cred->paCred = _serverCertificates;
    }

    return Status::OK();
}

SSLConnectionInterface* SSLManagerWindows::connect(Socket* socket) {
    std::unique_ptr<SSLConnectionWindows> sslConn =
        stdx::make_unique<SSLConnectionWindows>(&_clientCred, socket, nullptr, 0);

    handshake(sslConn.get(), true);
    return sslConn.release();
}

SSLConnectionInterface* SSLManagerWindows::accept(Socket* socket,
                                                  const char* initialBytes,
                                                  int len) {
    std::unique_ptr<SSLConnectionWindows> sslConn =
        stdx::make_unique<SSLConnectionWindows>(&_serverCred, socket, initialBytes, len);

    handshake(sslConn.get(), false);

    return sslConn.release();
}

void SSLManagerWindows::handshake(SSLConnectionWindows* conn, bool client) {
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

SSLPeerInfo SSLManagerWindows::parseAndValidatePeerCertificateDeprecated(
    const SSLConnectionInterface* conn, const std::string& remoteHost) {
    auto swPeerSubjectName = parseAndValidatePeerCertificate(
        const_cast<SSLConnectionWindows*>(static_cast<const SSLConnectionWindows*>(conn))
            ->_engine.native_handle(),
        remoteHost);
    // We can't use uassertStatusOK here because we need to throw a SocketException.
    if (!swPeerSubjectName.isOK()) {
        throwSocketError(SocketErrorKind::CONNECT_ERROR, swPeerSubjectName.getStatus().reason());
    }

    return swPeerSubjectName.getValue().get_value_or(SSLPeerInfo());
}

StatusWith<boost::optional<SSLPeerInfo>> SSLManagerWindows::parseAndValidatePeerCertificate(
    PCtxtHandle ssl, const std::string& remoteHost) {

    return {boost::none};
}

}  // namespace mongo
