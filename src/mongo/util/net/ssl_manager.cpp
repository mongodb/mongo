/*    Copyright 2009 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
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
#include "mongo/util/hex.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/private/ssl_expiration.h"
#include "mongo/util/net/sock.h"
#include "mongo/util/net/socket_exception.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/net/ssl_types.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/text.h"

#ifdef MONGO_CONFIG_SSL
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
#endif

namespace mongo {

namespace {

std::string removeFQDNRoot(std::string name) {
    if (name.back() == '.') {
        name.pop_back();
    }
    return name;
};


// Because the hostname having a slash is used by `mongo::SockAddr` to determine if a hostname is a
// Unix Domain Socket endpoint, this function uses the same logic.  (See
// `mongo::SockAddr::Sockaddr(StringData, int, sa_family_t)`).  A user explicitly specifying a Unix
// Domain Socket in the present working directory, through a code path which supplies `sa_family_t`
// as `AF_UNIX` will cause this code to lie.  This will, in turn, cause the
// `SSLManager::parseAndValidatePeerCertificate` code to believe a socket is a host, which will then
// cause a connection failure if and only if that domain socket also has a certificate for SSL and
// the connection is an SSL connection.
bool isUnixDomainSocket(const std::string& hostname) {
    return end(hostname) != std::find(begin(hostname), end(hostname), '/');
}

const transport::Session::Decoration<SSLPeerInfo> peerInfoForSession =
    transport::Session::declareDecoration<SSLPeerInfo>();

/**
 * Configurable via --setParameter disableNonSSLConnectionLogging=true. If false (default)
 * if the sslMode is set to preferSSL, we will log connections that are not using SSL.
 * If true, such log messages will be suppressed.
 */
ExportedServerParameter<bool, ServerParameterType::kStartupOnly>
    disableNonSSLConnectionLoggingParameter(ServerParameterSet::getGlobal(),
                                            "disableNonSSLConnectionLogging",
                                            &sslGlobalParams.disableNonSSLConnectionLogging);

ExportedServerParameter<std::string, ServerParameterType::kStartupOnly>
    setDiffieHellmanParameterPEMFile(ServerParameterSet::getGlobal(),
                                     "opensslDiffieHellmanParameters",
                                     &sslGlobalParams.sslPEMTempDHParam);

ExportedServerParameter<bool, ServerParameterType::kStartupOnly>
    suppressNoTLSPeerCertificateWarning(ServerParameterSet::getGlobal(),
                                        "suppressNoTLSPeerCertificateWarning",
                                        &sslGlobalParams.suppressNoTLSPeerCertificateWarning);

ExportedServerParameter<bool, ServerParameterType::kStartupOnly> sslWithholdClientCertificate(
    ServerParameterSet::getGlobal(),
    "sslWithholdClientCertificate",
    &sslGlobalParams.tlsWithholdClientCertificate);

}  // namespace

SSLPeerInfo& SSLPeerInfo::forSession(const transport::SessionHandle& session) {
    return peerInfoForSession(session.get());
}

SSLParams sslGlobalParams;

const SSLParams& getSSLGlobalParams() {
    return sslGlobalParams;
}

class OpenSSLCipherConfigParameter
    : public ExportedServerParameter<std::string, ServerParameterType::kStartupOnly> {
public:
    OpenSSLCipherConfigParameter()
        : ExportedServerParameter<std::string, ServerParameterType::kStartupOnly>(
              ServerParameterSet::getGlobal(),
              "opensslCipherConfig",
              &sslGlobalParams.sslCipherConfig) {}
    Status validate(const std::string& potentialNewValue) final {
        if (!sslGlobalParams.sslCipherConfig.empty()) {
            return Status(
                ErrorCodes::BadValue,
                "opensslCipherConfig setParameter is incompatible with net.ssl.sslCipherConfig");
        }
        // Note that there is very little validation that we can do here.
        // OpenSSL exposes no API to validate a cipher config string. The only way to figure out
        // what a string maps to is to make an SSL_CTX object, set the string on it, then parse the
        // resulting STACK_OF object. If provided an invalid entry in the string, it will silently
        // ignore it. Because an entry in the string may map to multiple ciphers, or remove ciphers
        // from the final set produced by the full string, we can't tell if any entry failed
        // to parse.
        return Status::OK();
    }
} openSSLCipherConfig;
}  // namespace mongo

