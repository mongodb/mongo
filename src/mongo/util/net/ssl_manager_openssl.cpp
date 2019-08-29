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

#include <boost/algorithm/string.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stack>
#include <string>
#include <vector>

#include "mongo/base/checked_cast.h"
#include "mongo/base/init.h"
#include "mongo/base/secure_allocator.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/transport/session.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/net/cidr.h"
#include "mongo/util/net/dh_openssl.h"
#include "mongo/util/net/private/ssl_expiration.h"
#include "mongo/util/net/socket_exception.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/net/ssl_parameters_gen.h"
#include "mongo/util/net/ssl_types.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/text.h"

#ifndef _WIN32
#include <netinet/in.h>
#endif
#include <openssl/asn1.h>
#include <openssl/asn1t.h>
#include <openssl/dh.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>
#ifdef MONGO_CONFIG_HAVE_SSL_EC_KEY_NEW
#include <openssl/ec.h>
#endif
#if defined(_WIN32)
#include <wincrypt.h>
#elif defined(__APPLE__)
#include <Security/Security.h>
#endif

namespace mongo {

namespace {

// Modulus for Diffie-Hellman parameter 'ffdhe3072' defined in RFC 7919
constexpr std::array<std::uint8_t, 384> ffdhe3072_p = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xAD, 0xF8, 0x54, 0x58, 0xA2, 0xBB, 0x4A, 0x9A,
    0xAF, 0xDC, 0x56, 0x20, 0x27, 0x3D, 0x3C, 0xF1, 0xD8, 0xB9, 0xC5, 0x83, 0xCE, 0x2D, 0x36, 0x95,
    0xA9, 0xE1, 0x36, 0x41, 0x14, 0x64, 0x33, 0xFB, 0xCC, 0x93, 0x9D, 0xCE, 0x24, 0x9B, 0x3E, 0xF9,
    0x7D, 0x2F, 0xE3, 0x63, 0x63, 0x0C, 0x75, 0xD8, 0xF6, 0x81, 0xB2, 0x02, 0xAE, 0xC4, 0x61, 0x7A,
    0xD3, 0xDF, 0x1E, 0xD5, 0xD5, 0xFD, 0x65, 0x61, 0x24, 0x33, 0xF5, 0x1F, 0x5F, 0x06, 0x6E, 0xD0,
    0x85, 0x63, 0x65, 0x55, 0x3D, 0xED, 0x1A, 0xF3, 0xB5, 0x57, 0x13, 0x5E, 0x7F, 0x57, 0xC9, 0x35,
    0x98, 0x4F, 0x0C, 0x70, 0xE0, 0xE6, 0x8B, 0x77, 0xE2, 0xA6, 0x89, 0xDA, 0xF3, 0xEF, 0xE8, 0x72,
    0x1D, 0xF1, 0x58, 0xA1, 0x36, 0xAD, 0xE7, 0x35, 0x30, 0xAC, 0xCA, 0x4F, 0x48, 0x3A, 0x79, 0x7A,
    0xBC, 0x0A, 0xB1, 0x82, 0xB3, 0x24, 0xFB, 0x61, 0xD1, 0x08, 0xA9, 0x4B, 0xB2, 0xC8, 0xE3, 0xFB,
    0xB9, 0x6A, 0xDA, 0xB7, 0x60, 0xD7, 0xF4, 0x68, 0x1D, 0x4F, 0x42, 0xA3, 0xDE, 0x39, 0x4D, 0xF4,
    0xAE, 0x56, 0xED, 0xE7, 0x63, 0x72, 0xBB, 0x19, 0x0B, 0x07, 0xA7, 0xC8, 0xEE, 0x0A, 0x6D, 0x70,
    0x9E, 0x02, 0xFC, 0xE1, 0xCD, 0xF7, 0xE2, 0xEC, 0xC0, 0x34, 0x04, 0xCD, 0x28, 0x34, 0x2F, 0x61,
    0x91, 0x72, 0xFE, 0x9C, 0xE9, 0x85, 0x83, 0xFF, 0x8E, 0x4F, 0x12, 0x32, 0xEE, 0xF2, 0x81, 0x83,
    0xC3, 0xFE, 0x3B, 0x1B, 0x4C, 0x6F, 0xAD, 0x73, 0x3B, 0xB5, 0xFC, 0xBC, 0x2E, 0xC2, 0x20, 0x05,
    0xC5, 0x8E, 0xF1, 0x83, 0x7D, 0x16, 0x83, 0xB2, 0xC6, 0xF3, 0x4A, 0x26, 0xC1, 0xB2, 0xEF, 0xFA,
    0x88, 0x6B, 0x42, 0x38, 0x61, 0x1F, 0xCF, 0xDC, 0xDE, 0x35, 0x5B, 0x3B, 0x65, 0x19, 0x03, 0x5B,
    0xBC, 0x34, 0xF4, 0xDE, 0xF9, 0x9C, 0x02, 0x38, 0x61, 0xB4, 0x6F, 0xC9, 0xD6, 0xE6, 0xC9, 0x07,
    0x7A, 0xD9, 0x1D, 0x26, 0x91, 0xF7, 0xF7, 0xEE, 0x59, 0x8C, 0xB0, 0xFA, 0xC1, 0x86, 0xD9, 0x1C,
    0xAE, 0xFE, 0x13, 0x09, 0x85, 0x13, 0x92, 0x70, 0xB4, 0x13, 0x0C, 0x93, 0xBC, 0x43, 0x79, 0x44,
    0xF4, 0xFD, 0x44, 0x52, 0xE2, 0xD7, 0x4D, 0xD3, 0x64, 0xF2, 0xE2, 0x1E, 0x71, 0xF5, 0x4B, 0xFF,
    0x5C, 0xAE, 0x82, 0xAB, 0x9C, 0x9D, 0xF6, 0x9E, 0xE8, 0x6D, 0x2B, 0xC5, 0x22, 0x36, 0x3A, 0x0D,
    0xAB, 0xC5, 0x21, 0x97, 0x9B, 0x0D, 0xEA, 0xDA, 0x1D, 0xBF, 0x9A, 0x42, 0xD5, 0xC4, 0x48, 0x4E,
    0x0A, 0xBC, 0xD0, 0x6B, 0xFA, 0x53, 0xDD, 0xEF, 0x3C, 0x1B, 0x20, 0xEE, 0x3F, 0xD5, 0x9D, 0x7C,
    0x25, 0xE4, 0x1D, 0x2B, 0x66, 0xC6, 0x2E, 0x37, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Generator for Diffie-Hellman parameter 'ffdhe3072' defined in RFC 7919 (2)
constexpr std::uint8_t ffdhe3072_g = 0x02;

// Because the hostname having a slash is used by `mongo::SockAddr` to determine if a hostname is a
// Unix Domain Socket endpoint, this function uses the same logic.  (See
// `mongo::SockAddr::Sockaddr(StringData, int, sa_family_t)`).  A user explicitly specifying a Unix
// Domain Socket in the present working directory, through a code path which supplies `sa_family_t`
// as `AF_UNIX` will cause this code to lie.  This will, in turn, cause the
// `SSLManagerInterface::parseAndValidatePeerCertificate` code to believe a socket is a host, which
// will then cause a connection failure if and only if that domain socket also has a certificate for
// SSL and the connection is an SSL connection.
bool isUnixDomainSocket(const std::string& hostname) {
    return end(hostname) != std::find(begin(hostname), end(hostname), '/');
}

using UniqueBIO = std::unique_ptr<BIO, OpenSSLDeleter<decltype(::BIO_free), ::BIO_free>>;

#ifdef MONGO_CONFIG_HAVE_SSL_EC_KEY_NEW
using UniqueEC_KEY =
    std::unique_ptr<EC_KEY, OpenSSLDeleter<decltype(::EC_KEY_free), ::EC_KEY_free>>;
#endif

using UniqueBIGNUM = std::unique_ptr<BIGNUM, OpenSSLDeleter<decltype(::BN_free), ::BN_free>>;

UniqueBIO makeUniqueMemBio(std::vector<std::uint8_t>& v) {
    UniqueBIO rv(::BIO_new_mem_buf(v.data(), v.size()));
    if (!rv) {
        class ssl_bad_alloc : public std::bad_alloc {
        private:
            std::string message;

        public:
            explicit ssl_bad_alloc(std::string m) : message(std::move(m)) {}

            const char* what() const noexcept override {
                return message.c_str();
            }
        };
        throw ssl_bad_alloc(str::stream()
                            << "Error allocating SSL BIO: "
                            << SSLManagerInterface::getSSLErrorMessage(ERR_get_error()));
    }
    return rv;
}

// Attempts to set a hard coded curve (prime256v1) for ECDHE if the version of OpenSSL supports it.
bool useDefaultECKey(SSL_CTX* const ctx) {
#ifdef MONGO_CONFIG_HAVE_SSL_EC_KEY_NEW
    UniqueEC_KEY key(EC_KEY_new_by_curve_name(NID_X9_62_prime256v1));

    if (key) {
        return SSL_CTX_set_tmp_ecdh(ctx, key.get()) == 1;
    }
#endif
    return false;
}

// If the underlying SSL supports auto-configuration of ECDH parameters, this function will select
// it. If not, this function will attempt to use a hard-coded but widely supported elliptic curve.
// If that fails, ECDHE will not be enabled.
bool enableECDHE(SSL_CTX* const ctx) {
#ifdef MONGO_CONFIG_HAVE_SSL_SET_ECDH_AUTO
    SSL_CTX_set_ecdh_auto(ctx, true);
#elif OPENSSL_VERSION_NUMBER < 0x10100000L || \
    (defined(LIBRESSL_VERSION_NUMBER) && LIBRESSL_VERSION_NUMBER < 0x2070000fL)
    // SSL_CTRL_SET_ECDH_AUTO is defined to be 94 in OpenSSL 1.0.2. On RHEL 7, Mongo could be built
    // against 1.0.1 but actually linked with 1.0.2 at runtime. The define may not be present, but
    // this call could actually enable auto ecdh. We also ensure the OpenSSL version is sufficiently
    // old to protect against future versions where SSL_CTX_set_ecdh_auto could be removed and 94
    // ctrl code could be repurposed.
    if (SSL_CTX_ctrl(ctx, 94, 1, nullptr) != 1) {
        // If manually setting the configuration option failed, use a hard coded curve
        if (!useDefaultECKey(ctx)) {
            warning() << "Failed to enable ECDHE due to a lack of support from system libraries.";
            return false;
        }
    }
#endif
    std::ignore = ctx;
    return true;
}


// Old copies of OpenSSL will not have constants to disable protocols they don't support.
// Define them to values we can OR together safely to generically disable these protocols across
// all versions of OpenSSL.
#ifndef SSL_OP_NO_TLSv1_1
#define SSL_OP_NO_TLSv1_1 0
#endif
#ifndef SSL_OP_NO_TLSv1_2
#define SSL_OP_NO_TLSv1_2 0
#endif
#ifndef SSL_OP_NO_TLSv1_3
#define SSL_OP_NO_TLSv1_3 0
#endif

// clang-format off
#ifndef MONGO_CONFIG_HAVE_ASN1_ANY_DEFINITIONS
// Copies of OpenSSL before 1.0.0 do not have ASN1_SEQUENCE_ANY, ASN1_SET_ANY, or the helper
// functions which let us deserialize these objects. We must polyfill the definitions to interact
// with ASN1 objects so stored.
typedef STACK_OF(ASN1_TYPE) ASN1_SEQUENCE_ANY;

ASN1_ITEM_TEMPLATE(ASN1_SEQUENCE_ANY) =
    ASN1_EX_TEMPLATE_TYPE(ASN1_TFLG_SEQUENCE_OF, 0, ASN1_SEQUENCE_ANY, ASN1_ANY)
ASN1_ITEM_TEMPLATE_END(ASN1_SEQUENCE_ANY)

ASN1_ITEM_TEMPLATE(ASN1_SET_ANY) =
    ASN1_EX_TEMPLATE_TYPE(ASN1_TFLG_SET_OF, 0, ASN1_SET_ANY, ASN1_ANY)
ASN1_ITEM_TEMPLATE_END(ASN1_SET_ANY)

IMPLEMENT_ASN1_ENCODE_FUNCTIONS_const_fname(ASN1_SEQUENCE_ANY, ASN1_SEQUENCE_ANY,
                                            ASN1_SEQUENCE_ANY)
IMPLEMENT_ASN1_ENCODE_FUNCTIONS_const_fname(ASN1_SEQUENCE_ANY, ASN1_SET_ANY, ASN1_SET_ANY)
; // clang format needs to see a semicolon or it will start formatting unrelated code
#endif // MONGO_CONFIG_NEEDS_ASN1_ANY_DEFINITIONS
// clang-format on

#if OPENSSL_VERSION_NUMBER < 0x10100000L || \
    (defined(LIBRESSL_VERSION_NUMBER) && LIBRESSL_VERSION_NUMBER < 0x2070000fL)
// Copies of OpenSSL after 1.1.0 define new functions for interaction with
// X509 and DH structures. We must polyfill used definitions to interact with older OpenSSL
// versions.
const STACK_OF(X509_EXTENSION) * X509_get0_extensions(const X509* peerCert) {
    return peerCert->cert_info->extensions;
}

inline ASN1_TIME* X509_get0_notAfter(const X509* cert) {
    return X509_get_notAfter(cert);
}

inline int X509_NAME_ENTRY_set(const X509_NAME_ENTRY* ne) {
    return ne->set;
}

#if OPENSSL_VESION_NUMBER < 0x10002000L
inline bool ASN1_TIME_diff(int*, int*, const ASN1_TIME*, const ASN1_TIME*) {
    return false;
}
#endif

int DH_set0_pqg(DH* dh, BIGNUM* p, BIGNUM* q, BIGNUM* g) {
    dh->p = p;
    dh->g = g;

    return 1;
}

void DH_get0_pqg(const DH* dh, const BIGNUM** p, const BIGNUM** q, const BIGNUM** g) {
    if (p) {
        *p = dh->p;
    }

    if (g) {
        *g = dh->g;
    }
}
#endif

/**
 * Multithreaded Support for SSL.
 *
 * In order to allow OpenSSL to work in a multithreaded environment, you
 * may need to provide some callbacks for it to use for locking. The following code
 * sets up a vector of mutexes and provides a thread unique ID number.
 * The so-called SSLThreadInfo class encapsulates most of the logic required for
 * OpenSSL multithreaded support.
 *
 * OpenSSL before version 1.1.0 requires applications provide a callback which emits a thread
 * identifier. This ID is used to store thread specific ERR information. When a thread is
 * terminated, it must call ERR_remove_state or ERR_remove_thread_state. These functions may
 * themselves invoke the application provided callback. These IDs are stored in a hashtable with
 * a questionable hash function. They must be uniformly distributed to prevent collisions.
 */
class SSLThreadInfo {
public:
    static unsigned long getID() {
        struct CallErrRemoveState {
            explicit CallErrRemoveState(ThreadIDManager& manager, unsigned long id)
                : _manager(manager), id(id) {}

            ~CallErrRemoveState() {
                ERR_remove_state(0);
                _manager.releaseID(id);
            };

            ThreadIDManager& _manager;
            unsigned long id;
        };

        // NOTE: This logic is fully intentional. Because ERR_remove_state (called within
        // the destructor of the kRemoveStateFromThread object) re-enters this function,
        // we must have a two phase protection, otherwise we would access a thread local
        // during its destruction.
        static thread_local boost::optional<CallErrRemoveState> threadLocalState;
        if (!threadLocalState) {
            threadLocalState.emplace(_idManager, _idManager.reserveID());
        }

        return threadLocalState->id;
    }

