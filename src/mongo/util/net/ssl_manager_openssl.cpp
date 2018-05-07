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

#include <boost/algorithm/string.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stack>
#include <string>
#include <vector>

#include "mongo/base/checked_cast.h"
#include "mongo/base/init.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"
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
#include "mongo/util/net/socket_exception.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/net/ssl_types.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/text.h"

#include <openssl/asn1.h>
#include <openssl/asn1t.h>
#include <openssl/evp.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>
#if defined(_WIN32)
#include <wincrypt.h>
#elif defined(__APPLE__)
#include <Security/Security.h>
#endif

namespace mongo {

namespace {

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

// If the underlying SSL supports auto-configuration of ECDH parameters, this function will select
// it, otherwise this function will do nothing.
void setECDHModeAuto(SSL_CTX* const ctx) {
#ifdef MONGO_CONFIG_HAVE_SSL_SET_ECDH_AUTO
    ::SSL_CTX_set_ecdh_auto(ctx, true);
#endif
    std::ignore = ctx;
}

struct DHFreer {
    void operator()(DH* const dh) noexcept {
        if (dh) {
            ::DH_free(dh);
        }
    }
};
using UniqueDHParams = std::unique_ptr<DH, DHFreer>;

struct BIOFree {
    void operator()(BIO* const p) noexcept {
        // Assumes that BIO_free succeeds.
        if (p) {
            ::BIO_free(p);
        }
    }
};
using UniqueBIO = std::unique_ptr<BIO, BIOFree>;

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

// Old copies of OpenSSL will not have constants to disable protocols they don't support.
// Define them to values we can OR together safely to generically disable these protocols across
// all versions of OpenSSL.
#ifndef SSL_OP_NO_TLSv1_1
#define SSL_OP_NO_TLSv1_1 0
#endif
#ifndef SSL_OP_NO_TLSv1_2
#define SSL_OP_NO_TLSv1_2 0
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

#if OPENSSL_VERSION_NUMBER < 0x10100000L || defined(LIBRESSL_VERSION_NUMBER)
// Copies of OpenSSL after 1.1.0 define new functions for interaction with
// X509 structure. We must polyfill used definitions to interact with older
// OpenSSL versions.
const STACK_OF(X509_EXTENSION) * X509_get0_extensions(const X509* peerCert) {
    return peerCert->cert_info->extensions;
}
inline int X509_NAME_ENTRY_set(const X509_NAME_ENTRY* ne) {
    return ne->set;
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
            _mutex.emplace_back(stdx::make_unique<stdx::recursive_mutex>());
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

namespace {
// We only want to free SSL_CTX objects if they have been populated. OpenSSL seems to perform this
// check before freeing them, but because it does not document this, we should protect ourselves.
void free_ssl_context(SSL_CTX* ctx) {
    if (ctx != nullptr) {
        SSL_CTX_free(ctx);
    }
}
}  // namespace

class SSLConnectionOpenSSL : public SSLConnectionInterface {
public:
    SSL* ssl;
    BIO* networkBIO;
    BIO* internalBIO;
    Socket* socket;

    SSLConnectionOpenSSL(SSL_CTX* ctx, Socket* sock, const char* initialBytes, int len);

    ~SSLConnectionOpenSSL();

    std::string getSNIServerName() const final {
        const char* name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
        if (!name)
            return "";

        return name;
    }
};

////////////////////////////////////////////////////////////////

SimpleMutex sslManagerMtx;
SSLManagerInterface* theSSLManager = NULL;
using UniqueSSLContext = std::unique_ptr<SSL_CTX, decltype(&free_ssl_context)>;
static const int BUFFER_SIZE = 8 * 1024;
static const int DATE_LEN = 128;

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
                                                          const std::string& remoteHost) final;