#ifdef MONGO_CONFIG_SSL
namespace mongo {
namespace {

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

#if OPENSSL_VERSION_NUMBER < 0x10100000L || \
    (defined(LIBRESSL_VERSION_NUMBER) && LIBRESSL_VERSION_NUMBER < 0x2070000fL)
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

////////////////////////////////////////////////////////////////

SimpleMutex sslManagerMtx;
SSLManagerInterface* theSSLManager = NULL;
using UniqueSSLContext = std::unique_ptr<SSL_CTX, decltype(&free_ssl_context)>;
static const int BUFFER_SIZE = 8 * 1024;
static const int DATE_LEN = 128;

class SSLManager : public SSLManagerInterface {
public:
    explicit SSLManager(const SSLParams& params, bool isServer);

    /**
     * Initializes an OpenSSL context according to the provided settings. Only settings which are
     * acceptable on non-blocking connections are set.
     */
    Status initSSLContext(SSL_CTX* context,
                          const SSLParams& params,
                          ConnectionDirection direction) final;

    virtual SSLConnection* connect(Socket* socket);

    virtual SSLConnection* accept(Socket* socket, const char* initialBytes, int len);

    virtual SSLPeerInfo parseAndValidatePeerCertificateDeprecated(const SSLConnection* conn,
                                                                  const std::string& remoteHost);

    StatusWith<boost::optional<SSLPeerInfo>> parseAndValidatePeerCertificate(
        SSL* conn, const std::string& remoteHost) final;

    virtual const SSLConfiguration& getSSLConfiguration() const {
        return _sslConfiguration;
    }

    virtual int SSL_read(SSLConnection* conn, void* buf, int num);

    virtual int SSL_write(SSLConnection* conn, const void* buf, int num);

    virtual unsigned long ERR_get_error();

    virtual char* ERR_error_string(unsigned long e, char* buf);

    virtual int SSL_get_error(const SSLConnection* conn, int ret);

    virtual int SSL_shutdown(SSLConnection* conn);

    virtual void SSL_free(SSLConnection* conn);

private:
    const int _rolesNid = OBJ_create(mongodbRolesOID.identifier.c_str(),
                                     mongodbRolesOID.shortDescription.c_str(),
                                     mongodbRolesOID.longDescription.c_str());
    UniqueSSLContext _serverContext;  // SSL context for incoming connections
    UniqueSSLContext _clientContext;  // SSL context for outgoing connections
    bool _weakValidation;
    bool _allowInvalidCertificates;
    bool _allowInvalidHostnames;
    bool _suppressNoCertificateWarning;
    SSLConfiguration _sslConfiguration;

    /**
     * creates an SSL object to be used for this file descriptor.
     * caller must SSL_free it.
     */
    SSL* _secure(SSL_CTX* context, int fd);

    /**
     * Given an error code from an SSL-type IO function, logs an
     * appropriate message and throws a SocketException.
     */
    MONGO_COMPILER_NORETURN void _handleSSLError(SSLConnection* conn, int ret);

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
    bool _doneWithSSLOp(SSLConnection* conn, int status);

    /*
     * Send and receive network data
     */
    void _flushNetworkBIO(SSLConnection* conn);

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
        theSSLManager = new SSLManager(sslGlobalParams, isSSLServer);
    }
    return Status::OK();
}

std::unique_ptr<SSLManagerInterface> SSLManagerInterface::create(const SSLParams& params,
                                                                 bool isServer) {
    return stdx::make_unique<SSLManager>(params, isServer);
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

SSLConnection::SSLConnection(SSL_CTX* context, Socket* sock, const char* initialBytes, int len)
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
            throw SocketException(SocketException::RECV_ERROR, socket->remoteString());
        }
    }
}

SSLConnection::~SSLConnection() {
    if (ssl) {  // The internalBIO is automatically freed as part of SSL_free
        SSL_free(ssl);
    }
    if (networkBIO) {
        BIO_free(networkBIO);
    }
}