    static void lockingCallback(int mode, int type, const char* file, int line) {
        if (mode & CRYPTO_LOCK) {
            _mutex[type]->lock();
        } else {
            _mutex[type]->unlock();
        }
    }

    static void init() {
        CRYPTO_set_id_callback(&SSLThreadInfo::getID);
        CRYPTO_set_locking_callback(&SSLThreadInfo::lockingCallback);

        while ((int)_mutex.size() < CRYPTO_num_locks()) {
            _mutex.emplace_back(std::make_unique<stdx::recursive_mutex>());
        }
    }

private:
    SSLThreadInfo() = delete;

    // Note: see SERVER-8734 for why we are using a recursive mutex here.
    // Once the deadlock fix in OpenSSL is incorporated into most distros of
    // Linux, this can be changed back to a nonrecursive mutex.
    static std::vector<std::unique_ptr<stdx::recursive_mutex>> _mutex;

    class ThreadIDManager {
    public:
        unsigned long reserveID() {
            stdx::unique_lock<stdx::mutex> lock(_idMutex);
            if (!_idLast.empty()) {
                unsigned long ret = _idLast.top();
                _idLast.pop();
                return ret;
            }
            return ++_idNext;
        }

        void releaseID(unsigned long id) {
            stdx::unique_lock<stdx::mutex> lock(_idMutex);
            _idLast.push(id);
        }

    private:
        // Machinery for producing IDs that are unique for the life of a thread.
        stdx::mutex _idMutex;       // Protects _idNext and _idLast.
        unsigned long _idNext = 0;  // Stores the next thread ID to use, if none already allocated.
        std::stack<unsigned long, std::vector<unsigned long>>
            _idLast;  // Stores old thread IDs, for reuse.
    };
    static ThreadIDManager _idManager;
};
std::vector<std::unique_ptr<stdx::recursive_mutex>> SSLThreadInfo::_mutex;
SSLThreadInfo::ThreadIDManager SSLThreadInfo::_idManager;

boost::optional<std::string> getRawSNIServerName(const SSL* const ssl) {
    const char* name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    if (!name) {
        return boost::none;
    }
    return std::string(name);
}

class SSLConnectionOpenSSL : public SSLConnectionInterface {
public:
    SSL* ssl;
    BIO* networkBIO;
    BIO* internalBIO;
    Socket* socket;

    SSLConnectionOpenSSL(SSL_CTX* ctx, Socket* sock, const char* initialBytes, int len);

    ~SSLConnectionOpenSSL();