    StatusWith<boost::optional<SSLPeerInfo>> parseAndValidatePeerCertificate(
        SSL* conn, const std::string& remoteHost) final;

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
    bool _weakValidation;
    bool _allowInvalidCertificates;
    bool _allowInvalidHostnames;
    SSLConfiguration _sslConfiguration;

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
                                      const std::string& keyPassword,
                                      SSLX509Name* subjectName,
                                      Date_t* serverNotAfter);


    StatusWith<stdx::unordered_set<RoleName>> _parsePeerRoles(X509* peerCert) const;

    /** @return true if was successful, otherwise false */
    bool _setupPEM(SSL_CTX* context, const std::string& keyFile, const std::string& password);

    /*
     * Set up an SSL context for certificate validation by loading a CA
     */
    Status _setupCA(SSL_CTX* context, const std::string& caFile);

    /*
     * Set up an SSL context for certificate validation by loading the system's CA store
     */
    Status _setupSystemCA(SSL_CTX* context);

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

MONGO_INITIALIZER_WITH_PREREQUISITES(SSLManager, ("SetupOpenSSL"))(InitializerContext*) {
    stdx::lock_guard<SimpleMutex> lck(sslManagerMtx);
    if (!isSSLServer || (sslGlobalParams.sslMode.load() != SSLParams::SSLMode_disabled)) {
        theSSLManager = new SSLManagerOpenSSL(sslGlobalParams, isSSLServer);
    }
    return Status::OK();
}

std::unique_ptr<SSLManagerInterface> SSLManagerInterface::create(const SSLParams& params,
                                                                 bool isServer) {
    return stdx::make_unique<SSLManagerOpenSSL>(params, isServer);
}

SSLManagerInterface* getSSLManager() {
    stdx::lock_guard<SimpleMutex> lck(sslManagerMtx);
    if (theSSLManager)
        return theSSLManager;
    return NULL;
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

    return SSLX509Name(std::move(entries));
}

SSLConnectionOpenSSL::SSLConnectionOpenSSL(SSL_CTX* context,
                                           Socket* sock,
                                           const char* initialBytes,
                                           int len)
    : socket(sock) {
    ssl = SSL_new(context);

    std::string sslErr =
        NULL != getSSLManager() ? getSSLManager()->getSSLErrorMessage(ERR_get_error()) : "";
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
    : _serverContext(nullptr, free_ssl_context),
      _clientContext(nullptr, free_ssl_context),
      _weakValidation(params.sslWeakCertificateValidation),
      _allowInvalidCertificates(params.sslAllowInvalidCertificates),
      _allowInvalidHostnames(params.sslAllowInvalidHostnames) {
    if (!_initSynchronousSSLContext(&_clientContext, params, ConnectionDirection::kOutgoing)) {
        uasserted(16768, "ssl initialization problem");
    }

    // pick the certificate for use in outgoing connections,
    std::string clientPEM, clientPassword;
    if (!isServer || params.sslClusterFile.empty()) {
        // We are either a client, or a server without a cluster key,
        // so use the PEM key file, if specified
        clientPEM = params.sslPEMKeyFile;
        clientPassword = params.sslPEMKeyPassword;
    } else {
        // We are a server with a cluster key, so use the cluster key file
        clientPEM = params.sslClusterFile;
        clientPassword = params.sslClusterPassword;
    }

    if (!clientPEM.empty()) {
        if (!_parseAndValidateCertificate(
                clientPEM, clientPassword, &_sslConfiguration.clientSubjectName, NULL)) {
            uasserted(16941, "ssl initialization problem");
        }
    }
    // SSL server specific initialization
    if (isServer) {
        if (!_initSynchronousSSLContext(&_serverContext, params, ConnectionDirection::kIncoming)) {
            uasserted(16562, "ssl initialization problem");
        }

        if (!_parseAndValidateCertificate(params.sslPEMKeyFile,
                                          params.sslPEMKeyPassword,
                                          &_sslConfiguration.serverSubjectName,
                                          &_sslConfiguration.serverCertificateExpirationDate)) {
            uasserted(16942, "ssl initialization problem");
        }

        static CertificateExpirationMonitor task =
            CertificateExpirationMonitor(_sslConfiguration.serverCertificateExpirationDate);
    }
}

int SSLManagerOpenSSL::password_cb(char* buf, int num, int rwflag, void* userdata) {
    // Unless OpenSSL misbehaves, num should always be positive
    fassert(17314, num > 0);
    invariant(userdata);
    auto pw = static_cast<const std::string*>(userdata);

    const size_t copied = pw->copy(buf, num - 1);
    buf[copied] = '\0';
    return copied;
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
    long supportedProtocols = SSL_OP_ALL | SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3;

    // Set the supported TLS protocols. Allow --sslDisabledProtocols to disable selected
    // ciphers.
    for (const SSLParams::Protocols& protocol : params.sslDisabledProtocols) {
        if (protocol == SSLParams::Protocols::TLS1_0) {
            supportedProtocols |= SSL_OP_NO_TLSv1;
        } else if (protocol == SSLParams::Protocols::TLS1_1) {
            supportedProtocols |= SSL_OP_NO_TLSv1_1;
        } else if (protocol == SSLParams::Protocols::TLS1_2) {
            supportedProtocols |= SSL_OP_NO_TLSv1_2;
        }
    }
    ::SSL_CTX_set_options(context, supportedProtocols);

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
    if (0 == ::SSL_CTX_set_session_id_context(
                 context, reinterpret_cast<unsigned char*>(&context), sizeof(context))) {
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "Can not store ssl session id context: "
                                    << getSSLErrorMessage(ERR_get_error()));
    }

    if (direction == ConnectionDirection::kOutgoing && !params.sslClusterFile.empty()) {
        ::EVP_set_pw_prompt("Enter cluster certificate passphrase");
        if (!_setupPEM(context, params.sslClusterFile, params.sslClusterPassword)) {
            return Status(ErrorCodes::InvalidSSLConfiguration, "Can not set up ssl clusterFile.");
        }
    } else if (!params.sslPEMKeyFile.empty()) {
        // Use the pemfile for everything else
        ::EVP_set_pw_prompt("Enter PEM passphrase");
        if (!_setupPEM(context, params.sslPEMKeyFile, params.sslPEMKeyPassword)) {
            return Status(ErrorCodes::InvalidSSLConfiguration, "Can not set up PEM key file.");
        }
    }

    const auto status =
        params.sslCAFile.empty() ? _setupSystemCA(context) : _setupCA(context, params.sslCAFile);
    if (!status.isOK())
        return status;

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
    setECDHModeAuto(context);

    return Status::OK();
}

bool SSLManagerOpenSSL::_initSynchronousSSLContext(UniqueSSLContext* contextPtr,
                                                   const SSLParams& params,
                                                   ConnectionDirection direction) {
    *contextPtr = UniqueSSLContext(SSL_CTX_new(SSLv23_method()), free_ssl_context);

    uassertStatusOK(initSSLContext(contextPtr->get(), params, direction));

    // If renegotiation is needed, don't return from recv() or send() until it's successful.
    // Note: this is for blocking sockets only.
    SSL_CTX_set_mode(contextPtr->get(), SSL_MODE_AUTO_RETRY);

    return true;
}

unsigned long long SSLManagerOpenSSL::_convertASN1ToMillis(ASN1_TIME* asn1time) {
    BIO* outBIO = BIO_new(BIO_s_mem());
    int timeError = ASN1_TIME_print(outBIO, asn1time);
    ON_BLOCK_EXIT(BIO_free, outBIO);

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
                                                     const std::string& keyPassword,
                                                     SSLX509Name* subjectName,
                                                     Date_t* serverCertificateExpirationDate) {
    BIO* inBIO = BIO_new(BIO_s_file());
    if (inBIO == NULL) {
        error() << "failed to allocate BIO object: " << getSSLErrorMessage(ERR_get_error());
        return false;
    }

    ON_BLOCK_EXIT(BIO_free, inBIO);
    if (BIO_read_filename(inBIO, keyFile.c_str()) <= 0) {
        error() << "cannot read key file when setting subject name: " << keyFile << ' '
                << getSSLErrorMessage(ERR_get_error());
        return false;
    }

    // Callback will not manipulate the password, so const_cast is safe.
    X509* x509 = PEM_read_bio_X509(inBIO,
                                   NULL,
                                   &SSLManagerOpenSSL::password_cb,
                                   const_cast<void*>(static_cast<const void*>(&keyPassword)));
    if (x509 == NULL) {
        error() << "cannot retrieve certificate from keyfile: " << keyFile << ' '
                << getSSLErrorMessage(ERR_get_error());
        return false;
    }
    ON_BLOCK_EXIT(X509_free, x509);

    *subjectName = getCertificateSubjectX509Name(x509);
    if (serverCertificateExpirationDate != NULL) {
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
                                  const std::string& password) {
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
    const auto bioGuard = MakeGuard([&inBio]() { BIO_free(inBio); });

    if (BIO_read_filename(inBio, keyFile.c_str()) <= 0) {
        error() << "cannot read PEM key file: " << keyFile << ' '
                << getSSLErrorMessage(ERR_get_error());
        return false;
    }

    // If password is empty, use default OpenSSL callback, which uses the terminal
    // to securely request the password interactively from the user.
    decltype(&SSLManagerOpenSSL::password_cb) password_cb = nullptr;
    void* userdata = nullptr;
    if (!password.empty()) {
        password_cb = &SSLManagerOpenSSL::password_cb;
        // SSLManagerOpenSSL::password_cb will not manipulate the password, so const_cast is safe.
        userdata = const_cast<void*>(static_cast<const void*>(&password));
    }
    EVP_PKEY* privateKey = PEM_read_bio_PrivateKey(inBio, nullptr, password_cb, userdata);
    if (!privateKey) {
        error() << "cannot read PEM key file: " << keyFile << ' '
                << getSSLErrorMessage(ERR_get_error());
        return false;
    }
    const auto privateKeyGuard = MakeGuard([&privateKey]() { EVP_PKEY_free(privateKey); });

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
    if (certNames == NULL) {
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "cannot read certificate authority file: " << caFile << " "
                                    << getSSLErrorMessage(ERR_get_error()));
    }
    SSL_CTX_set_client_CA_list(context, certNames);

    // Load trusted CA
    if (SSL_CTX_load_verify_locations(context, caFile.c_str(), NULL) != 1) {
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
    HCERTSTORE systemStore = CertOpenStore(CERT_STORE_PROV_SYSTEM_W,
                                           0,
                                           NULL,
                                           storeLocation | CERT_STORE_READONLY_FLAG,
                                           const_cast<LPWSTR>(storeName));
    if (systemStore == NULL) {
        return {ErrorCodes::InvalidSSLConfiguration,
                str::stream() << "error opening system CA store: " << errnoWithDescription()};
    }
    auto systemStoreGuard = MakeGuard([systemStore]() { CertCloseStore(systemStore, 0); });

    PCCERT_CONTEXT certCtx = NULL;
    while ((certCtx = CertEnumCertificatesInStore(systemStore, certCtx)) != NULL) {
        auto certBytes = static_cast<const unsigned char*>(certCtx->pbCertEncoded);
        X509* x509Obj = d2i_X509(NULL, &certBytes, certCtx->cbCertEncoded);
        if (x509Obj == NULL) {
            return {ErrorCodes::InvalidSSLConfiguration,
                    str::stream() << "Error parsing X509 object from Windows certificate store"
                                  << SSLManagerInterface::getSSLErrorMessage(ERR_get_error())};
        }
        const auto x509ObjGuard = MakeGuard([&x509Obj]() { X509_free(x509Obj); });

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

Status importKeychainToX509_STORE(X509_STORE* verifyStore) {
    CFArrayRef result;
    OSStatus status;

    // This copies all the certificates trusted by the system (regardless of what keychain they're
    // attached to) into a CFArray.
    if ((status = SecTrustCopyAnchorCertificates(&result)) != 0) {
        return {ErrorCodes::InvalidSSLConfiguration,
                str::stream() << "Error enumerating certificates: " << OSStatusToString(status)};
    }
    const auto resultGuard = makeCFTypeRefHolder(result);

    for (CFIndex i = 0; i < CFArrayGetCount(result); i++) {
        SecCertificateRef cert =
            static_cast<SecCertificateRef>(const_cast<void*>(CFArrayGetValueAtIndex(result, i)));

        auto rawData = makeCFTypeRefHolder(SecCertificateCopyData(cert));
        if (!rawData) {
            return {ErrorCodes::InvalidSSLConfiguration,
                    str::stream() << "Error enumerating certificates: "
                                  << OSStatusToString(status)};
        }
        const uint8_t* rawDataPtr = CFDataGetBytePtr(rawData);

        // Parse an openssl X509 object from each returned certificate
        X509* x509Cert = d2i_X509(nullptr, &rawDataPtr, CFDataGetLength(rawData));
        if (!x509Cert) {
            return {ErrorCodes::InvalidSSLConfiguration,
                    str::stream() << "Error parsing X509 certificate from system keychain: "
                                  << ERR_reason_error_string(ERR_peek_last_error())};
        }
        const auto x509CertGuard = MakeGuard([&x509Cert]() { X509_free(x509Cert); });

        // Add the parsed X509 object to the X509_STORE verification store
        if (X509_STORE_add_cert(verifyStore, x509Cert) != 1) {
            auto status = checkX509_STORE_error();
            if (!status.isOK())
                return status;
        }
    }

    return Status::OK();
}
#endif

Status SSLManagerOpenSSL::_setupSystemCA(SSL_CTX* context) {
#if !defined(_WIN32) && !defined(__APPLE__)
    // On non-Windows/non-Apple platforms, the OpenSSL libraries should have been configured
    // with default locations for CA certificates.
    if (SSL_CTX_set_default_verify_paths(context) != 1) {
        return {ErrorCodes::InvalidSSLConfiguration,
                str::stream() << "error loading system CA certificates "
                              << "(default certificate file: "
                              << X509_get_default_cert_file()
                              << ", "
                              << "default certificate path: "
                              << X509_get_default_cert_dir()
                              << ")"};
    }
    return Status::OK();
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
    return importKeychainToX509_STORE(verifyStore);
#endif
#endif
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
    std::unique_ptr<SSLConnectionOpenSSL> sslConn =
        stdx::make_unique<SSLConnectionOpenSSL>(_clientContext.get(), socket, (const char*)NULL, 0);

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
        stdx::make_unique<SSLConnectionOpenSSL>(_serverContext.get(), socket, initialBytes, len);

    int ret;
    do {
        ret = ::SSL_accept(sslConn->ssl);
    } while (!_doneWithSSLOp(sslConn.get(), ret));

    if (ret != 1)
        _handleSSLError(sslConn.get(), ret);

    return sslConn.release();
}

StatusWith<boost::optional<SSLPeerInfo>> SSLManagerOpenSSL::parseAndValidatePeerCertificate(
    SSL* conn, const std::string& remoteHost) {
    if (!_sslConfiguration.hasCA && isSSLServer)
        return {boost::none};

    X509* peerCert = SSL_get_peer_certificate(conn);

    if (NULL == peerCert) {  // no certificate presented by peer
        if (_weakValidation) {
            warning() << "no SSL certificate provided by peer";
        } else {
            auto msg = "no SSL certificate provided by peer; connection rejected";
            error() << msg;
            return Status(ErrorCodes::SSLHandshakeFailed, msg);
        }
        return {boost::none};
    }
    ON_BLOCK_EXIT(X509_free, peerCert);

    long result = SSL_get_verify_result(conn);

    if (result != X509_V_OK) {
        if (_allowInvalidCertificates) {
            warning() << "SSL peer certificate validation failed: "
                      << X509_verify_cert_error_string(result);
            return {boost::none};
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

    // If this is an SSL client context (on a MongoDB server or client)
    // perform hostname validation of the remote server
    if (remoteHost.empty()) {
        return boost::make_optional(
            SSLPeerInfo(peerSubject, std::move(swPeerCertificateRoles.getValue())));
    }

    // Try to match using the Subject Alternate Name, if it exists.
    // RFC-2818 requires the Subject Alternate Name to be used if present.
    // Otherwise, the most specific Common Name field in the subject field
    // must be used.

    bool sanMatch = false;
    bool cnMatch = false;
    StringBuilder certificateNames;

    STACK_OF(GENERAL_NAME)* sanNames = static_cast<STACK_OF(GENERAL_NAME)*>(
        X509_get_ext_d2i(peerCert, NID_subject_alt_name, NULL, NULL));

    if (sanNames != NULL) {
        int sanNamesList = sk_GENERAL_NAME_num(sanNames);
        certificateNames << "SAN(s): ";
        for (int i = 0; i < sanNamesList; i++) {
            const GENERAL_NAME* currentName = sk_GENERAL_NAME_value(sanNames, i);
            if (currentName && currentName->type == GEN_DNS) {
                char* dnsName = reinterpret_cast<char*>(ASN1_STRING_data(currentName->d.dNSName));
                if (hostNameMatchForX509Certificates(remoteHost, dnsName)) {
                    sanMatch = true;
                    break;
                }
                certificateNames << std::string(dnsName) << " ";
            }
        }
        sk_GENERAL_NAME_pop_free(sanNames, GENERAL_NAME_free);
    } else {
        // If Subject Alternate Name (SAN) doesn't exist and Common Name (CN) does,
        // check Common Name.
        auto swCN = peerSubject.getOID(kOID_CommonName);
        if (swCN.isOK()) {
            auto commonName = std::move(swCN.getValue());
            if (hostNameMatchForX509Certificates(remoteHost, commonName)) {
                cnMatch = true;
            }
            certificateNames << "CN: " << commonName;
        } else {
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

    return boost::make_optional(SSLPeerInfo(peerSubject, stdx::unordered_set<RoleName>()));
}


SSLPeerInfo SSLManagerOpenSSL::parseAndValidatePeerCertificateDeprecated(
    const SSLConnectionInterface* connInterface, const std::string& remoteHost) {
    const SSLConnectionOpenSSL* conn = checked_cast<const SSLConnectionOpenSSL*>(connInterface);

    auto swPeerSubjectName = parseAndValidatePeerCertificate(conn->ssl, remoteHost);
    // We can't use uassertStatusOK here because we need to throw a NetworkException.
    if (!swPeerSubjectName.isOK()) {
        throwSocketError(SocketErrorKind::CONNECT_ERROR, swPeerSubjectName.getStatus().reason());
    }
    return swPeerSubjectName.getValue().get_value_or(SSLPeerInfo());
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

    switch (code) {
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
            // should not happen because we turned on AUTO_RETRY
            // However, it turns out this CAN happen during a connect, if the other side
            // accepts the socket connection but fails to do the SSL handshake in a timely
            // manner.
            error() << "SSL: " << code << ", possibly timed out during connect";
            break;

        case SSL_ERROR_ZERO_RETURN:
            // TODO: Check if we can avoid throwing an exception for this condition
            LOG(3) << "SSL network connection closed";
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
    throwSocketError(SocketErrorKind::CONNECT_ERROR, "");
}
}  // namespace mongo