namespace {
std::string x509OidToShortName(const std::string& name) {
    const auto nid = OBJ_txt2nid(name.c_str());
    if (nid == 0) {
        return name;
    }
    const auto* sn = OBJ_nid2sn(nid);
    if (!sn) {
        return name;
    }
    return sn;
}

// Characters that need to be escaped in RFC 2253
const std::array<char, 7> rfc2253EscapeChars = {',', '+', '"', '\\', '<', '>', ';'};

// See section "2.4 Converting an AttributeValue from ASN.1 to a String" in RFC 2243
std::string escapeRfc2253(StringData str) {
    std::string ret;

    if (str.size() > 0) {
        size_t pos = 0;

        // a space or "#" character occurring at the beginning of the string
        if (str[0] == ' ') {
            ret = "\\ ";
            pos = 1;
        } else if (str[0] == '#') {
            ret = "\\#";
            pos = 1;
        }

        while (pos < str.size()) {
            if (static_cast<signed char>(str[pos]) < 0) {
                ret += '\\';
                ret += integerToHex(str[pos]);
            } else {
                if (std::find(rfc2253EscapeChars.cbegin(), rfc2253EscapeChars.cend(), str[pos]) !=
                    rfc2253EscapeChars.cend()) {
                    ret += '\\';
                }

                ret += str[pos];
            }
            ++pos;
        }

        // a space character occurring at the end of the string
        if (ret.size() > 2 && ret[ret.size() - 1] == ' ') {
            ret[ret.size() - 1] = '\\';
            ret += ' ';
        }
    }

    return ret;
}

TLSVersionCounts tlsVersionCounts;

}  // namespace

TLSVersionCounts& TLSVersionCounts::get() {
    return tlsVersionCounts;
}

StatusWith<std::string> SSLX509Name::getOID(StringData oid) const {
    for (const auto& rdn : _entries) {
        for (const auto& entry : rdn) {
            if (entry.oid == oid) {
                return entry.value;
            }
        }
    }
    return {ErrorCodes::KeyNotFound, "OID does not exist"};
}

StringBuilder& operator<<(StringBuilder& os, const SSLX509Name& name) {
    std::string comma;
    for (const auto& rdn : name._entries) {
        std::string plus;
        os << comma;
        for (const auto& entry : rdn) {
            os << plus << x509OidToShortName(entry.oid) << "=" << escapeRfc2253(entry.value);
            plus = "+";
        }
        comma = ",";
    }
    return os;
}

std::string SSLX509Name::toString() const {
    StringBuilder os;
    os << *this;
    return os.str();
}

namespace {
void canonicalizeClusterDN(std::vector<std::string>* dn) {
    // remove all RDNs we don't care about
    for (size_t i = 0; i < dn->size(); i++) {
        std::string& comp = dn->at(i);
        boost::algorithm::trim(comp);
        if (!mongoutils::str::startsWith(comp.c_str(), "DC=") &&
            !mongoutils::str::startsWith(comp.c_str(), "O=") &&
            !mongoutils::str::startsWith(comp.c_str(), "OU=")) {
            dn->erase(dn->begin() + i);
            i--;
        }
    }
    std::stable_sort(dn->begin(), dn->end());
}

constexpr StringData kOID_DC = "0.9.2342.19200300.100.1.25"_sd;
constexpr StringData kOID_O = "2.5.4.10"_sd;
constexpr StringData kOID_OU = "2.5.4.11"_sd;

std::vector<SSLX509Name::Entry> canonicalizeClusterDN(
    const std::vector<std::vector<SSLX509Name::Entry>>& entries) {
    std::vector<SSLX509Name::Entry> ret;

    for (const auto& rdn : entries) {
        for (const auto& entry : rdn) {
            if ((entry.oid != kOID_DC) && (entry.oid != kOID_O) && (entry.oid != kOID_OU)) {
                continue;
            }
            ret.push_back(entry);
        }
    }
    std::stable_sort(ret.begin(), ret.end());
    return ret;
}
}  // namespace

/**
 * The behavior of isClusterMember() is subtly different when passed
 * an SSLX509Name versus a StringData.
 *
 * The SSLX509Name version (immediately below) compares distinguished
 * names in their raw, unescaped forms and provides a more reliable match.
 *
 * The StringData version attempts to do a simplified string compare
 * with the serialized version of the server subject name.
 *
 * Because escaping is not checked in the StringData version,
 * some not-strictly matching RDNs will appear to share O/OU/DC with the
 * server subject name.  Therefore, that variant should be called with care.
 */