    std::string getSNIServerName() const final {
        return getRawSNIServerName(ssl).value_or("");
    }
};

////////////////////////////////////////////////////////////////

using UniqueSSLContext =
    std::unique_ptr<SSL_CTX, OpenSSLDeleter<decltype(::SSL_CTX_free), ::SSL_CTX_free>>;
static const int BUFFER_SIZE = 8 * 1024;
static const int DATE_LEN = 128;

using UniqueX509 = std::unique_ptr<X509, OpenSSLDeleter<decltype(X509_free), ::X509_free>>;

class SSLManagerOpenSSL : public SSLManagerInterface {
public:
    explicit SSLManagerOpenSSL(const SSLParams& params, bool isServer);

    /**
     * Initializes an OpenSSL context according to the provided settings. Only settings which are
     * acceptable on non-blocking connections are set.
     */
    Status initSSLContext(SSL_CTX* context,
                          const SSLParams& params,
                          ConnectionDirection direction) final;

    SSLConnectionInterface* connect(Socket* socket) final;

    SSLConnectionInterface* accept(Socket* socket, const char* initialBytes, int len) final;

    SSLPeerInfo parseAndValidatePeerCertificateDeprecated(const SSLConnectionInterface* conn,
                                                          const std::string& remoteHost,
                                                          const HostAndPort& hostForLogging) final;

    StatusWith<SSLPeerInfo> parseAndValidatePeerCertificate(
        SSL* conn, const std::string& remoteHost, const HostAndPort& hostForLogging) final;

    const SSLConfiguration& getSSLConfiguration() const final {
        return _sslConfiguration;
    }

    int SSL_read(SSLConnectionInterface* conn, void* buf, int num) final;

    int SSL_write(SSLConnectionInterface* conn, const void* buf, int num) final;

    int SSL_shutdown(SSLConnectionInterface* conn) final;

private:
    const int _rolesNid = OBJ_create(mongodbRolesOID.identifier.c_str(),
                                     mongodbRolesOID.shortDescription.c_str(),
                                     mongodbRolesOID.longDescription.c_str());
    UniqueSSLContext _serverContext;  // SSL context for incoming connections
    UniqueSSLContext _clientContext;  // SSL context for outgoing connections

// On OSX, it's not safe to copy the system CA certificates after a fork(), which we need to
// do the deathtest unittests. So on __APPLE__ platforms, we copy the certificates into a vector
// of OpenSSL X509 objects once at startup in _getSystemCerts() and then append them into
// X509_STORE's when initializing SSL_CTX's.
//
// On other platforms it's safe to load the certificate each time we initialize an SSL_CTX.
#if defined(__APPLE__)
    std::vector<UniqueX509> _systemCACertificates;
#endif
    bool _weakValidation;
    bool _allowInvalidCertificates;
    bool _allowInvalidHostnames;
    bool _suppressNoCertificateWarning;
    SSLConfiguration _sslConfiguration;

    /** Password caching helper class.
     * Objects of this type will remember the config provided password they had access to at
     * construction.
     * If the config provides no password, fetching will invoke OpenSSL's password prompting
     * routine, and cache the outcome.
     */
    class PasswordFetcher {
    public:
        PasswordFetcher(StringData configParameter, StringData prompt)
            : _password(configParameter.begin(), configParameter.end()),
              _prompt(prompt.toString()) {
            invariant(!prompt.empty());
        }

        /** Either returns a cached password, or prompts the user to enter one. */
        StatusWith<StringData> fetchPassword() {
            stdx::lock_guard<stdx::mutex> lock(_mutex);
            if (_password->size()) {
                return StringData(_password->c_str());
            }

            std::array<char, 1025> pwBuf;
            int ret = EVP_read_pw_string(pwBuf.data(), pwBuf.size() - 1, _prompt.c_str(), 0);
            pwBuf.at(pwBuf.size() - 1) = '\0';

            if (ret != 0) {
                StringBuilder error;
                if (ret == -1) {
                    error << "Failed to read user provided decryption password: "
                          << SSLManagerInterface::getSSLErrorMessage(ERR_get_error());
                } else {
                    error << "Failed to read user provided decryption password";
                }
                return Status(ErrorCodes::UnknownError, error.str());
            }

            _password = SecureString(pwBuf.data());
            return StringData(_password->c_str());
        }

    private:
        stdx::mutex _mutex;
        SecureString _password;  // Protected by _mutex

        std::string _prompt;
    };
    PasswordFetcher _serverPEMPassword;
    PasswordFetcher _clusterPEMPassword;

    /**
     * creates an SSL object to be used for this file descriptor.
     * caller must SSL_free it.
     */
    SSL* _secure(SSL_CTX* context, int fd);

    /**
     * Given an error code from an SSL-type IO function, logs an
     * appropriate message and throws a NetworkException.
     */
    MONGO_COMPILER_NORETURN void _handleSSLError(SSLConnectionOpenSSL* conn, int ret);

    /*
     * Init the SSL context using parameters provided in params. This SSL context will
     * be configured for blocking send/receive.
     */
    bool _initSynchronousSSLContext(UniqueSSLContext* context,
                                    const SSLParams& params,
                                    ConnectionDirection direction);

    /*
     * Converts time from OpenSSL return value to unsigned long long
     * representing the milliseconds since the epoch.
     */
    unsigned long long _convertASN1ToMillis(ASN1_TIME* t);

    /*
     * Parse and store x509 subject name from the PEM keyfile.
     * For server instances check that PEM certificate is not expired
     * and extract server certificate notAfter date.
     * @param keyFile referencing the PEM file to be read.
     * @param subjectName as a pointer to the subject name variable being set.
     * @param serverNotAfter a Date_t object pointer that is valued if the
     * date is to be checked (as for a server certificate) and null otherwise.
     * @return bool showing if the function was successful.
     */
    bool _parseAndValidateCertificate(const std::string& keyFile,
                                      PasswordFetcher* keyPassword,
                                      SSLX509Name* subjectName,
                                      Date_t* serverNotAfter);


    StatusWith<stdx::unordered_set<RoleName>> _parsePeerRoles(X509* peerCert) const;

    /** @return true if was successful, otherwise false */
    bool _setupPEM(SSL_CTX* context, const std::string& keyFile, PasswordFetcher* password);

    /*
     * Set up an SSL context for certificate validation by loading a CA
     */
    Status _setupCA(SSL_CTX* context, const std::string& caFile);

    /*
     * Set up an SSL context for certificate validation by loading the system's CA store
     */
    Status _setupSystemCA(SSL_CTX* context);

#if defined(__APPLE__)
    /*
     * Loads the system-trusted CA certificates from a native source into a vector of X509 objects
     */
    std::vector<UniqueX509> _getSystemCerts();
#endif

    /*
     * Import a certificate revocation list into an SSL context
     * for use with validating certificates
     */
    bool _setupCRL(SSL_CTX* context, const std::string& crlFile);

    /*
     * sub function for checking the result of an SSL operation
     */
    bool _doneWithSSLOp(SSLConnectionOpenSSL* conn, int status);

    /*
     * Send and receive network data
     */
    void _flushNetworkBIO(SSLConnectionOpenSSL* conn);