bool SSLConfiguration::isClusterMember(const SSLX509Name& subject) const {
    auto client = canonicalizeClusterDN(subject._entries);
    auto server = canonicalizeClusterDN(serverSubjectName._entries);

    return !client.empty() && (client == server);
}

bool SSLConfiguration::isClusterMember(StringData subjectName) const {
    std::vector<std::string> clientRDN = StringSplitter::split(subjectName.toString(), ",");
    std::vector<std::string> serverRDN = StringSplitter::split(serverSubjectName.toString(), ",");

    canonicalizeClusterDN(&clientRDN);
    canonicalizeClusterDN(&serverRDN);

    return !clientRDN.empty() && (clientRDN == serverRDN);
}

BSONObj SSLConfiguration::getServerStatusBSON() const {
    BSONObjBuilder security;
    security.append("SSLServerSubjectName", serverSubjectName.toString());
    security.appendBool("SSLServerHasCertificateAuthority", hasCA);
    security.appendDate("SSLServerCertificateExpirationDate", serverCertificateExpirationDate);
    return security.obj();
}

SSLManagerInterface::~SSLManagerInterface() {}

SSLManager::SSLManager(const SSLParams& params, bool isServer)
    : _serverContext(nullptr, free_ssl_context),
      _clientContext(nullptr, free_ssl_context),
      _weakValidation(params.sslWeakCertificateValidation),
      _allowInvalidCertificates(params.sslAllowInvalidCertificates),
      _allowInvalidHostnames(params.sslAllowInvalidHostnames),
      _suppressNoCertificateWarning(params.suppressNoTLSPeerCertificateWarning) {
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

int SSLManager::password_cb(char* buf, int num, int rwflag, void* userdata) {
    // Unless OpenSSL misbehaves, num should always be positive
    fassert(17314, num > 0);
    invariant(userdata);
    auto pw = static_cast<const std::string*>(userdata);

    const size_t copied = pw->copy(buf, num - 1);
    buf[copied] = '\0';
    return copied;
}

int SSLManager::verify_cb(int ok, X509_STORE_CTX* ctx) {
    return 1;  // always succeed; we will catch the error in our get_verify_result() call
}

int SSLManager::SSL_read(SSLConnection* conn, void* buf, int num) {
    int status;
    do {
        status = ::SSL_read(conn->ssl, buf, num);
    } while (!_doneWithSSLOp(conn, status));

    if (status <= 0)
        _handleSSLError(conn, status);
    return status;
}

int SSLManager::SSL_write(SSLConnection* conn, const void* buf, int num) {
    int status;
    do {
        status = ::SSL_write(conn->ssl, buf, num);
    } while (!_doneWithSSLOp(conn, status));

    if (status <= 0)
        _handleSSLError(conn, status);
    return status;
}

unsigned long SSLManager::ERR_get_error() {
    return ::ERR_get_error();
}

char* SSLManager::ERR_error_string(unsigned long e, char* buf) {
    return ::ERR_error_string(e, buf);
}

int SSLManager::SSL_get_error(const SSLConnection* conn, int ret) {
    return ::SSL_get_error(conn->ssl, ret);
}

int SSLManager::SSL_shutdown(SSLConnection* conn) {
    int status;
    do {
        status = ::SSL_shutdown(conn->ssl);
    } while (!_doneWithSSLOp(conn, status));

    if (status < 0)
        _handleSSLError(conn, status);
    return status;
}

void SSLManager::SSL_free(SSLConnection* conn) {
    return ::SSL_free(conn->ssl);
}

Status SSLManager::initSSLContext(SSL_CTX* context,
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

    if (direction == ConnectionDirection::kOutgoing && params.tlsWithholdClientCertificate) {
        // Do not send a client certificate if they have been suppressed.

    } else if (direction == ConnectionDirection::kOutgoing && !params.sslClusterFile.empty()) {
        // Use the configured clusterFile as our client certificate.
        ::EVP_set_pw_prompt("Enter cluster certificate passphrase");
        if (!_setupPEM(context, params.sslClusterFile, params.sslClusterPassword)) {
            return Status(ErrorCodes::InvalidSSLConfiguration, "Can not set up ssl clusterFile.");
        }

    } else if (!params.sslPEMKeyFile.empty()) {
        // Use the base pemKeyFile for any other outgoing connections,
        // as well as all incoming connections.
        ::EVP_set_pw_prompt("Enter PEM passphrase");
        if (!_setupPEM(context, params.sslPEMKeyFile, params.sslPEMKeyPassword)) {
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
    setECDHModeAuto(context);

    return Status::OK();
}

bool SSLManager::_initSynchronousSSLContext(UniqueSSLContext* contextPtr,
                                            const SSLParams& params,
                                            ConnectionDirection direction) {
    *contextPtr = UniqueSSLContext(SSL_CTX_new(SSLv23_method()), free_ssl_context);

    uassertStatusOK(initSSLContext(contextPtr->get(), params, direction));

    // If renegotiation is needed, don't return from recv() or send() until it's successful.
    // Note: this is for blocking sockets only.
    SSL_CTX_set_mode(contextPtr->get(), SSL_MODE_AUTO_RETRY);

    return true;
}

unsigned long long SSLManager::_convertASN1ToMillis(ASN1_TIME* asn1time) {
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

bool SSLManager::_parseAndValidateCertificate(const std::string& keyFile,
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
                                   &SSLManager::password_cb,
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

bool SSLManager::_setupPEM(SSL_CTX* context,
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
    decltype(&SSLManager::password_cb) password_cb = nullptr;
    void* userdata = nullptr;
    if (!password.empty()) {
        password_cb = &SSLManager::password_cb;
        // SSLManager::password_cb will not manipulate the password, so const_cast is safe.
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

Status SSLManager::_setupCA(SSL_CTX* context, const std::string& caFile) {
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
    SSL_CTX_set_verify(context, SSL_VERIFY_PEER, &SSLManager::verify_cb);
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

Status SSLManager::_setupSystemCA(SSL_CTX* context) {
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

bool SSLManager::_setupCRL(SSL_CTX* context, const std::string& crlFile) {
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
void SSLManager::_flushNetworkBIO(SSLConnection* conn) {
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
            throw SocketException(SocketException::RECV_ERROR, conn->socket->remoteString());
        }
    }
}

bool SSLManager::_doneWithSSLOp(SSLConnection* conn, int status) {
    int sslErr = SSL_get_error(conn, status);
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

SSLConnection* SSLManager::connect(Socket* socket) {
    std::unique_ptr<SSLConnection> sslConn =
        stdx::make_unique<SSLConnection>(_clientContext.get(), socket, (const char*)NULL, 0);

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

SSLConnection* SSLManager::accept(Socket* socket, const char* initialBytes, int len) {
    std::unique_ptr<SSLConnection> sslConn =
        stdx::make_unique<SSLConnection>(_serverContext.get(), socket, initialBytes, len);

    int ret;
    do {
        ret = ::SSL_accept(sslConn->ssl);
    } while (!_doneWithSSLOp(sslConn.get(), ret));

    if (ret != 1)
        _handleSSLError(sslConn.get(), ret);

    return sslConn.release();
}


void recordTLSVersion(const SSL* conn) {
    int protocol = SSL_version(conn);

    auto& counts = mongo::TLSVersionCounts::get();
    switch (protocol) {
        case TLS1_VERSION:
            counts.tls10.addAndFetch(1);
            break;
        case TLS1_1_VERSION:
            counts.tls11.addAndFetch(1);
            break;
        case TLS1_2_VERSION:
            counts.tls12.addAndFetch(1);
            break;
#ifdef TLS1_3_VERSION
        case TLS1_3_VERSION:
            counts.tls13.addAndFetch(1);
            break;
#endif
        default:
            // Do nothing
            break;
    }
}

StatusWith<boost::optional<SSLPeerInfo>> SSLManager::parseAndValidatePeerCertificate(
    SSL* conn, const std::string& remoteHost) {

    recordTLSVersion(conn);

    if (!_sslConfiguration.hasCA && isSSLServer)
        return {boost::none};

    X509* peerCert = SSL_get_peer_certificate(conn);

    if (NULL == peerCert) {  // no certificate presented by peer
        if (_weakValidation) {
            // do not give warning if certificate warnings are suppressed
            if (!_suppressNoCertificateWarning) {
                warning() << "no SSL certificate provided by peer";
            }
            return {boost::none};
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


SSLPeerInfo SSLManager::parseAndValidatePeerCertificateDeprecated(const SSLConnection* conn,
                                                                  const std::string& remoteHost) {
    auto swPeerSubjectName = parseAndValidatePeerCertificate(conn->ssl, remoteHost);
    // We can't use uassertStatusOK here because we need to throw a SocketException.
    if (!swPeerSubjectName.isOK()) {
        throw SocketException(SocketException::CONNECT_ERROR,
                              swPeerSubjectName.getStatus().reason());
    }
    return swPeerSubjectName.getValue().get_value_or(SSLPeerInfo());
}

StatusWith<stdx::unordered_set<RoleName>> SSLManager::_parsePeerRoles(X509* peerCert) const {
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

            /*
             * MongoDBAuthorizationGrant ::= CHOICE {
             *  MongoDBRole,
             *  ...!UTF8String:"Unrecognized entity in MongoDBAuthorizationGrant"
             * }
             * MongoDBAuthorizationGrants ::= SET OF MongoDBAuthorizationGrant
             */
            // Extract the set of roles from our extension, and load them into an OpenSSL stack.
            STACK_OF(ASN1_TYPE)* mongoDBAuthorizationGrants = nullptr;

            // OpenSSL's parsing function will try and manipulate the pointer it's passed. If we
            // passed it 'data->data' directly, it would modify structures owned by peerCert.
            const unsigned char* dataBytes = data->data;
            mongoDBAuthorizationGrants =
                d2i_ASN1_SET_ANY(&mongoDBAuthorizationGrants, &dataBytes, data->length);
            if (!mongoDBAuthorizationGrants) {
                return Status(ErrorCodes::FailedToParse,
                              "Failed to parse x509 authorization grants");
            }
            const auto grantGuard = MakeGuard([&mongoDBAuthorizationGrants]() {
                sk_ASN1_TYPE_pop_free(mongoDBAuthorizationGrants, ASN1_TYPE_free);
            });

            /*
             * MongoDBRole ::= SEQUENCE {
             *  role     UTF8String,
             *  database UTF8String
             * }
             */
            // Loop through every role in the stack.
            ASN1_TYPE* MongoDBRoleWrapped = nullptr;
            while ((MongoDBRoleWrapped = sk_ASN1_TYPE_pop(mongoDBAuthorizationGrants))) {
                const auto roleWrappedGuard =
                    MakeGuard([MongoDBRoleWrapped]() { ASN1_TYPE_free(MongoDBRoleWrapped); });

                if (MongoDBRoleWrapped->type == V_ASN1_SEQUENCE) {
                    // Unwrap the ASN1Type into a STACK_OF(ASN1_TYPE)
                    unsigned char* roleBytes = ASN1_STRING_data(MongoDBRoleWrapped->value.sequence);
                    int roleBytesLength = ASN1_STRING_length(MongoDBRoleWrapped->value.sequence);
                    ASN1_SEQUENCE_ANY* MongoDBRole = nullptr;
                    MongoDBRole = d2i_ASN1_SEQUENCE_ANY(
                        &MongoDBRole, (const unsigned char**)&roleBytes, roleBytesLength);
                    if (!MongoDBRole) {
                        return Status(ErrorCodes::FailedToParse,
                                      "Failed to parse role in x509 authorization grant");
                    }
                    const auto roleGuard = MakeGuard(
                        [&MongoDBRole]() { sk_ASN1_TYPE_pop_free(MongoDBRole, ASN1_TYPE_free); });

                    if (sk_ASN1_TYPE_num(MongoDBRole) != 2) {
                        return Status(ErrorCodes::FailedToParse,
                                      "Role entity in MongoDBAuthorizationGrant must have exactly "
                                      "2 sequence elements");
                    }
                    // Extract the subcomponents of the sequence, which are popped off the stack in
                    // reverse order. Here, parse the role's database.
                    ASN1_TYPE* roleComponent = sk_ASN1_TYPE_pop(MongoDBRole);
                    const auto roleDBGuard =
                        MakeGuard([roleComponent]() { ASN1_TYPE_free(roleComponent); });
                    if (roleComponent->type != V_ASN1_UTF8STRING) {
                        return Status(ErrorCodes::FailedToParse,
                                      "database in MongoDBRole must be a UTF8 string");
                    }
                    std::string roleDB(
                        reinterpret_cast<char*>(ASN1_STRING_data(roleComponent->value.utf8string)));

                    // Parse the role's name.
                    roleComponent = sk_ASN1_TYPE_pop(MongoDBRole);
                    const auto roleNameGuard =
                        MakeGuard([roleComponent]() { ASN1_TYPE_free(roleComponent); });
                    if (roleComponent->type != V_ASN1_UTF8STRING) {
                        return Status(ErrorCodes::FailedToParse,
                                      "role in MongoDBRole must be a UTF8 string");
                    }
                    std::string roleName(
                        reinterpret_cast<char*>(ASN1_STRING_data(roleComponent->value.utf8string)));

                    // Construct a RoleName from the subcomponents
                    roles.emplace(RoleName(roleName, roleDB));

                } else {
                    return Status(ErrorCodes::FailedToParse,
                                  "Unrecognized entity in MongoDBAuthorizationGrant");
                }
            }
            LOG(1) << "MONGODB-X509 authorization parsed the following roles from peer "
                      "certificate: "
                   << [&roles]() {
                          StringBuilder sb;
                          std::for_each(roles.begin(), roles.end(), [&sb](const RoleName& role) {
                              sb << role.toString();
                          });
                          return sb.str();
                      }();
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

void SSLManager::_handleSSLError(SSLConnection* conn, int ret) {
    int code = SSL_get_error(conn, ret);
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
    throw SocketException(SocketException::CONNECT_ERROR, "");
}
}  // namespace mongo

// TODO SERVER-11601 Use NFC Unicode canonicalization
bool mongo::hostNameMatchForX509Certificates(std::string nameToMatch, std::string certHostName) {
    nameToMatch = removeFQDNRoot(std::move(nameToMatch));
    certHostName = removeFQDNRoot(std::move(certHostName));

    if (certHostName.size() < 2) {
        return false;
    }

    // match wildcard DNS names
    if (certHostName[0] == '*' && certHostName[1] == '.') {
        // allow name.example.com if the cert is *.example.com, '*' does not match '.'
        const char* subName = strchr(nameToMatch.c_str(), '.');
        return subName && !strcasecmp(certHostName.c_str() + 1, subName);
    } else {
        return !strcasecmp(nameToMatch.c_str(), certHostName.c_str());
    }
}

boost::optional<std::array<std::uint8_t, 7>> mongo::checkTLSRequest(ConstDataRange dataRange) {
    // This method's caller should have read in at least one MSGHEADER::Value's worth of data.
    // The fragment we are about to examine must be strictly smaller.
    static const size_t sizeOfTLSFragmentToRead = 11;
    invariant(dataRange.length() >= sizeOfTLSFragmentToRead);

    static_assert(sizeOfTLSFragmentToRead < sizeof(MSGHEADER::Value),
                  "checkTLSRequest's caller read a MSGHEADER::Value, which must be larger than "
                  "message containing the TLS version");

    ConstDataRangeCursor cdr(dataRange);

    /**
     * The fragment we are to examine is a record, containing a handshake, containing a
     * ClientHello. We wish to examine the advertised protocol version in the ClientHello.
     * The following roughly describes the contents of these structures. Note that we do not
     * need, or wish to, examine the entire ClientHello, we're looking exclusively for the
     * client_version.
     *
     * Below is a rough description of the payload we will be examining. We shall perform some
     * basic checks to ensure the payload matches these expectations. If it does not, we should
     * bail out, and not emit protocol version alerts.
     *
     * enum {alert(21), handshake(22)} ContentType;
     * TLSPlaintext {
     *   ContentType type = handshake(22),
     *   ProtocolVersion version; // Irrelevant. Clients send the real version in ClientHello.
     *   uint16 length;
     *   fragment, see Handshake stuct for contents
     * ...
     * }
     *
     * enum {client_hello(1)} HandshakeType;
     * Handshake {
     *   HandshakeType msg_type = client_hello(1);
     *   uint24_t length;
     *   ClientHello body;
     * }
     *
     * ClientHello {
     *   ProtocolVersion client_version; // <- This is the value we want to extract.
     * }
     */

    static const std::uint8_t ContentType_handshake = 22;
    static const std::uint8_t HandshakeType_client_hello = 1;

    using ProtocolVersion = std::array<std::uint8_t, 2>;
    static const ProtocolVersion tls10VersionBytes{3, 1};
    static const ProtocolVersion tls11VersionBytes{3, 2};

    // Parse the record header.
    // Extract the ContentType from the header, and ensure it is a handshake.
    StatusWith<std::uint8_t> record_ContentType = cdr.readAndAdvance<std::uint8_t>();
    if (!record_ContentType.isOK() || record_ContentType.getValue() != ContentType_handshake) {
        return boost::none;
    }
    // Skip the record's ProtocolVersion. Clients tend to send TLS 1.0 in
    // the record, but then their real protocol version in the enclosed ClientHello.
    StatusWith<ProtocolVersion> record_protocol_version = cdr.readAndAdvance<ProtocolVersion>();
    if (!record_protocol_version.isOK()) {
        return boost::none;
    }
    // Parse the record length. It should be be larger than the remaining expected payload.
    auto record_length = cdr.readAndAdvance<BigEndian<std::uint16_t>>();
    if (!record_length.isOK() || record_length.getValue() < cdr.length()) {
        return boost::none;
    }

    // Parse the handshake header.
    // Extract the HandshakeType, and ensure it is a ClientHello.
    StatusWith<std::uint8_t> handshake_type = cdr.readAndAdvance<std::uint8_t>();
    if (!handshake_type.isOK() || handshake_type.getValue() != HandshakeType_client_hello) {
        return boost::none;
    }
    // Extract the handshake length, and ensure it is larger than the remaining expected
    // payload. This requires a little work because the packet represents it with a uint24_t.
    StatusWith<std::array<std::uint8_t, 3>> handshake_length_bytes =
        cdr.readAndAdvance<std::array<std::uint8_t, 3>>();
    if (!handshake_length_bytes.isOK()) {
        return boost::none;
    }
    std::uint32_t handshake_length = 0;
    for (std::uint8_t handshake_byte : handshake_length_bytes.getValue()) {
        handshake_length <<= 8;
        handshake_length |= handshake_byte;
    }
    if (handshake_length < cdr.length()) {
        return boost::none;
    }
    StatusWith<ProtocolVersion> client_version = cdr.readAndAdvance<ProtocolVersion>();
    if (!client_version.isOK()) {
        return boost::none;
    }

    // Invariant: We read exactly as much data as expected.
    invariant(cdr.data() - dataRange.data() == sizeOfTLSFragmentToRead);

    auto isProtocolDisabled = [](SSLParams::Protocols protocol) {
        const auto& params = getSSLGlobalParams();
        return std::find(params.sslDisabledProtocols.begin(),
                         params.sslDisabledProtocols.end(),
                         protocol) != params.sslDisabledProtocols.end();
    };

    auto makeTLSProtocolVersionAlert =
        [](const std::array<std::uint8_t, 2>& versionBytes) -> std::array<std::uint8_t, 7> {
        /**
         * The structure for this alert packet is as follows:
         * TLSPlaintext {
         *   ContentType type = alert(21);
         *   ProtocolVersion = versionBytes;
         *   uint16_t length = 2
         *   fragment = AlertDescription {
         *     AlertLevel level = fatal(2);
         *     AlertDescription = protocol_version(70);
         *   }
         *
         */
        return std::array<std::uint8_t, 7>{
            0x15, versionBytes[0], versionBytes[1], 0x00, 0x02, 0x02, 0x46};
    };

    ProtocolVersion version = client_version.getValue();
    if (version == tls10VersionBytes && isProtocolDisabled(SSLParams::Protocols::TLS1_0)) {
        return makeTLSProtocolVersionAlert(version);
    } else if (client_version == tls11VersionBytes &&
               isProtocolDisabled(SSLParams::Protocols::TLS1_1)) {
        return makeTLSProtocolVersionAlert(version);
    }
    // TLS1.2 cannot be distinguished from TLS1.3, just by looking at the ProtocolVersion bytes.
    // TLS 1.3 compatible clients advertise a "supported_versions" extension, which we would
    // have to extract here.
    // Hopefully by the time this matters, OpenSSL will properly emit protocol_version alerts.

    return boost::none;
}

#else

namespace mongo {
namespace {
MONGO_INITIALIZER(SSLManager)(InitializerContext*) {
    // we need a no-op initializer so that we can depend on SSLManager as a prerequisite in
    // non-SSL builds.
    return Status::OK();
}
}  // namespace
}  // namespace mongo

#endif  // #ifdef MONGO_CONFIG_SSL