    /**
     * Callbacks for SSL functions.
     */
    static int password_cb(char* buf, int num, int rwflag, void* userdata);
    static int verify_cb(int ok, X509_STORE_CTX* ctx);
};

void setupFIPS() {
// Turn on FIPS mode if requested, OPENSSL_FIPS must be defined by the OpenSSL headers
#if defined(MONGO_CONFIG_HAVE_FIPS_MODE_SET)
    int status = FIPS_mode_set(1);
    if (!status) {
        severe() << "can't activate FIPS mode: "
                 << SSLManagerInterface::getSSLErrorMessage(ERR_get_error());
        fassertFailedNoTrace(16703);
    }
    log() << "FIPS 140-2 mode activated";
#else
    severe() << "this version of mongodb was not compiled with FIPS support";
    fassertFailedNoTrace(17089);
#endif
}

}  // namespace

// Global variable indicating if this is a server or a client instance
bool isSSLServer = false;

extern SSLManagerInterface* theSSLManager;

MONGO_INITIALIZER(SetupOpenSSL)(InitializerContext*) {
    SSL_library_init();
    SSL_load_error_strings();
    ERR_load_crypto_strings();

    if (sslGlobalParams.sslFIPSMode) {
        setupFIPS();
    }

    // Add all digests and ciphers to OpenSSL's internal table
    // so that encryption/decryption is backwards compatible
    OpenSSL_add_all_algorithms();

    // Setup OpenSSL multithreading callbacks and mutexes
    SSLThreadInfo::init();

    return Status::OK();
}

MONGO_INITIALIZER_WITH_PREREQUISITES(SSLManager, ("SetupOpenSSL", "EndStartupOptionHandling"))
(InitializerContext*) {
    if (!isSSLServer || (sslGlobalParams.sslMode.load() != SSLParams::SSLMode_disabled)) {
        theSSLManager = new SSLManagerOpenSSL(sslGlobalParams, isSSLServer);
    }
    return Status::OK();
}

std::unique_ptr<SSLManagerInterface> SSLManagerInterface::create(const SSLParams& params,
                                                                 bool isServer) {
    return std::make_unique<SSLManagerOpenSSL>(params, isServer);
}

SSLX509Name getCertificateSubjectX509Name(X509* cert) {
    std::vector<std::vector<SSLX509Name::Entry>> entries;

    auto name = X509_get_subject_name(cert);
    int count = X509_NAME_entry_count(name);
    int prevSet = -1;
    std::vector<SSLX509Name::Entry> rdn;
    for (int i = count - 1; i >= 0; --i) {
        auto* entry = X509_NAME_get_entry(name, i);

        const auto currentSet = X509_NAME_ENTRY_set(entry);
        if (currentSet != prevSet) {
            if (!rdn.empty()) {
                entries.push_back(std::move(rdn));
                rdn = std::vector<SSLX509Name::Entry>();
            }
            prevSet = currentSet;
        }

        char buffer[128];
        // OBJ_obj2txt can only fail if we pass a nullptr from get_object,
        // or if OpenSSL's BN library falls over.
        // In either case, just panic.
        uassert(ErrorCodes::InvalidSSLConfiguration,
                "Unable to parse certiciate subject name",
                OBJ_obj2txt(buffer, sizeof(buffer), X509_NAME_ENTRY_get_object(entry), 1) > 0);

        const auto* str = X509_NAME_ENTRY_get_data(entry);
        rdn.emplace_back(
            buffer, str->type, std::string(reinterpret_cast<const char*>(str->data), str->length));
    }
    if (!rdn.empty()) {
        entries.push_back(std::move(rdn));
    }

    SSLX509Name subjectName = SSLX509Name(std::move(entries));
    uassertStatusOK(subjectName.normalizeStrings());
    return subjectName;
}

int verifyDHParameters(const UniqueDHParams& dhparams) {
    int codes = 0;

    ::DH_check(dhparams.get(), &codes);

    const BIGNUM* p = nullptr;
    const BIGNUM* g = nullptr;

    DH_get0_pqg(dhparams.get(), &p, nullptr, &g);

    // Many RFC's define DH parameters where 2 is listed as the generator for the group. If p = 11
    // mod 24, then 2 generates the entire group. However, it becomes trivial for an attacker to
    // determine the lsb of any shared secret. Instead of leaking this bit, the RFC designers chose
    // primes which halve the number of possible shared secrets. Since 2 does not generate the
    // entire group associated with such primes, OpenSSL fails the DH_check with
    // DH_NOT_SUITABLE_GENERATOR. Since leaking a single bit and halving the number of possible
    // shared secrets is essentially the same thing, we manually check for it here (p = 23 mod 24)
    // and strip out the errors as necessary. See Appendix E of RFC 2412.
    if (BN_is_word(g, DH_GENERATOR_2)) {
        long residue = BN_mod_word(p, 24);
        if (residue == 11 || residue == 23) {
            codes &= ~DH_NOT_SUITABLE_GENERATOR;
        }
    }
    return codes;
}

UniqueDHParams makeDefaultDHParameters() {
    UniqueDHParams dhparams(::DH_new());

    if (!dhparams) {
        return nullptr;
    }

    UniqueBIGNUM p(::BN_bin2bn(ffdhe3072_p.data(), ffdhe3072_p.size(), nullptr));
    UniqueBIGNUM g(::BN_bin2bn(&ffdhe3072_g, sizeof(ffdhe3072_g), nullptr));

    if (!p || !g) {
        return nullptr;
    }

    if (DH_set0_pqg(dhparams.get(), p.get(), nullptr, g.get()) != 1) {
        return nullptr;
    }

    // DH takes over memory management responsibilities after successfully setting these
    p.release();
    g.release();

    return dhparams;
}

SSLConnectionOpenSSL::SSLConnectionOpenSSL(SSL_CTX* context,
                                           Socket* sock,
                                           const char* initialBytes,
                                           int len)
    : socket(sock) {
    ssl = SSL_new(context);

    std::string sslErr =
        nullptr != getSSLManager() ? getSSLManager()->getSSLErrorMessage(ERR_get_error()) : "";
    massert(15861, "Error creating new SSL object " + sslErr, ssl);

    BIO_new_bio_pair(&internalBIO, BUFFER_SIZE, &networkBIO, BUFFER_SIZE);
    SSL_set_bio(ssl, internalBIO, internalBIO);

    if (len > 0) {
        int toBIO = BIO_write(networkBIO, initialBytes, len);
        if (toBIO != len) {
            LOG(3) << "Failed to write initial network data to the SSL BIO layer";
            throwSocketError(SocketErrorKind::RECV_ERROR, socket->remoteString());
        }
    }
}

SSLConnectionOpenSSL::~SSLConnectionOpenSSL() {
    if (ssl) {  // The internalBIO is automatically freed as part of SSL_free
        SSL_free(ssl);
    }
    if (networkBIO) {
        BIO_free(networkBIO);
    }
}

SSLManagerOpenSSL::SSLManagerOpenSSL(const SSLParams& params, bool isServer)
    : _serverContext(nullptr),
      _clientContext(nullptr),
#if defined(__APPLE__)
      _systemCACertificates(_getSystemCerts()),
#endif
      _weakValidation(params.sslWeakCertificateValidation),
      _allowInvalidCertificates(params.sslAllowInvalidCertificates),
      _allowInvalidHostnames(params.sslAllowInvalidHostnames),
      _suppressNoCertificateWarning(params.suppressNoTLSPeerCertificateWarning),
      _serverPEMPassword(params.sslPEMKeyPassword, "Enter PEM passphrase"),
      _clusterPEMPassword(params.sslClusterPassword, "Enter cluster certificate passphrase") {
    if (!_initSynchronousSSLContext(&_clientContext, params, ConnectionDirection::kOutgoing)) {
        uasserted(16768, "ssl initialization problem");
    }

    // pick the certificate for use in outgoing connections,
    std::string clientPEM;
    PasswordFetcher* clientPassword;
    if (!isServer || params.sslClusterFile.empty()) {
        // We are either a client, or a server without a cluster key,
        // so use the PEM key file, if specified
        clientPEM = params.sslPEMKeyFile;
        clientPassword = &_serverPEMPassword;
    } else {
        // We are a server with a cluster key, so use the cluster key file
        clientPEM = params.sslClusterFile;
        clientPassword = &_clusterPEMPassword;
    }

    if (!clientPEM.empty()) {
        if (!_parseAndValidateCertificate(
                clientPEM, clientPassword, &_sslConfiguration.clientSubjectName, nullptr)) {
            uasserted(16941, "ssl initialization problem");
        }
    }
    // SSL server specific initialization
    if (isServer) {
        if (!_initSynchronousSSLContext(&_serverContext, params, ConnectionDirection::kIncoming)) {
            uasserted(16562, "ssl initialization problem");
        }

        SSLX509Name serverSubjectName;
        if (!_parseAndValidateCertificate(params.sslPEMKeyFile,
                                          &_serverPEMPassword,
                                          &serverSubjectName,
                                          &_sslConfiguration.serverCertificateExpirationDate)) {
            uasserted(16942, "ssl initialization problem");
        }

        uassertStatusOK(_sslConfiguration.setServerSubjectName(std::move(serverSubjectName)));

        static CertificateExpirationMonitor task =
            CertificateExpirationMonitor(_sslConfiguration.serverCertificateExpirationDate);
    }
}

int SSLManagerOpenSSL::password_cb(char* buf, int num, int rwflag, void* userdata) {
    // Unless OpenSSL misbehaves, num should always be positive
    fassert(17314, num > 0);
    invariant(userdata);

    auto pwFetcher = static_cast<PasswordFetcher*>(userdata);
    auto swPassword = pwFetcher->fetchPassword();
    if (!swPassword.isOK()) {
        error() << "Unable to fetch password: " << swPassword.getStatus();
        return -1;
    }
    StringData password = std::move(swPassword.getValue());

    const size_t copyCount = std::min(password.size(), static_cast<size_t>(num));
    std::copy_n(password.begin(), copyCount, buf);
    buf[copyCount] = '\0';

    return copyCount;
}

int SSLManagerOpenSSL::verify_cb(int ok, X509_STORE_CTX* ctx) {
    return 1;  // always succeed; we will catch the error in our get_verify_result() call
}

int SSLManagerOpenSSL::SSL_read(SSLConnectionInterface* connInterface, void* buf, int num) {
    int status;
    SSLConnectionOpenSSL* conn = checked_cast<SSLConnectionOpenSSL*>(connInterface);
    do {
        status = ::SSL_read(conn->ssl, buf, num);
    } while (!_doneWithSSLOp(conn, status));

    if (status <= 0)
        _handleSSLError(conn, status);
    return status;
}

int SSLManagerOpenSSL::SSL_write(SSLConnectionInterface* connInterface, const void* buf, int num) {
    int status;
    SSLConnectionOpenSSL* conn = checked_cast<SSLConnectionOpenSSL*>(connInterface);
    do {
        status = ::SSL_write(conn->ssl, buf, num);
    } while (!_doneWithSSLOp(conn, status));

    if (status <= 0)
        _handleSSLError(conn, status);
    return status;
}

int SSLManagerOpenSSL::SSL_shutdown(SSLConnectionInterface* connInterface) {
    int status;
    SSLConnectionOpenSSL* conn = checked_cast<SSLConnectionOpenSSL*>(connInterface);
    do {
        status = ::SSL_shutdown(conn->ssl);
    } while (!_doneWithSSLOp(conn, status));

    if (status < 0)
        _handleSSLError(conn, status);
    return status;
}

Status SSLManagerOpenSSL::initSSLContext(SSL_CTX* context,
                                         const SSLParams& params,
                                         ConnectionDirection direction) {
    // SSL_OP_ALL - Activate all bug workaround options, to support buggy client SSL's.
    // SSL_OP_NO_SSLv2 - Disable SSL v2 support
    // SSL_OP_NO_SSLv3 - Disable SSL v3 support
    long options = SSL_OP_ALL | SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3;

    // Set the supported TLS protocols. Allow --sslDisabledProtocols to disable selected
    // ciphers.
    for (const SSLParams::Protocols& protocol : params.sslDisabledProtocols) {
        if (protocol == SSLParams::Protocols::TLS1_0) {
            options |= SSL_OP_NO_TLSv1;
        } else if (protocol == SSLParams::Protocols::TLS1_1) {
            options |= SSL_OP_NO_TLSv1_1;
        } else if (protocol == SSLParams::Protocols::TLS1_2) {
            options |= SSL_OP_NO_TLSv1_2;
        } else if (protocol == SSLParams::Protocols::TLS1_3) {
            options |= SSL_OP_NO_TLSv1_3;
        }
    }

#ifdef SSL_OP_NO_RENEGOTIATION
    options |= SSL_OP_NO_RENEGOTIATION;
#endif

    ::SSL_CTX_set_options(context, options);

    // HIGH - Enable strong ciphers
    // !EXPORT - Disable export ciphers (40/56 bit)
    // !aNULL - Disable anonymous auth ciphers
    // @STRENGTH - Sort ciphers based on strength
    std::string cipherConfig = "HIGH:!EXPORT:!aNULL@STRENGTH";

    // Allow the cipher configuration string to be overriden by --sslCipherConfig
    if (!params.sslCipherConfig.empty()) {
        cipherConfig = params.sslCipherConfig;
    }

    if (0 == ::SSL_CTX_set_cipher_list(context, cipherConfig.c_str())) {
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "Can not set supported cipher suites: "
                                    << getSSLErrorMessage(ERR_get_error()));
    }

    // We use the address of the context as the session id context.
    if (0 ==
        ::SSL_CTX_set_session_id_context(
            context, reinterpret_cast<unsigned char*>(&context), sizeof(context))) {
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "Can not store ssl session id context: "
                                    << getSSLErrorMessage(ERR_get_error()));
    }

    if (direction == ConnectionDirection::kOutgoing && params.tlsWithholdClientCertificate) {
        // Do not send a client certificate if they have been suppressed.

    } else if (direction == ConnectionDirection::kOutgoing && !params.sslClusterFile.empty()) {
        // Use the configured clusterFile as our client certificate.
        if (!_setupPEM(context, params.sslClusterFile, &_clusterPEMPassword)) {
            return Status(ErrorCodes::InvalidSSLConfiguration, "Can not set up ssl clusterFile.");
        }

    } else if (!params.sslPEMKeyFile.empty()) {
        // Use the base pemKeyFile for any other outgoing connections,
        // as well as all incoming connections.
        if (!_setupPEM(context, params.sslPEMKeyFile, &_serverPEMPassword)) {
            return Status(ErrorCodes::InvalidSSLConfiguration, "Can not set up PEM key file.");
        }
    }

    std::string cafile = params.sslCAFile;
    if (direction == ConnectionDirection::kIncoming && !params.sslClusterCAFile.empty()) {
        cafile = params.sslClusterCAFile;
    }
    const auto status = cafile.empty() ? _setupSystemCA(context) : _setupCA(context, cafile);
    if (!status.isOK()) {
        return status;
    }

    if (!params.sslCRLFile.empty()) {
        if (!_setupCRL(context, params.sslCRLFile)) {
            return Status(ErrorCodes::InvalidSSLConfiguration, "Can not set up CRL file.");
        }
    }

    if (!params.sslPEMTempDHParam.empty()) {
        try {
            std::ifstream dhparamPemFile(params.sslPEMTempDHParam, std::ios_base::binary);
            if (dhparamPemFile.fail() || dhparamPemFile.bad()) {
                return Status(ErrorCodes::InvalidSSLConfiguration,
                              str::stream() << "Cannot open PEM DHParams file.");
            }

            std::vector<std::uint8_t> paramData{std::istreambuf_iterator<char>(dhparamPemFile),
                                                std::istreambuf_iterator<char>()};
            auto bio = makeUniqueMemBio(paramData);

            UniqueDHParams dhparams(::PEM_read_bio_DHparams(bio.get(), nullptr, nullptr, nullptr));
            if (!dhparams) {
                return Status(ErrorCodes::InvalidSSLConfiguration,
                              str::stream() << "Error reading DHParams file."
                                            << getSSLErrorMessage(ERR_get_error()));
            }

            if (::SSL_CTX_set_tmp_dh(context, dhparams.get()) != 1) {
                return Status(ErrorCodes::InvalidSSLConfiguration,
                              str::stream() << "Failure to set PFS DH parameters: "
                                            << getSSLErrorMessage(ERR_get_error()));
            }
        } catch (const std::exception& ex) {
            return Status(ErrorCodes::InvalidSSLConfiguration, ex.what());
        }
    }

    // We always set ECDH mode anyhow, if available.
    if (enableECDHE(context)) {
        // If we enable ECDHE successfully, we can also enable DHE without breaking compatibility
        // with Java 7. Java 7 clients are unable to use DHE with strong parameters, but they will
        // ignore that we advertise it if we advertise ECDHE first, which it does support.

        // If opensslDiffieHellmanParameters has been specified, we set it above (even if ECDHE is
        // not enabled). Otherwise, we use a default, (currently) strong DHE parameter.
        if (params.sslPEMTempDHParam.empty()) {
            UniqueDHParams dhparams = makeDefaultDHParameters();

            if (!dhparams || SSL_CTX_set_tmp_dh(context, dhparams.get()) != 1) {
                error() << "Failed to set default DH parameters: "
                        << getSSLErrorMessage(ERR_get_error());
            }
        }
    }

    return Status::OK();
}

bool SSLManagerOpenSSL::_initSynchronousSSLContext(UniqueSSLContext* contextPtr,
                                                   const SSLParams& params,
                                                   ConnectionDirection direction) {
    *contextPtr = UniqueSSLContext(SSL_CTX_new(SSLv23_method()));

    uassertStatusOK(initSSLContext(contextPtr->get(), params, direction));

    // If renegotiation is needed, don't return from recv() or send() until it's successful.
    // Note: this is for blocking sockets only.
    SSL_CTX_set_mode(contextPtr->get(), SSL_MODE_AUTO_RETRY);

    return true;
}

unsigned long long SSLManagerOpenSSL::_convertASN1ToMillis(ASN1_TIME* asn1time) {
    BIO* outBIO = BIO_new(BIO_s_mem());
    int timeError = ASN1_TIME_print(outBIO, asn1time);
    ON_BLOCK_EXIT([&] { BIO_free(outBIO); });

    if (timeError <= 0) {
        error() << "ASN1_TIME_print failed or wrote no data.";
        return 0;
    }

    char dateChar[DATE_LEN];
    timeError = BIO_gets(outBIO, dateChar, DATE_LEN);
    if (timeError <= 0) {
        error() << "BIO_gets call failed to transfer contents to buf";
        return 0;
    }

    // Ensure that day format is two digits for parsing.
    // Jun  8 17:00:03 2014 becomes Jun 08 17:00:03 2014.
    if (dateChar[4] == ' ') {
        dateChar[4] = '0';
    }

    std::istringstream inStringStream((std::string(dateChar, 20)));
    boost::posix_time::time_input_facet* inputFacet =
        new boost::posix_time::time_input_facet("%b %d %H:%M:%S %Y");

    inStringStream.imbue(std::locale(std::cout.getloc(), inputFacet));
    boost::posix_time::ptime posixTime;
    inStringStream >> posixTime;

    const boost::gregorian::date epoch = boost::gregorian::date(1970, boost::gregorian::Jan, 1);

    return (posixTime - boost::posix_time::ptime(epoch)).total_milliseconds();
}

bool SSLManagerOpenSSL::_parseAndValidateCertificate(const std::string& keyFile,
                                                     PasswordFetcher* keyPassword,
                                                     SSLX509Name* subjectName,
                                                     Date_t* serverCertificateExpirationDate) {
    BIO* inBIO = BIO_new(BIO_s_file());
    if (inBIO == nullptr) {
        error() << "failed to allocate BIO object: " << getSSLErrorMessage(ERR_get_error());
        return false;
    }

    ON_BLOCK_EXIT([&] { BIO_free(inBIO); });
    if (BIO_read_filename(inBIO, keyFile.c_str()) <= 0) {
        error() << "cannot read key file when setting subject name: " << keyFile << ' '
                << getSSLErrorMessage(ERR_get_error());
        return false;
    }

    X509* x509 = PEM_read_bio_X509(
        inBIO, nullptr, &SSLManagerOpenSSL::password_cb, static_cast<void*>(&keyPassword));
    if (x509 == nullptr) {
        error() << "cannot retrieve certificate from keyfile: " << keyFile << ' '
                << getSSLErrorMessage(ERR_get_error());
        return false;
    }
    ON_BLOCK_EXIT([&] { X509_free(x509); });

    *subjectName = getCertificateSubjectX509Name(x509);
    if (serverCertificateExpirationDate != nullptr) {
        unsigned long long notBeforeMillis = _convertASN1ToMillis(X509_get_notBefore(x509));
        if (notBeforeMillis == 0) {
            error() << "date conversion failed";
            return false;
        }

        unsigned long long notAfterMillis = _convertASN1ToMillis(X509_get_notAfter(x509));
        if (notAfterMillis == 0) {
            error() << "date conversion failed";
            return false;
        }

        if ((notBeforeMillis > curTimeMillis64()) || (curTimeMillis64() > notAfterMillis)) {
            severe() << "The provided SSL certificate is expired or not yet valid.";
            fassertFailedNoTrace(28652);
        }

        *serverCertificateExpirationDate = Date_t::fromMillisSinceEpoch(notAfterMillis);
    }

    return true;
}

bool SSLManagerOpenSSL::_setupPEM(SSL_CTX* context,
                                  const std::string& keyFile,
                                  PasswordFetcher* password) {
    if (SSL_CTX_use_certificate_chain_file(context, keyFile.c_str()) != 1) {
        error() << "cannot read certificate file: " << keyFile << ' '
                << getSSLErrorMessage(ERR_get_error());
        return false;
    }

    BIO* inBio = BIO_new(BIO_s_file());
    if (!inBio) {
        error() << "failed to allocate BIO object: " << getSSLErrorMessage(ERR_get_error());
        return false;
    }
    const auto bioGuard = makeGuard([&inBio]() { BIO_free(inBio); });

    if (BIO_read_filename(inBio, keyFile.c_str()) <= 0) {
        error() << "cannot read PEM key file: " << keyFile << ' '
                << getSSLErrorMessage(ERR_get_error());
        return false;
    }

    // Obtain the private key, using our callback to acquire a decryption password if necessary.
    decltype(&SSLManagerOpenSSL::password_cb) password_cb = &SSLManagerOpenSSL::password_cb;
    void* userdata = static_cast<void*>(password);
    EVP_PKEY* privateKey = PEM_read_bio_PrivateKey(inBio, nullptr, password_cb, userdata);
    if (!privateKey) {
        error() << "cannot read PEM key file: " << keyFile << ' '
                << getSSLErrorMessage(ERR_get_error());
        return false;
    }
    const auto privateKeyGuard = makeGuard([&privateKey]() { EVP_PKEY_free(privateKey); });

    if (SSL_CTX_use_PrivateKey(context, privateKey) != 1) {
        error() << "cannot use PEM key file: " << keyFile << ' '
                << getSSLErrorMessage(ERR_get_error());
        return false;
    }

    // Verify that the certificate and the key go together.
    if (SSL_CTX_check_private_key(context) != 1) {
        error() << "SSL certificate validation: " << getSSLErrorMessage(ERR_get_error());
        return false;
    }

    return true;
}

Status SSLManagerOpenSSL::_setupCA(SSL_CTX* context, const std::string& caFile) {
    // Set the list of CAs sent to clients
    STACK_OF(X509_NAME)* certNames = SSL_load_client_CA_file(caFile.c_str());
    if (certNames == nullptr) {
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "cannot read certificate authority file: " << caFile << " "
                                    << getSSLErrorMessage(ERR_get_error()));
    }
    SSL_CTX_set_client_CA_list(context, certNames);

    // Load trusted CA
    if (SSL_CTX_load_verify_locations(context, caFile.c_str(), nullptr) != 1) {
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "cannot read certificate authority file: " << caFile << " "
                                    << getSSLErrorMessage(ERR_get_error()));
    }

    // Set SSL to require peer (client) certificate verification
    // if a certificate is presented
    SSL_CTX_set_verify(context, SSL_VERIFY_PEER, &SSLManagerOpenSSL::verify_cb);
    _sslConfiguration.hasCA = true;
    return Status::OK();
}

inline Status checkX509_STORE_error() {
    const auto errCode = ERR_peek_last_error();
    if (ERR_GET_LIB(errCode) != ERR_LIB_X509 ||
        ERR_GET_REASON(errCode) != X509_R_CERT_ALREADY_IN_HASH_TABLE) {
        return {ErrorCodes::InvalidSSLConfiguration,
                str::stream() << "Error adding certificate to X509 store: "
                              << ERR_reason_error_string(errCode)};
    }
    return Status::OK();
}

#if defined(_WIN32)
// This imports the certificates in a given Windows certificate store into an X509_STORE for
// openssl to use during certificate validation.
Status importCertStoreToX509_STORE(const wchar_t* storeName,
                                   DWORD storeLocation,
                                   X509_STORE* verifyStore) {
    // Use NULL for argument 3, nullptr is not convertible to HCRYPTPROV_LEGACY type.
    HCERTSTORE systemStore = CertOpenStore(CERT_STORE_PROV_SYSTEM_W,
                                           0,
                                           NULL,
                                           storeLocation | CERT_STORE_READONLY_FLAG,
                                           const_cast<LPWSTR>(storeName));
    if (systemStore == nullptr) {
        return {ErrorCodes::InvalidSSLConfiguration,
                str::stream() << "error opening system CA store: " << errnoWithDescription()};
    }
    auto systemStoreGuard = makeGuard([systemStore]() { CertCloseStore(systemStore, 0); });

    PCCERT_CONTEXT certCtx = nullptr;
    while ((certCtx = CertEnumCertificatesInStore(systemStore, certCtx)) != nullptr) {
        auto certBytes = static_cast<const unsigned char*>(certCtx->pbCertEncoded);
        X509* x509Obj = d2i_X509(nullptr, &certBytes, certCtx->cbCertEncoded);
        if (x509Obj == nullptr) {
            return {ErrorCodes::InvalidSSLConfiguration,
                    str::stream() << "Error parsing X509 object from Windows certificate store"
                                  << SSLManagerInterface::getSSLErrorMessage(ERR_get_error())};
        }
        const auto x509ObjGuard = makeGuard([&x509Obj]() { X509_free(x509Obj); });

        if (X509_STORE_add_cert(verifyStore, x509Obj) != 1) {
            auto status = checkX509_STORE_error();
            if (!status.isOK())
                return status;
        }
    }
    int lastError = GetLastError();
    if (lastError != CRYPT_E_NOT_FOUND) {
        return {ErrorCodes::InvalidSSLConfiguration,
                str::stream() << "Error enumerating certificates: "
                              << errnoWithDescription(lastError)};
    }

    return Status::OK();
}

#elif defined(__APPLE__)

template <typename T>
class CFTypeRefHolder {
public:
    explicit CFTypeRefHolder(T ptr) : ref(static_cast<CFTypeRef>(ptr)) {}
    ~CFTypeRefHolder() {
        CFRelease(ref);
    }
    operator T() {
        return static_cast<T>(ref);
    }

private:
    CFTypeRef ref = nullptr;
};
template <typename T>
CFTypeRefHolder<T> makeCFTypeRefHolder(T ptr) {
    return CFTypeRefHolder<T>(ptr);
}

std::string OSStatusToString(OSStatus status) {
    auto errMsg = makeCFTypeRefHolder(SecCopyErrorMessageString(status, NULL));
    return std::string{CFStringGetCStringPtr(errMsg, kCFStringEncodingUTF8)};
}

std::vector<UniqueX509> SSLManagerOpenSSL::_getSystemCerts() {
    // Copy the system CA certs into a CFArray for us to iterate and convert into X509*
    auto anchorCerts = makeCFTypeRefHolder([] {
        CFArrayRef ret;
        auto status = SecTrustCopyAnchorCertificates(&ret);
        uassert(50928,
                str::stream() << "Error enumerating certificates: " << OSStatusToString(status),
                status == ::errSecSuccess);
        return ret;
    }());

    std::vector<UniqueX509> ret;
    for (CFIndex i = 0; i < CFArrayGetCount(anchorCerts); i++) {
        SecCertificateRef cert = static_cast<SecCertificateRef>(
            const_cast<void*>(CFArrayGetValueAtIndex(anchorCerts, i)));

        uassert(50929,
                "Certificate array had something other than a certificate in it",
                ::CFGetTypeID(cert) == ::SecCertificateGetTypeID());

        // Get the raw X509 bytes out of the certificate
        auto rawData = makeCFTypeRefHolder(SecCertificateCopyData(cert));
        uassert(50930, str::stream() << "Error converting certificate to raw bytes", rawData);
        const uint8_t* rawDataPtr = CFDataGetBytePtr(rawData);

        // Parse an openssl X509 object from each returned certificate
        UniqueX509 x509Cert(d2i_X509(nullptr, &rawDataPtr, CFDataGetLength(rawData)));
        uassert(50931,
                str::stream() << "Error parsing X509 certificate from system keychain: "
                              << ERR_reason_error_string(ERR_peek_last_error()),
                x509Cert);
        ret.push_back(std::move(x509Cert));
    }

    return ret;
}
#endif

Status SSLManagerOpenSSL::_setupSystemCA(SSL_CTX* context) {
#if !defined(_WIN32) && !defined(__APPLE__)
    // On non-Windows/non-Apple platforms, the OpenSSL libraries should have been configured
    // with default locations for CA certificates.
    if (SSL_CTX_set_default_verify_paths(context) != 1) {
        return {
            ErrorCodes::InvalidSSLConfiguration,
            str::stream() << "error loading system CA certificates "
                          << "(default certificate file: " << X509_get_default_cert_file() << ", "
                          << "default certificate path: " << X509_get_default_cert_dir() << ")"};
    }
#else

    X509_STORE* verifyStore = SSL_CTX_get_cert_store(context);
    if (!verifyStore) {
        return {ErrorCodes::InvalidSSLConfiguration,
                "no X509 store found for SSL context while loading system certificates"};
    }
#if defined(_WIN32)
    auto status = importCertStoreToX509_STORE(L"root", CERT_SYSTEM_STORE_CURRENT_USER, verifyStore);
    if (!status.isOK())
        return status;
    return importCertStoreToX509_STORE(L"CA", CERT_SYSTEM_STORE_CURRENT_USER, verifyStore);
#elif defined(__APPLE__)
    for (const auto& cert : _systemCACertificates) {
        X509_STORE_add_cert(verifyStore, cert.get());
    }
#endif
#endif

    return Status::OK();
}

bool SSLManagerOpenSSL::_setupCRL(SSL_CTX* context, const std::string& crlFile) {
    X509_STORE* store = SSL_CTX_get_cert_store(context);
    fassert(16583, store);

    X509_STORE_set_flags(store, X509_V_FLAG_CRL_CHECK);
    X509_LOOKUP* lookup = X509_STORE_add_lookup(store, X509_LOOKUP_file());
    fassert(16584, lookup);

    int status = X509_load_crl_file(lookup, crlFile.c_str(), X509_FILETYPE_PEM);
    if (status == 0) {
        error() << "cannot read CRL file: " << crlFile << ' '
                << getSSLErrorMessage(ERR_get_error());
        return false;
    }
    log() << "ssl imported " << status << " revoked certificate" << ((status == 1) ? "" : "s")
          << " from the revocation list.";
    return true;
}

/*
 * The interface layer between network and BIO-pair. The BIO-pair buffers
 * the data to/from the TLS layer.
 */
void SSLManagerOpenSSL::_flushNetworkBIO(SSLConnectionOpenSSL* conn) {
    char buffer[BUFFER_SIZE];
    int wantWrite;

    /*
     * Write the complete contents of the buffer. Leaving the buffer
     * unflushed could cause a deadlock.
     */
    while ((wantWrite = BIO_ctrl_pending(conn->networkBIO)) > 0) {
        if (wantWrite > BUFFER_SIZE) {
            wantWrite = BUFFER_SIZE;
        }
        int fromBIO = BIO_read(conn->networkBIO, buffer, wantWrite);

        int writePos = 0;
        do {
            int numWrite = fromBIO - writePos;
            numWrite = send(conn->socket->rawFD(), buffer + writePos, numWrite, portSendFlags);
            if (numWrite < 0) {
                conn->socket->handleSendError(numWrite, "");
            }
            writePos += numWrite;
        } while (writePos < fromBIO);
    }

    int wantRead;
    while ((wantRead = BIO_ctrl_get_read_request(conn->networkBIO)) > 0) {
        if (wantRead > BUFFER_SIZE) {
            wantRead = BUFFER_SIZE;
        }

        int numRead = recv(conn->socket->rawFD(), buffer, wantRead, portRecvFlags);
        if (numRead <= 0) {
            conn->socket->handleRecvError(numRead, wantRead);
            continue;
        }

        int toBIO = BIO_write(conn->networkBIO, buffer, numRead);
        if (toBIO != numRead) {
            LOG(3) << "Failed to write network data to the SSL BIO layer";
            throwSocketError(SocketErrorKind::RECV_ERROR, conn->socket->remoteString());
        }
    }
}

bool SSLManagerOpenSSL::_doneWithSSLOp(SSLConnectionOpenSSL* conn, int status) {
    int sslErr = SSL_get_error(conn->ssl, status);
    switch (sslErr) {
        case SSL_ERROR_NONE:
            _flushNetworkBIO(conn);  // success, flush network BIO before leaving
            return true;
        case SSL_ERROR_WANT_WRITE:
        case SSL_ERROR_WANT_READ:
            _flushNetworkBIO(conn);  // not ready, flush network BIO and try again
            return false;
        default:
            return true;
    }
}

SSLConnectionInterface* SSLManagerOpenSSL::connect(Socket* socket) {
    std::unique_ptr<SSLConnectionOpenSSL> sslConn = std::make_unique<SSLConnectionOpenSSL>(
        _clientContext.get(), socket, (const char*)nullptr, 0);

    const auto undotted = removeFQDNRoot(socket->remoteAddr().hostOrIp());
    int ret = ::SSL_set_tlsext_host_name(sslConn->ssl, undotted.c_str());
    if (ret != 1)
        _handleSSLError(sslConn.get(), ret);

    do {
        ret = ::SSL_connect(sslConn->ssl);
    } while (!_doneWithSSLOp(sslConn.get(), ret));

    if (ret != 1)
        _handleSSLError(sslConn.get(), ret);

    return sslConn.release();
}

SSLConnectionInterface* SSLManagerOpenSSL::accept(Socket* socket,
                                                  const char* initialBytes,
                                                  int len) {
    std::unique_ptr<SSLConnectionOpenSSL> sslConn =
        std::make_unique<SSLConnectionOpenSSL>(_serverContext.get(), socket, initialBytes, len);

    int ret;
    do {
        ret = ::SSL_accept(sslConn->ssl);
    } while (!_doneWithSSLOp(sslConn.get(), ret));

    if (ret != 1)
        _handleSSLError(sslConn.get(), ret);

    return sslConn.release();
}


StatusWith<TLSVersion> mapTLSVersion(SSL* conn) {
    int protocol = SSL_version(conn);

    switch (protocol) {
        case TLS1_VERSION:
            return TLSVersion::kTLS10;
        case TLS1_1_VERSION:
            return TLSVersion::kTLS11;
        case TLS1_2_VERSION:
            return TLSVersion::kTLS12;
#ifdef TLS1_3_VERSION
        case TLS1_3_VERSION:
            return TLSVersion::kTLS13;
#endif
        default:
            return TLSVersion::kUnknown;
    }
}

StatusWith<SSLPeerInfo> SSLManagerOpenSSL::parseAndValidatePeerCertificate(
    SSL* conn, const std::string& remoteHost, const HostAndPort& hostForLogging) {
    auto sniName = getRawSNIServerName(conn);

    auto tlsVersionStatus = mapTLSVersion(conn);
    if (!tlsVersionStatus.isOK()) {
        return tlsVersionStatus.getStatus();
    }

    recordTLSVersion(tlsVersionStatus.getValue(), hostForLogging);

    if (!_sslConfiguration.hasCA && isSSLServer)
        return SSLPeerInfo(std::move(sniName));

    X509* peerCert = SSL_get_peer_certificate(conn);

    if (nullptr == peerCert) {  // no certificate presented by peer
        if (_weakValidation) {
            // do not give warning if certificate warnings are  suppressed
            if (!_suppressNoCertificateWarning) {
                warning() << "no SSL certificate provided by peer";
            }
            return SSLPeerInfo(std::move(sniName));
        } else {
            auto msg = "no SSL certificate provided by peer; connection rejected";
            error() << msg;
            return Status(ErrorCodes::SSLHandshakeFailed, msg);
        }
    }
    ON_BLOCK_EXIT([&] { X509_free(peerCert); });

    long result = SSL_get_verify_result(conn);

    if (result != X509_V_OK) {
        if (_allowInvalidCertificates) {
            warning() << "SSL peer certificate validation failed: "
                      << X509_verify_cert_error_string(result);
            return SSLPeerInfo(std::move(sniName));
        } else {
            str::stream msg;
            msg << "SSL peer certificate validation failed: "
                << X509_verify_cert_error_string(result);
            error() << msg.ss.str();
            return Status(ErrorCodes::SSLHandshakeFailed, msg);
        }
    }

    // TODO: check optional cipher restriction, using cert.
    auto peerSubject = getCertificateSubjectX509Name(peerCert);
    LOG(2) << "Accepted TLS connection from peer: " << peerSubject;

    StatusWith<stdx::unordered_set<RoleName>> swPeerCertificateRoles = _parsePeerRoles(peerCert);
    if (!swPeerCertificateRoles.isOK()) {
        return swPeerCertificateRoles.getStatus();
    }

    // Server side.
    if (remoteHost.empty()) {
        const auto exprThreshold = tlsX509ExpirationWarningThresholdDays;
        if (exprThreshold > 0) {
            const auto expiration = X509_get0_notAfter(peerCert);
            time_t threshold = (Date_t::now() + Days(exprThreshold)).toTimeT();

            if (X509_cmp_time(expiration, &threshold) < 0) {
                int days = 0, secs = 0;
                if (!ASN1_TIME_diff(&days, &secs, nullptr /* now */, expiration)) {
                    tlsEmitWarningExpiringClientCertificate(peerSubject);
                } else {
                    tlsEmitWarningExpiringClientCertificate(peerSubject, Days(days));
                }
            }
        }

        // If client and server certificate are the same, log a warning.
        if (_sslConfiguration.serverSubjectName() == peerSubject) {
            warning() << "Client connecting with server's own TLS certificate";
        }

        return SSLPeerInfo(peerSubject, sniName, std::move(swPeerCertificateRoles.getValue()));
    }

    // If this is an SSL client context (on a MongoDB server or client)
    // perform hostname validation of the remote server.

    // This is to standardize the IPAddress format for comparison.
    auto swCIDRRemoteHost = CIDR::parse(remoteHost);

    // Try to match using the Subject Alternate Name, if it exists.
    // RFC-2818 requires the Subject Alternate Name to be used if present.
    // Otherwise, the most specific Common Name field in the subject field
    // must be used.

    bool sanMatch = false;
    bool cnMatch = false;
    StringBuilder certificateNames;

    STACK_OF(GENERAL_NAME)* sanNames = static_cast<STACK_OF(GENERAL_NAME)*>(
        X509_get_ext_d2i(peerCert, NID_subject_alt_name, nullptr, nullptr));

    if (sanNames != nullptr) {
        int sanNamesList = sk_GENERAL_NAME_num(sanNames);
        certificateNames << "SAN(s): ";
        for (int i = 0; i < sanNamesList; i++) {
            const GENERAL_NAME* currentName = sk_GENERAL_NAME_value(sanNames, i);
            if (currentName && currentName->type == GEN_DNS) {
                std::string dnsName(
                    reinterpret_cast<char*>(ASN1_STRING_data(currentName->d.dNSName)));
                auto swCIDRDNSName = CIDR::parse(dnsName);
                if (swCIDRDNSName.isOK()) {
                    warning() << "You have an IP Address in the DNS Name field on your "
                                 "certificate. This formulation is deprecated.";
                    if (swCIDRRemoteHost.isOK() &&
                        swCIDRRemoteHost.getValue() == swCIDRDNSName.getValue()) {
                        sanMatch = true;
                        break;
                    }
                }
                if (hostNameMatchForX509Certificates(remoteHost, dnsName)) {
                    sanMatch = true;
                    break;
                }
                certificateNames << std::string(dnsName) << ", ";
            } else if (currentName && currentName->type == GEN_IPADD) {
                auto ipAddrStruct = currentName->d.iPAddress;
                struct sockaddr_storage ss;
                memset(&ss, 0, sizeof(ss));
                if (ipAddrStruct->length == 4) {
                    struct sockaddr_in* sa = reinterpret_cast<struct sockaddr_in*>(&ss);
                    sa->sin_family = AF_INET;
                    memcpy(&(sa->sin_addr), ipAddrStruct->data, ipAddrStruct->length);
                } else if (ipAddrStruct->length == 16) {
                    struct sockaddr_in6* sa = reinterpret_cast<struct sockaddr_in6*>(&ss);
                    sa->sin6_family = AF_INET6;
                    memcpy(&(sa->sin6_addr), ipAddrStruct->data, ipAddrStruct->length);
                }
                auto ipAddress = SockAddr(ss, sizeof(ss)).getAddr();
                auto swIpAddress = CIDR::parse(ipAddress);
                if (swCIDRRemoteHost.isOK() && swIpAddress.isOK() &&
                    swCIDRRemoteHost.getValue() == swIpAddress.getValue()) {
                    sanMatch = true;
                    break;
                }
                certificateNames << ipAddress << ", ";
            }
        }
        sk_GENERAL_NAME_pop_free(sanNames, GENERAL_NAME_free);
    }

    if (!sanMatch) {
        // If SAN doesn't match, check to see if CN does.
        // If it does and no SAN was provided, that's a match.
        // Anything else is a varying degree of failure.
        auto swCN = peerSubject.getOID(kOID_CommonName);
        if (swCN.isOK()) {
            auto commonName = std::move(swCN.getValue());
            certificateNames << "CN: " << commonName;
            if (hostNameMatchForX509Certificates(remoteHost, commonName)) {
                if (sanNames) {
                    certificateNames << " would have matched, but was overridden by SAN";
                } else {
                    cnMatch = true;
                }
            }
        } else if (!sanNames) {
            certificateNames << "No Common Name (CN) or Subject Alternate Names (SAN) found";
        }
    }

    if (!sanMatch && !cnMatch) {
        StringBuilder msgBuilder;
        msgBuilder << "The server certificate does not match the host name. Hostname: "
                   << remoteHost << " does not match " << certificateNames.str();
        std::string msg = msgBuilder.str();
        if (_allowInvalidCertificates || _allowInvalidHostnames || isUnixDomainSocket(remoteHost)) {
            warning() << msg;
        } else {
            error() << msg;
            return Status(ErrorCodes::SSLHandshakeFailed, msg);
        }
    }

    return SSLPeerInfo(peerSubject);
}


SSLPeerInfo SSLManagerOpenSSL::parseAndValidatePeerCertificateDeprecated(
    const SSLConnectionInterface* connInterface,
    const std::string& remoteHost,
    const HostAndPort& hostForLogging) {
    const SSLConnectionOpenSSL* conn = checked_cast<const SSLConnectionOpenSSL*>(connInterface);

    auto swPeerSubjectName = parseAndValidatePeerCertificate(conn->ssl, remoteHost, hostForLogging);
    // We can't use uassertStatusOK here because we need to throw a NetworkException.
    if (!swPeerSubjectName.isOK()) {
        throwSocketError(SocketErrorKind::CONNECT_ERROR, swPeerSubjectName.getStatus().reason());
    }
    return swPeerSubjectName.getValue();
}

StatusWith<stdx::unordered_set<RoleName>> SSLManagerOpenSSL::_parsePeerRoles(X509* peerCert) const {
    // exts is owned by the peerCert
    const STACK_OF(X509_EXTENSION)* exts = X509_get0_extensions(peerCert);

    int extCount = 0;
    if (exts) {
        extCount = sk_X509_EXTENSION_num(exts);
    }

    ASN1_OBJECT* rolesObj = OBJ_nid2obj(_rolesNid);

    // Search all certificate extensions for our own
    stdx::unordered_set<RoleName> roles;
    for (int i = 0; i < extCount; i++) {
        X509_EXTENSION* ex = sk_X509_EXTENSION_value(exts, i);
        ASN1_OBJECT* obj = X509_EXTENSION_get_object(ex);

        if (!OBJ_cmp(obj, rolesObj)) {
            // We've found an extension which has our roles OID
            ASN1_OCTET_STRING* data = X509_EXTENSION_get_data(ex);

            return parsePeerRoles(
                ConstDataRange(reinterpret_cast<char*>(data->data),
                               reinterpret_cast<char*>(data->data) + data->length));
        }
    }

    return roles;
}

std::string SSLManagerInterface::getSSLErrorMessage(int code) {
    // 120 from the SSL documentation for ERR_error_string
    static const size_t msglen = 120;

    char msg[msglen];
    ERR_error_string_n(code, msg, msglen);
    return msg;
}

void SSLManagerOpenSSL::_handleSSLError(SSLConnectionOpenSSL* conn, int ret) {
    int code = SSL_get_error(conn->ssl, ret);
    int err = ERR_get_error();
    SocketErrorKind errToThrow = SocketErrorKind::CONNECT_ERROR;

    switch (code) {
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
            // should not happen because we turned on AUTO_RETRY
            // However, it turns out this CAN happen during a connect, if the other side
            // accepts the socket connection but fails to do the SSL handshake in a timely
            // manner.
            errToThrow = (code == SSL_ERROR_WANT_READ) ? SocketErrorKind::RECV_ERROR
                                                       : SocketErrorKind::SEND_ERROR;
            error() << "SSL: " << code << ", possibly timed out during connect";
            break;

        case SSL_ERROR_ZERO_RETURN:
            // TODO: Check if we can avoid throwing an exception for this condition
            // If so, change error() back to LOG(3)
            error() << "SSL network connection closed";
            break;
        case SSL_ERROR_SYSCALL:
            // If ERR_get_error returned 0, the error queue is empty
            // check the return value of the actual SSL operation
            if (err != 0) {
                error() << "SSL: " << getSSLErrorMessage(err);
            } else if (ret == 0) {
                error() << "Unexpected EOF encountered during SSL communication";
            } else {
                error() << "The SSL BIO reported an I/O error " << errnoWithDescription();
            }
            break;
        case SSL_ERROR_SSL: {
            error() << "SSL: " << getSSLErrorMessage(err);
            break;
        }

        default:
            error() << "unrecognized SSL error";
            break;
    }
    _flushNetworkBIO(conn);
    throwSocketError(errToThrow, conn->socket->remoteString());
}
}  // namespace mongo
