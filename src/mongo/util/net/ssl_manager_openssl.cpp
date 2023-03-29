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
#include "mongo/db/connection_health_metrics_parameter_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/transport/session.h"
#include "mongo/transport/ssl_connection_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/cidr.h"
#include "mongo/util/net/dh_openssl.h"
#include "mongo/util/net/ocsp/ocsp_manager.h"
#include "mongo/util/net/private/ssl_expiration.h"
#include "mongo/util/net/socket_exception.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/net/ssl_parameters_gen.h"
#include "mongo/util/net/ssl_peer_info.h"
#include "mongo/util/net/ssl_types.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/read_through_cache.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/strong_weak_finish_line.h"
#include "mongo/util/text.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/asn1.h>
#include <openssl/asn1t.h>
#include <openssl/dh.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/ocsp.h>
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>
#ifdef MONGO_CONFIG_HAVE_SSL_EC_KEY_NEW
#include <openssl/ec.h>
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


#if OPENSSL_VERSION_NUMBER < 0x1010100FL
int SSL_CTX_set_ciphersuites(SSL_CTX*, const char*) {
    uasserted(
        4877400,
        "Setting OpenSSL cipher suites is not allowed for OpenSSL versions older than 1.1.1.");
    return 0;
}
#endif

using namespace fmt::literals;

namespace mongo {

using transport::SSLConnectionContext;

namespace {

// NIDs mapping to OIDs in the MongoDBInc namespace.
// Allocated during MONGO_INITIALIZER(SSLManager), valid for process lifetime.
static int sMongoDbRolesNID;
static int sMongoDbClusterMembershipNID;

MONGO_FAIL_POINT_DEFINE(disableStapling);

using UniqueX509StoreCtx =
    std::unique_ptr<X509_STORE_CTX,
                    OpenSSLDeleter<decltype(X509_STORE_CTX_free), ::X509_STORE_CTX_free>>;

using UniqueX509 = std::unique_ptr<X509, OpenSSLDeleter<decltype(X509_free), ::X509_free>>;

// This deleter should be used when you have a stack of X509 objects that you own and that
// needs to be deleted.
struct X509StackDeleter {
    void operator()(STACK_OF(X509) * chain) {
        if (chain) {
            sk_X509_pop_free(chain, X509_free);
        }
    }
};

// If we have an X509 Stack that is owned by an internal SSL Object, we need to use this
// deleter.
struct X509StackDeleterNoOp {
    void operator()(STACK_OF(X509) * chain) {}
};

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
            LOGV2_WARNING(23230,
                          "Failed to enable ECDHE due to a lack of support from system libraries.");
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

inline void X509_OBJECT_free(X509_OBJECT* a) {
    X509_OBJECT_free_contents(a);
    OPENSSL_free(a);
}

void X509_STORE_CTX_set0_untrusted(X509_STORE_CTX* ctx, STACK_OF(X509) * sk) {
    X509_STORE_CTX_set_chain(ctx, sk);
}

X509_OBJECT* X509_STORE_CTX_get_obj_by_subject(X509_STORE_CTX* vs, int type, X509_NAME* name) {
    X509_OBJECT* ret;
    ret = (X509_OBJECT*)OPENSSL_malloc(sizeof(X509_OBJECT));

    if (ret == NULL) {
        return NULL;
    }
    if (!X509_STORE_get_by_subject(vs, type, name, ret)) {
        X509_OBJECT_free(ret);
        return NULL;
    }
    return ret;
}

X509* X509_OBJECT_get0_X509(const X509_OBJECT* a) {
    if (a == NULL || a->type != X509_LU_X509) {
        return NULL;
    }

    return a->data.x509;
}

using UniqueVerifiedChainPolyfill = std::unique_ptr<STACK_OF(X509), X509StackDeleter>;

STACK_OF(X509) * SSL_get0_verified_chain(SSL* s) {
    auto* store = SSL_CTX_get_cert_store(SSL_get_SSL_CTX(s));
    UniqueX509 peer(SSL_get_peer_certificate(s));
    auto* peerChain = SSL_get_peer_cert_chain(s);

    UniqueX509StoreCtx ctx(X509_STORE_CTX_new());
    if (!X509_STORE_CTX_init(ctx.get(), store, peer.get(), peerChain)) {
        return nullptr;
    }

    if (X509_verify_cert(ctx.get()) <= 0) {
        return nullptr;
    }

    return X509_STORE_CTX_get1_chain(ctx.get());
}

const OCSP_CERTID* OCSP_SINGLERESP_get0_id(const OCSP_SINGLERESP* single) {
    return single->certId;
}

#if OPENSSL_VERSION_NUMBER < 0x10002000L
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

// TLS versions before 1.1.0 did not define the TLS Feature extension
static ASN1OID tlsFeatureOID("1.3.6.1.5.5.7.1.24", "tlsfeature", "TLS Feature");
static int const NID_tlsfeature = OBJ_create(tlsFeatureOID.identifier.c_str(),
                                             tlsFeatureOID.shortDescription.c_str(),
                                             tlsFeatureOID.longDescription.c_str());

#else
using UniqueVerifiedChainPolyfill = std::unique_ptr<STACK_OF(X509), X509StackDeleterNoOp>;

#endif

UniqueVerifiedChainPolyfill SSLgetVerifiedChain(SSL* s) {
    return UniqueVerifiedChainPolyfill(SSL_get0_verified_chain(s));
}

SSLX509Name convertX509ToSSLX509Name(X509_NAME* x509Name) {
    std::vector<std::vector<SSLX509Name::Entry>> entries;

    int count = X509_NAME_entry_count(x509Name);
    int prevSet = -1;
    std::vector<SSLX509Name::Entry> rdn;
    for (int i = count - 1; i >= 0; --i) {
        auto* entry = X509_NAME_get_entry(x509Name, i);

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
                "Unable to parse certificate subject name",
                OBJ_obj2txt(buffer, sizeof(buffer), X509_NAME_ENTRY_get_object(entry), 1) > 0);

        const auto* str = X509_NAME_ENTRY_get_data(entry);
        rdn.emplace_back(
            buffer, str->type, std::string(reinterpret_cast<const char*>(str->data), str->length));
    }
    if (!rdn.empty()) {
        entries.push_back(std::move(rdn));
    }

    SSLX509Name sslX509Name = SSLX509Name(std::move(entries));
    uassertStatusOK(sslX509Name.normalizeStrings());
    return sslX509Name;
}

/*
 * Converts time from OpenSSL return value to Date_t representing the time on
 * the ASN1_TIME object.
 */
Date_t convertASN1ToMillis(const ASN1_TIME* asn1time) {
    static const int DATE_LEN = 128;

    BIO* outBIO = BIO_new(BIO_s_mem());
    int timeError = ASN1_TIME_print(outBIO, asn1time);
    ON_BLOCK_EXIT([&] { BIO_free(outBIO); });

    if (timeError <= 0) {
        LOGV2_ERROR(23241, "ASN1_TIME_print failed or wrote no data.");
        return Date_t();
    }

    char dateChar[DATE_LEN];
    timeError = BIO_gets(outBIO, dateChar, DATE_LEN);
    if (timeError <= 0) {
        LOGV2_ERROR(23242, "BIO_gets call failed to transfer contents to buf");
        return Date_t();
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

    return Date_t::fromMillisSinceEpoch(
        (posixTime - boost::posix_time::ptime(epoch)).total_milliseconds());
}

class SSLConnectionOpenSSL : public SSLConnectionInterface {
public:
    SSL* ssl;
    BIO* networkBIO;
    BIO* internalBIO;
    Socket* socket;

    SSLConnectionOpenSSL(SSL_CTX* ctx, Socket* sock, const char* initialBytes, int len);

    ~SSLConnectionOpenSSL();
};

////////////////////////////////////////////////////////////////

using UniqueSSLContext =
    std::unique_ptr<SSL_CTX, OpenSSLDeleter<decltype(::SSL_CTX_free), ::SSL_CTX_free>>;
using UniqueSSL = std::unique_ptr<SSL, OpenSSLDeleter<decltype(::SSL_free), ::SSL_free>>;
static const int BUFFER_SIZE = 8 * 1024;

using UniqueOpenSSLStringStack =
    std::unique_ptr<STACK_OF(OPENSSL_STRING),
                    OpenSSLDeleter<decltype(X509_email_free), ::X509_email_free>>;

using UniqueOCSPResponse =
    std::unique_ptr<OCSP_RESPONSE,
                    OpenSSLDeleter<decltype(OCSP_RESPONSE_free), ::OCSP_RESPONSE_free>>;

using UniqueCertId =
    std::unique_ptr<OCSP_CERTID, OpenSSLDeleter<decltype(OCSP_CERTID_free), ::OCSP_CERTID_free>>;

Status getSSLFailure(ErrorCodes::Error code, StringData errorMsg) {
    return Status(code,
                  str::stream() << "SSL peer certificate revocation status checking failed: "
                                << errorMsg << " "
                                << SSLManagerInterface::getSSLErrorMessage(ERR_get_error()));
}

Status getSSLFailure(StringData errorMsg) {
    return getSSLFailure(ErrorCodes::SSLHandshakeFailed, errorMsg);
}

// X509_OBJECT_free is not exposed in the same way as the rest of the functions.
struct X509_OBJECTFree {
    void operator()(X509_OBJECT* obj) noexcept {
        if (obj) {
            X509_OBJECT_free(obj);
        }
    }
};

using UniqueX509Object = std::unique_ptr<X509_OBJECT, X509_OBJECTFree>;

StatusWith<UniqueX509> getIssuerCertForCert(SSL_CTX* context,
                                            X509* cert,
                                            STACK_OF(X509) * intermediateCerts) {
    // First search the intermediate certificates for the issuer.
    for (int i = 0; i < sk_X509_num(intermediateCerts); i++) {
        if (X509_NAME_cmp(X509_get_issuer_name(cert),
                          X509_get_subject_name(sk_X509_value(intermediateCerts, i))) == 0) {
            return UniqueX509(X509_dup(sk_X509_value(intermediateCerts, i)));
        }
    }

    UniqueX509StoreCtx storeCtx(X509_STORE_CTX_new());

    if (!storeCtx) {
        return getSSLFailure("Could not create X509 store.");
    }

    // Look in the certificate store for the certificate that issued cert
    if (X509_STORE_CTX_init(storeCtx.get(), SSL_CTX_get_cert_store(context), NULL, NULL) == 0) {
        return getSSLFailure("Could not initialize the X509 Store Context for the SSL Context.");
    }

    UniqueX509Object obj(X509_STORE_CTX_get_obj_by_subject(
        storeCtx.get(), X509_LU_X509, X509_get_issuer_name(cert)));

    if (obj == nullptr) {
        return getSSLFailure("Could not get X509 Object from store.");
    }
    return UniqueX509(X509_dup(X509_OBJECT_get0_X509(obj.get())));
}

UniqueCertId getCertIdForCert(X509* cert, X509* issuerCert) {
    return UniqueCertId(OCSP_cert_to_id(nullptr, cert, issuerCert));
}

struct OCSPCertIDCompareLess {
    bool operator()(const UniqueCertId& id1, const UniqueCertId& id2) const {
        return OCSP_id_cmp(id1.get(), id2.get()) > 0;
    }
};

using OCSPCertIDSet = std::set<UniqueCertId, OCSPCertIDCompareLess>;

using UniqueOCSPRequest =
    std::unique_ptr<OCSP_REQUEST, OpenSSLDeleter<decltype(OCSP_REQUEST_free), ::OCSP_REQUEST_free>>;

using UniqueOcspBasicResp =
    std::unique_ptr<OCSP_BASICRESP,
                    OpenSSLDeleter<decltype(OCSP_BASICRESP_free), ::OCSP_BASICRESP_free>>;

struct OCSPRequestAndIDs {
    UniqueOCSPRequest request;
    OCSPCertIDSet certIDs;
};

struct OCSPValidationContext {
    std::map<std::string, OCSPRequestAndIDs> ocspRequestMap;
    OCSPCertIDSet uniqueCertIds;
    std::vector<std::string> leafResponders;
    UniqueX509 issuerCert;
};

constexpr Milliseconds kOCSPUnknownStatusRefreshRate = Minutes(5);

struct OCSPFetchResponse {
    OCSPFetchResponse(Status statusOfResponse,
                      UniqueOCSPResponse response,
                      boost::optional<Date_t> refreshTime)
        : statusOfResponse(statusOfResponse),
          response(std::move(response)),
          refreshTime(refreshTime.value_or(Date_t::now() + 2 * kOCSPUnknownStatusRefreshRate)),
          hasNextUpdate(refreshTime) {}

    Status statusOfResponse;
    UniqueOCSPResponse response;
    Date_t refreshTime;
    bool hasNextUpdate;

    bool cacheable() {
        return (statusOfResponse == ErrorCodes::OCSPCertificateStatusRevoked) || hasNextUpdate;
    }

    Milliseconds fetchNewResponseDuration() {
        Milliseconds timeBeforeNextUpdate = refreshTime - Date_t::now();
        if (timeBeforeNextUpdate < Milliseconds(0)) {
            return Milliseconds(0);
        }

        // If the setParameter for OCSPStaplingRefreshPeriodSecs is too high, we want to
        // make sure that we are never left without a stapled OCSP response because we
        // were not refreshing fast enough.
        if (kOCSPStaplingRefreshPeriodSecs != -1 &&
            kOCSPStaplingRefreshPeriodSecs <
                durationCount<Seconds>(timeBeforeNextUpdate -
                                       Seconds(60 + gTLSOCSPStaplingTimeoutSecs))) {

            return Milliseconds(Seconds(kOCSPStaplingRefreshPeriodSecs));
        }
        return timeBeforeNextUpdate / 2;
    }

    Date_t nextStapleRefresh() {
        return refreshTime;
    }
};

std::vector<unsigned char> convertX509ToDERVec(X509* cert) {
    auto len = i2d_X509(cert, nullptr);

    uassert(ErrorCodes::SSLHandshakeFailed,
            "Could not convert certificate to DER representation",
            len > 0);

    std::vector<unsigned char> x509Vec(len);
    auto ptr = x509Vec.data();

    len = i2d_X509(cert, &ptr);
    return x509Vec;
}

std::vector<std::vector<unsigned char>> convertStackOfX509ToDERVec(STACK_OF(X509) * chain) {
    int numCerts = sk_X509_num(chain);

    if (numCerts < 0) {
        return {};
    }

    std::vector<std::vector<unsigned char>> retVector(numCerts);

    for (int i = 0; i < numCerts; i++) {
        retVector.emplace_back(convertX509ToDERVec(sk_X509_value(chain, i)));
    }

    return retVector;
}

struct OCSPCacheKey {
    OCSPCacheKey(UniqueX509 cert, SSL_CTX* context, UniqueVerifiedChainPolyfill intermediateCerts)
        : peerCert(std::move(cert)),
          context(context),
          intermediateCerts(std::move(intermediateCerts)),
          peerCertBuffer(convertX509ToDERVec(peerCert.get())),
          chainBuffer(convertStackOfX509ToDERVec(intermediateCerts.get())) {}

    template <typename H>
    friend H AbslHashValue(H h, const OCSPCacheKey& key) {
        return H::combine(std::move(h), key.peerCertBuffer, key.chainBuffer, key.context);
    }

    bool operator==(const OCSPCacheKey key) const {
        return peerCertBuffer == key.peerCertBuffer && context == key.context &&
            chainBuffer == key.chainBuffer;
    }

    std::shared_ptr<X509> peerCert;
    SSL_CTX* context;
    std::shared_ptr<STACK_OF(X509)> intermediateCerts;
    std::vector<unsigned char> peerCertBuffer;
    std::vector<std::vector<unsigned char>> chainBuffer;
};

/**
 * This function takes an individual certificate and adds its OCSP certificate ID
 * to the ocspRequestMap. See comment in extractOcspUris for details.
 */
StatusWith<std::vector<std::string>> addOCSPUrlToMap(
    X509* cert,
    X509* issuerCert,
    std::map<std::string, OCSPRequestAndIDs>& ocspRequestMap,
    OCSPCertIDSet& uniqueCertIds) {

    UniqueOpenSSLStringStack aiaOCSP(X509_get1_ocsp(cert));
    std::vector<std::string> responders;

    if (!aiaOCSP) {
        return responders;
    }

    // Iterate through all the values in the Authority Information Access extension in
    // the certificate to get the location of the OCSP responder.
    for (int i = 0; i < sk_OPENSSL_STRING_num(aiaOCSP.get()); i++) {
        int useSSL = 0;
        char *host, *port, *path;
        ScopeGuard OCSPStrGuard([&] {
            if (host) {
                OPENSSL_free(host);
            }
            if (port) {
                OPENSSL_free(port);
            }
            if (path) {
                OPENSSL_free(path);
            }
        });
        if (!OCSP_parse_url(
                sk_OPENSSL_STRING_value(aiaOCSP.get(), i), &host, &port, &path, &useSSL)) {
            return getSSLFailure("Could not parse AIA url.");
        }

        HostAndPort hostAndPort(str::stream() << host << ":" << port);

        UniqueCertId certID = getCertIdForCert(cert, issuerCert);
        if (!certID) {
            return getSSLFailure("Could not get certificate ID for Map.");
        }

        UniqueCertId certIDForArray = getCertIdForCert(cert, issuerCert);
        if (certIDForArray == nullptr) {
            return getSSLFailure("Could not get certificate ID for Array.");
        }

        OCSPRequestAndIDs reqAndIDs{UniqueOCSPRequest(OCSP_REQUEST_new()), OCSPCertIDSet()};

        auto [mapIter, _] = ocspRequestMap.try_emplace(str::stream() << host << ":" << port << path,
                                                       std::move(reqAndIDs));

        responders.emplace_back(str::stream() << host << ":" << port << path);

        OCSP_request_add0_id(mapIter->second.request.get(), certID.release());
        mapIter->second.certIDs.insert(std::move(certIDForArray));
    }

    UniqueCertId certIDForSet = getCertIdForCert(cert, issuerCert);
    if (!certIDForSet) {
        return getSSLFailure("Could not get certificate ID for Set.");
    }

    uniqueCertIds.insert(std::move(certIDForSet));

    return responders;
}

Future<UniqueOCSPResponse> retrieveOCSPResponse(const std::string& host,
                                                OCSPRequestAndIDs& ocspRequestAndIDs,
                                                OCSPPurpose purpose) {
    auto& [ocspReq, certIDs] = ocspRequestAndIDs;

    // Decompose the OCSP request into a DER encoded OCSP request
    auto len = i2d_OCSP_REQUEST(ocspReq.get(), nullptr);
    std::vector<uint8_t> buffer;
    if (len <= 0) {
        return getSSLFailure("Could not decode response from responder.");
    }

    buffer.resize(len);
    auto bufferData = buffer.data();
    if (i2d_OCSP_REQUEST(ocspReq.get(), &bufferData) < 0) {
        return getSSLFailure("Could not convert type OCSP Response to DER encoded object.");
    }

    if (!OCSPManager::get(getGlobalServiceContext())) {
        return getSSLFailure("OCSP fetch could not complete, server is in shutdown mode.");
    }

    // Query the OCSP responder
    return OCSPManager::get(getGlobalServiceContext())
        ->requestStatus(buffer, host, purpose)
        .then([](std::vector<uint8_t> responseData) mutable -> StatusWith<UniqueOCSPResponse> {
            const uint8_t* respDataPtr = responseData.data();

            // Convert the Response back to a OpenSSL known format
            UniqueOCSPResponse response(
                d2i_OCSP_RESPONSE(nullptr, &respDataPtr, responseData.size()));

            if (response == nullptr) {
                return getSSLFailure("Could not retrieve OCSP Response.");
            }

            return std::move(response);
        });
}

bool certIdsMatch(const UniqueCertId& reqCertId, const UniqueCertId& rspCertId, X509* issuerCert) {

    if (!(OCSP_id_cmp(rspCertId.get(), reqCertId.get()))) {
        // all CertID fields match
        return true;
    }

    ASN1_OBJECT* rspHashAlgo = nullptr;
    ASN1_INTEGER* rspSerial = nullptr;
    ASN1_INTEGER* reqSerial = nullptr;

    if (!(OCSP_id_get0_info(nullptr, nullptr, nullptr, &reqSerial, reqCertId.get()))) {
        LOGV2_DEBUG(6031001, 3, "Failed to obtain field values from OCSP request CertID");
        return false;
    }

    if (!(OCSP_id_get0_info(nullptr, &rspHashAlgo, nullptr, &rspSerial, rspCertId.get()))) {
        LOGV2_DEBUG(6031002, 3, "Failed to obtain field values from OCSP response CertID");
        return false;
    }

    if (ASN1_INTEGER_cmp(rspSerial, reqSerial)) {
        // serial numbers don't match
        return false;
    }

    // It is possible that the issuer hashes did not match because
    // the hash algorithm used is different, so we have to re-hash the
    // requested issuer name & key using the algorithm in the response, and
    // do another comparison.
    auto md = EVP_get_digestbyobj(rspHashAlgo);
    if (!md) {
        // unrecognized digest algorithm, skip
        LOGV2_DEBUG(6031000, 3, "Received an OCSP CertID with an unknown hash algorithm");
        return false;
    }

    auto newReqCertId = UniqueCertId(OCSP_cert_id_new(
        md, X509_get_subject_name(issuerCert), X509_get0_pubkey_bitstr(issuerCert), reqSerial));

    // Compare just the issuer hashes here since the serial numbers are known to be the same.
    if (!(OCSP_id_issuer_cmp(newReqCertId.get(), rspCertId.get()))) {
        // issuers matched
        return true;
    }
    return false;
}

/**
 * This function iterates over the basic response object from the OCSP response object
 * and returns a set of Certificate IDs that are there in the response and a date object
 * which represents the time when the Response needs to be refreshed.
 */
StatusWith<std::pair<OCSPCertIDSet, boost::optional<Date_t>>> iterateResponse(
    OCSP_BASICRESP* basicResp, X509* issuerCert, const OCSPCertIDSet& certIdsInRequest) {
    boost::optional<Date_t> earliestNextUpdate = boost::none;

    OCSPCertIDSet certIdsInResponse;

    // Iterate over all the certificates in the Response, mainly to see if any
    // of them have been revoked.
    int count = OCSP_resp_count(basicResp);
    for (int i = 0; i < count; i++) {
        OCSP_SINGLERESP* singleResp = OCSP_resp_get0(basicResp, i);
        if (!singleResp) {
            return getSSLFailure("OCSP Basic Response invalid: Missing response.");
        }

        auto certId = UniqueCertId(
            OCSP_CERTID_dup(const_cast<OCSP_CERTID*>(OCSP_SINGLERESP_get0_id(singleResp))));

        // This is O(n), but certIdsInRequest is expected to only contain just
        // one CertID in it anyways, so this should be no big deal
        if (std::none_of(certIdsInRequest.begin(),
                         certIdsInRequest.end(),
                         [&certId, &issuerCert](const UniqueCertId& reqCertId) {
                             return certIdsMatch(reqCertId, certId, issuerCert);
                         })) {
            // skip irrelevant certificates
            continue;
        }

        certIdsInResponse.insert(std::move(certId));

        int reason;
        ASN1_GENERALIZEDTIME *revtime, *thisupd, *nextupd;

        auto status = OCSP_single_get0_status(singleResp, &reason, &revtime, &thisupd, &nextupd);

        if (status == V_OCSP_CERTSTATUS_REVOKED) {
            return getSSLFailure(ErrorCodes::OCSPCertificateStatusRevoked,
                                 str::stream() << "OCSP Certificate Status: Revoked. Reason: "
                                               << OCSP_crl_reason_str(reason));
        } else if (status != V_OCSP_CERTSTATUS_GOOD) {
            return getSSLFailure(str::stream()
                                 << "Unexpected OCSP Certificate Status. Reason: " << status);
        }

        if (nextupd) {
            Date_t nextUpdateDate(convertASN1ToMillis(static_cast<ASN1_TIME*>(nextupd)));
            earliestNextUpdate = earliestNextUpdate
                ? boost::optional<Date_t>(std::min(earliestNextUpdate.get(), nextUpdateDate))
                : boost::optional<Date_t>(nextUpdateDate);
        }
    }

    if (certIdsInResponse.empty()) {
        return getSSLFailure(
            "OCSP Basic Response is insufficient: No single responses matched any of the expected "
            "cert IDs");
    }

    if (earliestNextUpdate && earliestNextUpdate < Date_t::now()) {
        return getSSLFailure("OCSP Basic Response is invalid: Response is expired.");
    }

    return std::make_pair(std::move(certIdsInResponse), earliestNextUpdate);
}

/**
 * This function returns a pair of a CertID set and a Date_t object. The CertID set contains
 * the IDs of the certificates that the OCSP Response contains. The Date_t object is the
 * earliest expiration date on the OCSPResponse.
 */
StatusWith<std::pair<OCSPCertIDSet, boost::optional<Date_t>>> parseAndValidateOCSPResponse(
    SSL_CTX* context,
    X509_STORE* ca,
    OCSP_RESPONSE* response,
    STACK_OF(X509) * intermediateCerts,
    X509* issuerCert,
    const OCSPCertIDSet& certIdsInRequest) {
    // Read the overall status of the OCSP response
    int responseStatus = OCSP_response_status(response);
    switch (responseStatus) {
        case OCSP_RESPONSE_STATUS_SUCCESSFUL:
            break;
        case OCSP_RESPONSE_STATUS_MALFORMEDREQUEST:
        case OCSP_RESPONSE_STATUS_UNAUTHORIZED:
        case OCSP_RESPONSE_STATUS_SIGREQUIRED:
            return getSSLFailure(str::stream()
                                 << "Error querying the OCSP responder, issue with OCSP request. "
                                 << "Response Status: " << responseStatus);
        case OCSP_RESPONSE_STATUS_TRYLATER:
        case OCSP_RESPONSE_STATUS_INTERNALERROR:
            return getSSLFailure(str::stream()
                                 << "Error querying the OCSP responder, an error occured in the "
                                 << "responder itself. Response Status: " << responseStatus);
        default:
            return getSSLFailure(str::stream() << "Error querying the OCSP responder. "
                                               << "Response Status: " << responseStatus);
    }

    UniqueOcspBasicResp basicResponse(OCSP_response_get1_basic(response));
    if (!basicResponse) {
        return getSSLFailure("incomplete OCSP response.");
    }

    X509_STORE* store = ca == nullptr ? SSL_CTX_get_cert_store(context) : ca;

    // OCSP_basic_verify takes in the Response from the responder and verifies
    // that the signer of the OCSP response is in intermediateCerts. Then it tries
    // to form a chain from the signer certificate to the trusted CA in the store.
    if (OCSP_basic_verify(basicResponse.get(), intermediateCerts, store, 0) != 1) {
        return getSSLFailure("Failed to verify signature from OCSP response.");
    }

    return iterateResponse(basicResponse.get(), issuerCert, certIdsInRequest);
}

Future<OCSPFetchResponse> dispatchOCSPRequests(SSL_CTX* context,
                                               std::shared_ptr<X509_STORE> ca,
                                               std::shared_ptr<STACK_OF(X509)> intermediateCerts,
                                               OCSPValidationContext ocspContext,
                                               OCSPPurpose purpose) {
    auto& [ocspRequestMap, _, leafResponders, issuer] = ocspContext;

    struct OCSPCompletionState {
        OCSPCompletionState(int numRequests_,
                            Promise<OCSPFetchResponse> promise_,
                            std::shared_ptr<STACK_OF(X509)> intermediateCerts_,
                            OCSPValidationContext ocspContext_)
            : finishLine(numRequests_),
              promise(std::move(promise_)),
              intermediateCerts(std::move(intermediateCerts_)),
              ocspContext(std::move(ocspContext_)) {}

        StrongWeakFinishLine finishLine;
        Promise<OCSPFetchResponse> promise;
        std::shared_ptr<STACK_OF(X509)> intermediateCerts;
        OCSPValidationContext ocspContext;
    };

    if (purpose == OCSPPurpose::kClientVerify) {
        LOGV2_INFO(6144501, "Dispatching OCSP requests for client-side verification");
    } else {
        LOGV2_INFO(6144502, "Dispatching OCSP requests for server stapling");
    }

    std::vector<Future<UniqueOCSPResponse>> futureResponses{};
    std::vector<OCSPCertIDSet*> requestedCertIDSets{};

    for (auto& host : leafResponders) {
        auto& ocspRequestAndIDs = ocspRequestMap[host];
        Future<UniqueOCSPResponse> futureResponse =
            retrieveOCSPResponse(host, ocspRequestAndIDs, purpose);
        futureResponses.push_back(std::move(futureResponse));
        requestedCertIDSets.push_back(&ocspRequestAndIDs.certIDs);
    };

    auto pf = makePromiseFuture<OCSPFetchResponse>();
    auto state = std::make_shared<OCSPCompletionState>(futureResponses.size(),
                                                       std::move(pf.promise),
                                                       std::move(intermediateCerts),
                                                       std::move(ocspContext));
    auto startTimer = std::make_shared<Timer>();
    for (size_t i = 0; i < futureResponses.size(); i++) {
        auto futureResponse = std::move(futureResponses[i]);
        auto requestedCertIDs = requestedCertIDSets[i];

        std::move(futureResponse)
            .getAsync([context, ca, state, requestedCertIDs, startTimer, purpose](
                          StatusWith<UniqueOCSPResponse> swResponse) mutable {
                auto requestLatency = startTimer->millis();
                // We use a scope guard because we only want to log the metrics once we have come to
                // a resolution on the status of the connection. This happens on the event of:
                // 1. The first OCSP response that we get that indicates the certificate is valid or
                //    has been revoked.
                // 2. The last OCSP response returns and the status of the certificate is still
                //    unknown.
                ScopeGuard logLatencyGuard([requestLatency, purpose]() {
                    if (purpose != OCSPPurpose::kClientVerify ||
                        !gEnableDetailedConnectionHealthMetricLogLines) {
                        return;
                    }
                    LOGV2_INFO(6840101,
                               "Completed client-side verification of OCSP request",
                               "verificationTimeMillis"_attr = requestLatency);
                });
                if (!swResponse.isOK()) {
                    if (state->finishLine.arriveWeakly()) {
                        state->promise.setError(
                            Status(ErrorCodes::OCSPCertificateStatusUnknown,
                                   "Could not obtain status information of certificates."));
                    }
                    return;
                }

                auto swCertIDSetAndDuration =
                    parseAndValidateOCSPResponse(context,
                                                 ca.get(),
                                                 swResponse.getValue().get(),
                                                 state->intermediateCerts.get(),
                                                 state->ocspContext.issuerCert.get(),
                                                 *requestedCertIDs);

                if (swCertIDSetAndDuration.isOK() ||
                    swCertIDSetAndDuration.getStatus() ==
                        ErrorCodes::OCSPCertificateStatusRevoked) {

                    // We want to send the nextUpdate time down for the cache, so if there is a
                    // duration value passed from parseAndValidateOCSPResponse, we send that down.
                    // If not, we pass down a bogus response, and let the caller deal with it down
                    // there.
                    boost::optional<Date_t> nextUpdate = swCertIDSetAndDuration.isOK()
                        ? swCertIDSetAndDuration.getValue().second
                        : boost::none;

                    if (state->finishLine.arriveStrongly()) {
                        state->promise.emplaceValue(swCertIDSetAndDuration.getStatus(),
                                                    std::move(swResponse.getValue()),
                                                    nextUpdate);
                        return;
                    }
                } else {
                    if (state->finishLine.arriveWeakly()) {
                        state->promise.setError(
                            Status(ErrorCodes::OCSPCertificateStatusUnknown,
                                   swCertIDSetAndDuration.getStatus().reason())
                                .withContext(
                                    "Could not obtain status information of certificates"));
                        return;
                    }
                }
                // Don't log any metrics if we haven't come to a decision on the validity of the
                // certificate yet.
                logLatencyGuard.dismiss();
            });
    }

    return std::move(pf.future);
}

/**
 * Iterates over a list of intermediate certificates and the peer certificate
 * in a chain of X509 certificates
 * and adds the OCSP certificate ID to the correct OCSP Request object.
 * OCSP Request objects need to be separated by the specific OCSP responder URI.
 */
StatusWith<OCSPValidationContext> extractOcspUris(SSL_CTX* context,
                                                  X509* peerCert,
                                                  STACK_OF(X509) * intermediateCerts) {

    std::map<std::string, OCSPRequestAndIDs> ocspRequestMap;
    OCSPCertIDSet uniqueCertIds;

    auto swIssuerCert = getIssuerCertForCert(context, peerCert, intermediateCerts);
    if (!swIssuerCert.isOK()) {
        return swIssuerCert.getStatus();
    }

    auto swLeafResponders =
        addOCSPUrlToMap(peerCert, swIssuerCert.getValue().get(), ocspRequestMap, uniqueCertIds);
    if (!swLeafResponders.isOK()) {
        return swLeafResponders.getStatus();
    }

    auto leafResponders = std::move(swLeafResponders.getValue());
    if (leafResponders.size() == 0) {
        return getSSLFailure("Certificate has no OCSP Responders");
    }

    return OCSPValidationContext{std::move(ocspRequestMap),
                                 std::move(uniqueCertIds),
                                 std::move(leafResponders),
                                 std::move(swIssuerCert.getValue())};
}

class OCSPCache : public ReadThroughCache<OCSPCacheKey, OCSPFetchResponse> {
public:
    OCSPCache(ServiceContext* service)
        : ReadThroughCache(_mutex, service, _threadPool, _lookup, tlsOCSPCacheSize) {
        _threadPool.startup();
    }

    static void create(ServiceContext* service) {
        getOCSPCache(service).emplace(service);
    }

    static void destroy(ServiceContext* service) {
        getOCSPCache(service).reset();
    }

    static OCSPCache& get(ServiceContext* service) {
        return *getOCSPCache(service);
    }

private:
    static LookupResult _lookup(OperationContext* opCtx,
                                const OCSPCacheKey& key,
                                const ValueHandle& unusedCachedValue) {
        // If there is a CRL file, we expect the CRL file to cover the certificate status
        // information, and therefore we don't need to make a roundtrip.
        if (!getSSLGlobalParams().sslCRLFile.empty()) {
            return LookupResult(boost::none);
        }

        auto swOCSPContext =
            extractOcspUris(key.context, key.peerCert.get(), key.intermediateCerts.get());
        if (!swOCSPContext.isOK()) {
            return LookupResult(boost::none);
        }

        auto swResponse = dispatchOCSPRequests(key.context,
                                               nullptr,
                                               key.intermediateCerts,
                                               std::move(swOCSPContext.getValue()),
                                               OCSPPurpose::kClientVerify)
                              .getNoThrow();
        if (!swResponse.isOK() || !swResponse.getValue().cacheable()) {
            // if the response is unknown or not cacheable, (ie. because of
            // a missing nextUpdate field), then return none so that the
            // response is not cached
            return LookupResult(boost::none);
        }

        return LookupResult(std::move(swResponse.getValue()));
    }

    static const ServiceContext::Decoration<boost::optional<OCSPCache>> getOCSPCache;

    Mutex _mutex = MONGO_MAKE_LATCH("OCSPCache::_mutex");

    ThreadPool _threadPool{[] {
        ThreadPool::Options options;
        options.poolName = "OCSPCache";
        options.minThreads = 0;
        return options;
    }()};
};

const ServiceContext::Decoration<boost::optional<OCSPCache>> OCSPCache::getOCSPCache =
    ServiceContext::declareDecoration<boost::optional<OCSPCache>>();

ServiceContext::ConstructorActionRegisterer OCSPCacheRegisterer(
    "CreateOCSPCache",
    [](ServiceContext* context) { OCSPCache::create(context); },
    [](ServiceContext* context) { OCSPCache::destroy(context); });

using OCSPCacheVal = OCSPCache::ValueHandle;

struct OCSPStaplingContext {
    OCSPStaplingContext(UniqueOCSPResponse response, Date_t nextUpdate)
        : sharedResponseForServer(std::move(response)), sharedResponseNextUpdate(nextUpdate) {}

    OCSPStaplingContext() = default;

    std::shared_ptr<OCSP_RESPONSE> sharedResponseForServer;
    Date_t sharedResponseNextUpdate;
};

class SSLManagerOpenSSL;

/**
 * OCSPFetcher is responsible for periodically fetching OCSP responses.
 *
 * The first response can be optionally waited for.
 */
class OCSPFetcher {
public:
    OCSPFetcher(SSLManagerOpenSSL* manager) : _manager(manager) {}

    /**
     * Start fetching OCSP responses. The first one can be optionally waited for.
     */
    Status start(SSL_CTX* context, bool asyncOCSPStaple);

    /**
     * Stop fetching.
     */
    void shutdown();

private:
    Future<Milliseconds> fetchAndStaple(Promise<void>* promise);

    void startPeriodicJob(StatusWith<Milliseconds> swDurationInitial);

    void doPeriodicJob();

    bool _isShutdownConditionLocked(WithLock);

    void _shutdownLocked(WithLock);

private:
    // Hack
    // OpenSSL 1.1.0 = Since OpenSSL is ref counted underneath, we should use SSL_CTX_up_ref.
    // But we support older versions, we workaround the lack of SSL_CTX_up_ref so instead we create
    // a SSL* off the SSL_CTX to bump the ref count in SSL_CTX
    UniqueSSL _ssl{nullptr};
    SSL_CTX* _context{nullptr};
    X509* _cert{nullptr};
    std::shared_ptr<X509_STORE> _ca;

    // Note: A ref is kept by all futures and the periodic job on the SSLManagerOpenSSL ensure this
    // object is alive.
    SSLManagerOpenSSL* _manager;

    // Weak pointer to verify that the context owning the manager above is still valid
    // and the manager it owns still matches the manager.
    std::weak_ptr<const SSLConnectionContext> _ownedByContext;

    Mutex _staplingMutex = MONGO_MAKE_LATCH("OCSPStaplingJobRunner::_mutex");
    PeriodicRunner::JobAnchor _ocspStaplingAnchor;
    bool _shutdown{false};
};

/**
 * OCSPRefreshBackoff implements a backoff counter that is used by the OCSPFetcher in determining
 * how long to wait until the next fetch retry if it encounters transient errors while communicating
 * with an OCSP responder.
 */
struct OCSPRefreshBackoff {
    static constexpr auto kOCSPRefreshBackoffMinimum = Milliseconds(500);
    OCSPRefreshBackoff(Milliseconds limit = kOCSPUnknownStatusRefreshRate)
        : _backoff(kOCSPRefreshBackoffMinimum), _limit(limit) {}

    Milliseconds getNextRefreshDuration() {
        auto prev = _backoff;
        _backoff = std::min(_limit, _backoff * 2);
        return prev;
    }
    Milliseconds _backoff;
    Milliseconds _limit;
};

class SSLManagerOpenSSL : public SSLManagerInterface,
                          public std::enable_shared_from_this<SSLManagerOpenSSL> {
public:
    explicit SSLManagerOpenSSL(const SSLParams& params,
                               const boost::optional<TransientSSLParams>& transientSSLParams,
                               bool isServer);
    ~SSLManagerOpenSSL() {
        stopJobs();
    }

    /**
     * Initializes an OpenSSL context according to the provided settings. Only settings which are
     * acceptable on non-blocking connections are set.
     */
    Status initSSLContext(SSL_CTX* context,
                          const SSLParams& params,
                          ConnectionDirection direction) final;

    void registerOwnedBySSLContext(std::weak_ptr<const SSLConnectionContext> ownedByContext) final;

    std::weak_ptr<const SSLConnectionContext> getSSLContextOwner() {
        return *_ownedByContext;
    }

    std::shared_ptr<X509_STORE> getOCSPCertStore() {
        return _ocspCertStore;
    }

    SSLConnectionInterface* connect(Socket* socket) final;

    SSLConnectionInterface* accept(Socket* socket, const char* initialBytes, int len) final;

    SSLPeerInfo parseAndValidatePeerCertificateDeprecated(const SSLConnectionInterface* conn,
                                                          const std::string& remoteHost,
                                                          const HostAndPort& hostForLogging) final;

    Future<SSLPeerInfo> parseAndValidatePeerCertificate(SSL* conn,
                                                        boost::optional<std::string> sni,
                                                        const std::string& remoteHost,
                                                        const HostAndPort& hostForLogging,
                                                        const ExecutorPtr& reactor) final;

    /**
     * Sets the OCSP Response to be stapled to the TLS Connection. Sets the _ocspStaplingAnchor
     * object in the class.
     */
    Status stapleOCSPResponse(SSL_CTX* context, bool asyncOCSPStaple) final;

    void stopJobs() final;

    const SSLConfiguration& getSSLConfiguration() const final {
        return _sslConfiguration;
    }

    bool isTransient() const final;

    std::string getTargetedClusterConnectionString() const final;

    int SSL_read(SSLConnectionInterface* conn, void* buf, int num) final;

    int SSL_write(SSLConnectionInterface* conn, const void* buf, int num) final;

    int SSL_shutdown(SSLConnectionInterface* conn) final;

    Future<void> ocspClientVerification(SSL* ssl, const ExecutorPtr& reactor);

    SSLInformationToLog getSSLInformationToLog() const final;

    std::shared_ptr<OCSPStaplingContext> getOcspStaplingContext() {
        stdx::lock_guard<mongo::Mutex> guard(_sharedResponseMutex);
        return _ocspStaplingContext;
    }

    Milliseconds updateOcspStaplingContextWithResponse(StatusWith<OCSPFetchResponse> swResponse);

private:
    UniqueSSLContext _serverContext;             // SSL context for incoming connections
    UniqueSSLContext _clientContext;             // SSL context for outgoing connections
    std::shared_ptr<X509_STORE> _ocspCertStore;  // X509 Store specifically for OCSP stapling

    bool _weakValidation;
    bool _allowInvalidCertificates;
    bool _allowInvalidHostnames;
    bool _suppressNoCertificateWarning;
    SSLConfiguration _sslConfiguration;
    // If set, this manager is an instance providing authentication with remote server specified
    // with TransientSSLParams::targetedClusterConnectionString.
    const boost::optional<TransientSSLParams> _transientSSLParams;

    // Weak pointer to verify that this manager is still owned by this context.
    synchronized_value<std::weak_ptr<const SSLConnectionContext>> _ownedByContext;

    Mutex _sharedResponseMutex = MONGO_MAKE_LATCH("OCSPStaplingJobRunner::_sharedResponseMutex");
    std::shared_ptr<OCSPStaplingContext> _ocspStaplingContext;

    OCSPFetcher _fetcher;
    OCSPRefreshBackoff _fetcherBackoff;

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
            stdx::lock_guard<Latch> lock(_mutex);
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

        /**
         * This method can only return a cached password and never prompts.
         * @returns cached password if available, error if password is not cached.
         */
        StatusWith<StringData> fetchCachedPasswordNoPrompt() {
            stdx::lock_guard<Latch> lock(_mutex);
            if (_password->size()) {
                return StringData(_password->c_str());
            }
            return Status(ErrorCodes::UnknownError,
                          "Failed to return a cached password, cannot prompt.");
        }

    private:
        Mutex _mutex = MONGO_MAKE_LATCH("PasswordFetcher::_mutex");
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
     * Parse and store x509 subject name from the PEM keyfile.
     * For server instances check that PEM certificate is not expired
     * and extract server certificate notAfter date.
     * @param keyFile referencing the PEM file to be read.
     * @param subjectName as a pointer to the subject name variable being set.
     * @param verifyHasSubjectAlternativeName to generate warning if SAN is absent.
     * @param serverNotAfter a Date_t object pointer that is valued if the
     * date is to be checked (as for a server certificate) and null otherwise.
     * @return Status::Ok showing if the function was successful.
     */
    Status _parseAndValidateCertificate(const std::string& keyFile,
                                        PasswordFetcher* keyPassword,
                                        SSLX509Name* subjectName,
                                        bool verifyHasSubjectAlternativeName,
                                        Date_t* serverNotAfter);

    Status _parseAndValidateCertificateFromBIO(UniqueBIO inBio,
                                               PasswordFetcher* keyPassword,
                                               StringData fileNameForLogging,
                                               SSLX509Name* subjectName,
                                               bool verifyHasSubjectAlternativeName,
                                               Date_t* serverNotAfter);

    Status _parseAndValidateCertificateFromMemory(StringData buffer,
                                                  PasswordFetcher* keyPassword,
                                                  SSLX509Name* subjectName,
                                                  bool verifyHasSubjectAlternativeName,
                                                  Date_t* serverNotAfter);
    /*
     * Parse and return x509 object from the provided keyfile.
     * @param keyFile referencing the PEM file to be read.
     * @param keyPassword password to the PEM file.
     * @return UniqueX509 object parsed from the keyfile.
     */
    UniqueX509 _getX509Object(StringData keyFile, const PasswordFetcher* keyPassword) const;

    /*
     * Retrieve and store certificate information from the provided UniqueX509 object.
     * @param x509 referencing the UniqueX509 object to query.
     * @param info as a pointer to the CertInformationToLog struct to populate
     * with the information.
     */
    static void _getX509CertInfo(UniqueX509& x509,
                                 CertInformationToLog* info,
                                 boost::optional<StringData> keyFile,
                                 boost::optional<StringData> targetClusterURI);

    /*
     * Retrieve and store CRL information from the provided CRL filename.
     * @param crlFile referencing the CRL filename
     * @param info as a pointer to the CRLInformationToLog struct to populate
     * with the information.
     */
    void _getCRLInfo(StringData crlFile, CRLInformationToLog* info) const;

    struct ParsedPeerExtensions {
        stdx::unordered_set<RoleName> roles;
        boost::optional<std::string> clusterMembership;
    };
    StatusWith<ParsedPeerExtensions> _parsePeerExtensions(X509* peerCert) const;

    // Fetches NID for mongodbRolesOID constant.
    static int _getMongoDbRolesNID() {
        return sMongoDbRolesNID;
    }

    // Fetches NID for mongodbClusterMembershipOID constant.
    static int _getMongoDbClusterMembershipNID() {
        return sMongoDbClusterMembershipNID;
    }

    StatusWith<boost::optional<std::vector<DERInteger>>> _parseTLSFeature(X509* peerCert) const;

    /** @return true if was successful, otherwise false */
    bool _setupPEM(SSL_CTX* context, const std::string& keyFile, PasswordFetcher* password) const;

    /**
     * @param payload in-memory payload of a PEM file
     * @return true if was successful, otherwise false
     */
    bool _setupPEMFromMemoryPayload(SSL_CTX* context,
                                    const std::string& payload,
                                    PasswordFetcher* password,
                                    StringData targetClusterURI) const;

    /**
     * Setup PEM from BIO, which could be file or memory input abstraction.
     * @param inBio input BIO, where smart pointer is created with a custom deleter to call
     * 'BIO_free()'.
     * @param keyFile if the certificate was loaded from file, this is the file name. If empty,
     * it means the certificate came from memory payload.
     * @param targetClusterURI If the certificate targets a particular cluster, this is cluster URI.
     * If empty, it means the certificate is the default one for the local cluster.
     * @return true if was successful, otherwise false
     */
    bool _setupPEMFromBIO(SSL_CTX* context,
                          UniqueBIO inBio,
                          PasswordFetcher* password,
                          boost::optional<StringData> keyFile,
                          boost::optional<StringData> targetClusterURI) const;

    /**
     * Loads a certificate chain from memory into context.
     * This method is intended to be a repalcement of API call SSL_CTX_use_certificate_chain_file()
     * but using memory instead of file.
     * @return true if was successful, otherwise false
     */
    static bool _readCertificateChainFromMemory(SSL_CTX* context,
                                                const std::string& payload,
                                                PasswordFetcher* password,
                                                boost::optional<StringData> targetClusterURI);

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

    /*
     * Utility method to process the result returned by password Fetcher.
     */
    static int _processPasswordFetcherOutput(StatusWith<StringData>* fetcherResult,
                                             char* buf,
                                             int num,
                                             int rwflag);

    /**
     * Callbacks for SSL functions.
     */

    static int password_cb(char* buf, int num, int rwflag, void* userdata);

    /**
     * Special flawor of password callback, which always fails.
     * @return -1.
     */
    static int always_error_password_cb(char* buf, int num, int rwflag, void* userdata);
    static int servername_cb(SSL* s, int* al, void* arg);
    static int verify_cb(int ok, X509_STORE_CTX* ctx);
};

}  // namespace

// Global variable indicating if this is a server or a client instance
bool isSSLServer = false;

extern SSLManagerCoordinator* theSSLManagerCoordinator;

MONGO_INITIALIZER_WITH_PREREQUISITES(SSLManager, ("SetupOpenSSL", "EndStartupOptionHandling"))
(InitializerContext*) {
    if (!isSSLServer || (sslGlobalParams.sslMode.load() != SSLParams::SSLMode_disabled)) {
        theSSLManagerCoordinator = new SSLManagerCoordinator();
    }

    sMongoDbRolesNID = OBJ_create(mongodbRolesOID.identifier.c_str(),
                                  mongodbRolesOID.shortDescription.c_str(),
                                  mongodbRolesOID.longDescription.c_str());

    sMongoDbClusterMembershipNID = OBJ_create(mongodbClusterMembershipOID.identifier.c_str(),
                                              mongodbClusterMembershipOID.shortDescription.c_str(),
                                              mongodbClusterMembershipOID.longDescription.c_str());
}

std::shared_ptr<SSLManagerInterface> SSLManagerInterface::create(
    const SSLParams& params,
    const boost::optional<TransientSSLParams>& transientSSLParams,
    bool isServer) {
    return std::make_shared<SSLManagerOpenSSL>(params, transientSSLParams, isServer);
}

std::shared_ptr<SSLManagerInterface> SSLManagerInterface::create(const SSLParams& params,
                                                                 bool isServer) {
    return std::make_shared<SSLManagerOpenSSL>(
        params, boost::optional<TransientSSLParams>{}, isServer);
}

SSLX509Name getCertificateSubjectX509Name(X509* cert) {
    auto name = X509_get_subject_name(cert);
    return convertX509ToSSLX509Name(name);
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

    std::string sslErr = SSLManagerInterface::getSSLErrorMessage(ERR_get_error());
    massert(15861, "Error creating new SSL object " + sslErr, ssl);

    BIO_new_bio_pair(&internalBIO, BUFFER_SIZE, &networkBIO, BUFFER_SIZE);
    SSL_set_bio(ssl, internalBIO, internalBIO);

    if (len > 0) {
        int toBIO = BIO_write(networkBIO, initialBytes, len);
        if (toBIO != len) {
            LOGV2_DEBUG(23223, 3, "Failed to write initial network data to the SSL BIO layer");
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

SSLManagerOpenSSL::SSLManagerOpenSSL(const SSLParams& params,
                                     const boost::optional<TransientSSLParams>& transientSSLParams,
                                     bool isServer)
    : _serverContext(nullptr),
      _clientContext(nullptr),
      _ocspCertStore(nullptr),
      _weakValidation(params.sslWeakCertificateValidation),
      _allowInvalidCertificates(params.sslAllowInvalidCertificates),
      _allowInvalidHostnames(params.sslAllowInvalidHostnames),
      _suppressNoCertificateWarning(params.suppressNoTLSPeerCertificateWarning),
      _transientSSLParams(transientSSLParams),
      _fetcher(this),
      _serverPEMPassword(params.sslPEMKeyPassword, "Enter PEM passphrase"),
      _clusterPEMPassword(params.sslClusterPassword, "Enter cluster certificate passphrase") {
    if (!_initSynchronousSSLContext(&_clientContext, params, ConnectionDirection::kOutgoing)) {
        uasserted(16768, "ssl initialization problem");
    }

    if (_transientSSLParams.has_value()) {
        // No other initialization is necessary: this is egress connection manager that
        // is not using local PEM files.
        LOGV2_DEBUG(54090, 1, "Default params are ignored for transient SSL manager");
        return;
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
        auto status = _parseAndValidateCertificate(
            clientPEM, clientPassword, &_sslConfiguration.clientSubjectName, false, nullptr);
        uassertStatusOKWithContext(
            status,
            str::stream() << "ssl client initialization problem for certificate: " << clientPEM);
    }

    // SSL server specific initialization
    if (isServer) {
        if (!_initSynchronousSSLContext(&_serverContext, params, ConnectionDirection::kIncoming)) {
            uasserted(16562, "ssl initialization problem");
        }

        SSLX509Name serverSubjectName;
        auto status =
            _parseAndValidateCertificate(params.sslPEMKeyFile,
                                         &_serverPEMPassword,
                                         &serverSubjectName,
                                         true,
                                         &_sslConfiguration.serverCertificateExpirationDate);
        uassertStatusOKWithContext(status,
                                   str::stream()
                                       << "ssl server initialization problem for certificate: "
                                       << params.sslPEMKeyFile);

        uassertStatusOK(_sslConfiguration.setServerSubjectName(std::move(serverSubjectName)));
        uassertStatusOK(_sslConfiguration.setClusterAuthX509Attributes());

        CertificateExpirationMonitor::get()->updateExpirationDeadline(
            _sslConfiguration.serverCertificateExpirationDate);

        if (tlsOCSPEnabled) {
            OpenSSLDeleter<decltype(::X509_STORE_free), ::X509_STORE_free> deleter;
            _ocspCertStore = std::shared_ptr<X509_STORE>(X509_STORE_new(), deleter);
            if (!_ocspCertStore) {
                uasserted(5771600, "failed to allocate X509 store for OCSP fetcher");
            }

            LOGV2_DEBUG(5771602, 1, "Loading ocsp store", "cafile"_attr = params.sslCAFile);
            int result = params.sslCAFile.empty()
                ? X509_STORE_set_default_paths(_ocspCertStore.get())
                : X509_STORE_load_locations(
                      _ocspCertStore.get(), params.sslCAFile.c_str(), nullptr);

            if (result == 0) {
                uasserted(5771601, "failed to load certificates into OCSP X509 store");
            }
        }
    }
}

int SSLManagerOpenSSL::password_cb(char* buf, int num, int rwflag, void* userdata) {
    invariant(userdata);
    auto pwFetcher = static_cast<PasswordFetcher*>(userdata);
    auto swPassword = pwFetcher->fetchPassword();
    return _processPasswordFetcherOutput(&swPassword, buf, num, rwflag);
}

int SSLManagerOpenSSL::always_error_password_cb(char* buf, int num, int rwflag, void* userdata) {
    return -1;
}

int SSLManagerOpenSSL::_processPasswordFetcherOutput(StatusWith<StringData>* swPassword,
                                                     char* buf,
                                                     int num,
                                                     int rwflag) {
    // Unless OpenSSL misbehaves, num should always be positive
    fassert(17314, num > 0);

    if (!swPassword->isOK()) {
        LOGV2_ERROR(23239, "Unable to fetch password", "error"_attr = swPassword->getStatus());
        return -1;
    }
    StringData password = std::move(swPassword->getValue());

    const size_t copyCount = std::min(password.size(), static_cast<size_t>(num));
    std::copy_n(password.begin(), copyCount, buf);
    buf[copyCount] = '\0';

    return copyCount;
}

int SSLManagerOpenSSL::servername_cb(SSL* s, int* al, void* arg) {
    // Unconditionally accept the SNI presented by the client. This will ensure that if the client
    // later performs session resumption, subsequent connections will still have access to the SNI.
    return SSL_TLSEXT_ERR_OK;
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

int ocspServerCallback(SSL* ssl, void* arg) {
    std::shared_ptr<OCSPStaplingContext> context =
        static_cast<SSLManagerOpenSSL*>(arg)->getOcspStaplingContext();

    if (!context || !context->sharedResponseForServer) {
        return SSL_TLSEXT_ERR_NOACK;
    }

    unsigned char* ocspResponseBuffer = NULL;
    int length = i2d_OCSP_RESPONSE(context->sharedResponseForServer.get(), &ocspResponseBuffer);

    if (length == 0) {
        return SSL_TLSEXT_ERR_NOACK;
    }

    SSL_set_tlsext_status_ocsp_resp(ssl, ocspResponseBuffer, length);

    return SSL_TLSEXT_ERR_OK;
}

// If the OCSP response says any certificate is revoked, we return the error code associated with
// that. If the function returns a StatusWith<true>, the peer certificate is verified. If the
// function returns false, the peer certificate has not yet been verified.
StatusWith<bool> verifyStapledResponse(SSL* conn, X509* peerCert, OCSP_RESPONSE* response) {
    UniqueOpenSSLStringStack aiaOCSP(X509_get1_ocsp(peerCert));
    if (!aiaOCSP) {
        return true;
    }

    // OCSP checks. AIA stands for the Authority Information Access x509 extension.
    ERR_clear_error();
    auto intermediateCerts = SSLgetVerifiedChain(conn);

    auto context = SSL_get_SSL_CTX(conn);
    auto swIssuerCert = getIssuerCertForCert(context, peerCert, intermediateCerts.get());
    if (!swIssuerCert.isOK()) {
        return swIssuerCert.getStatus();
    }

    auto issuerCert = std::move(swIssuerCert.getValue());

    auto certId = getCertIdForCert(peerCert, issuerCert.get());
    OCSPCertIDSet requestedCertIds;
    requestedCertIds.insert(std::move(certId));

    auto swCertIDSetAndDuration = parseAndValidateOCSPResponse(
        context, nullptr, response, intermediateCerts.get(), issuerCert.get(), requestedCertIds);

    if (swCertIDSetAndDuration.getStatus() == ErrorCodes::OCSPCertificateStatusRevoked) {
        return swCertIDSetAndDuration.getStatus();
    }

    if (swCertIDSetAndDuration.isOK()) {
        return true;
    }

    return false;
}

// The definition of the callbacks
// https://www.openssl.org/docs/man1.0.2/man3/SSL_CTX_set_tlsext_status_cb.html
constexpr int OCSP_CLIENT_RESPONSE_NOT_ACCEPTABLE = 0;
constexpr int OCSP_CLIENT_RESPONSE_ERROR = -1;
constexpr int OCSP_CLIENT_RESPONSE_ACCEPTABLE = 1;

int ocspClientCallback(SSL* ssl, void* arg) {
    if (getSSLGlobalParams().sslAllowInvalidCertificates) {
        return OCSP_CLIENT_RESPONSE_ACCEPTABLE;
    }

    const unsigned char* response_ptr = NULL;
    long length = SSL_get_tlsext_status_ocsp_resp(ssl, &response_ptr);

    if (length <= 0) {
        return OCSP_CLIENT_RESPONSE_ACCEPTABLE;
    }

    UniqueX509 peerCert(SSL_get_peer_certificate(ssl));
    if (!peerCert) {
        LOGV2_DEBUG(23224,
                    1,
                    "Could not get peer certificate from SSL object in OCSP verification callback. "
                    "Will continue with the connection.");

        return OCSP_CLIENT_RESPONSE_ACCEPTABLE;
    }

    UniqueOpenSSLStringStack aiaOCSP(X509_get1_ocsp(peerCert.get()));

    if (!aiaOCSP) {
        return OCSP_CLIENT_RESPONSE_ACCEPTABLE;
    }

    auto response = UniqueOCSPResponse(d2i_OCSP_RESPONSE(NULL, &response_ptr, length));

    auto swStapleOK = verifyStapledResponse(ssl, peerCert.get(), response.get());

    // The swStapleOK object has three states. If the status returned by the function is
    // ErrorCodes::OCSPCertificateStatusRevoked, that means that the peer certificate has
    // been revoked. If the status is OK but the value is false, that means that the respose
    // doesn't verify the status of the peer certificate and we need to verify that using
    // CRLs or check with the OCSP responder ourselves. If it is true, then we are done.
    if (!swStapleOK.isOK()) {
        if (swStapleOK.getStatus() == ErrorCodes::OCSPCertificateStatusRevoked) {
            LOGV2_DEBUG(23225,
                        1,
                        "Stapled OCSP Response validation failed: {error}",
                        "Stapled OCSP Response validation failed",
                        "error"_attr = swStapleOK.getStatus());
            return OCSP_CLIENT_RESPONSE_NOT_ACCEPTABLE;
        }

        LOGV2_ERROR(4781101,
                    "Stapled OCSP Response validation threw an error: {error}",
                    "Stapled OCSP Response validation threw an error",
                    "error"_attr = swStapleOK.getStatus());

        return OCSP_CLIENT_RESPONSE_ERROR;
    } else if (!swStapleOK.getValue()) {
        LOGV2_DEBUG(23226,
                    1,
                    "Stapled Certificate validation failed: Stapled response does not contain "
                    "status information regarding the peer certificate.");
        return OCSP_CLIENT_RESPONSE_NOT_ACCEPTABLE;
    }

    return OCSP_CLIENT_RESPONSE_ACCEPTABLE;
}

/*
 * According to policy decided with drivers, the shell should verify the peer certificate with the
 * stapled response. If that works, no more work is required. If it doesn't work, it should verify
 * the chain using a CRL. If no CRL is provided then the shell should reach out to the OCSP
 * responders itself and verify the status of the peer certificate.
 */
Future<void> SSLManagerOpenSSL::ocspClientVerification(SSL* ssl, const ExecutorPtr& reactor) {
    const unsigned char* response_ptr = NULL;
    long length = SSL_get_tlsext_status_ocsp_resp(ssl, &response_ptr);
    UniqueX509 peerCert(SSL_get_peer_certificate(ssl));

    auto tlsFeature = _parseTLSFeature(peerCert.get());
    if (!tlsFeature.isOK()) {
        return tlsFeature.getStatus();
    }
    auto features = tlsFeature.getValue();
    // this DER INTEGER represents what a MustStaple feature should look like
    DERInteger mustStapleFeature{TLSEXT_TYPE_status_request};
    bool mustStaple = features != boost::none &&
        std::any_of(features->begin(), features->end(), [&](DERInteger feature) {
                          return feature == mustStapleFeature;
                      });

    // If we see that we had a OCSP response, we can assume that it passed the callback
    // verification, so we can bypass other verification.
    if (length > 0) {
        return Status::OK();
    } else if (mustStaple) {
        // mustStaple means the peer cert has to have a stapled response.
        // If length is 0, then there is no stapled response. This is bad.
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      "Peer certificate requires a stapled OCSP response, but none were provided.");
    }

    // Do this after everything else - only if a roundtrip is required.
    UniqueOpenSSLStringStack aiaOCSP(X509_get1_ocsp(peerCert.get()));

    if (!aiaOCSP) {
        return Status::OK();
    }

    // OCSP checks. AIA stands for the Authority Information Access x509 extension.
    ERR_clear_error();
    auto intermediateCerts = SSLgetVerifiedChain(ssl);

    auto context = SSL_get_SSL_CTX(ssl);

    OCSPCacheKey cacheKey(std::move(peerCert), context, std::move(intermediateCerts));

    // Convert
    auto convert =
        [reactor](SharedSemiFuture<OCSPCacheVal> semifuture) mutable -> Future<OCSPCacheVal> {
        if (!reactor) {
            return Future<OCSPCacheVal>::makeReady(std::move(semifuture).get());
        } else {
            auto pf = makePromiseFuture<OCSPCacheVal>();
            std::move(semifuture)
                .thenRunOn(reactor)
                .getAsync(
                    [promise = std::move(pf.promise)](StatusWith<OCSPCacheVal> response) mutable {
                        if (!response.isOK()) {
                            promise.setError(response.getStatus());
                            return;
                        }
                        promise.emplaceValue(std::move(response.getValue()));
                    });

            return std::move(pf.future);
        }
    };

    auto validate = [](StatusWith<OCSPCacheVal> swOcspFetchResponse)
        -> std::pair<Status, boost::optional<Date_t>> {
        // OCSP Status Unknown of some kind
        if (!swOcspFetchResponse.isOK()) {
            return {Status::OK(), boost::none};
        }

        // If lookup returns a boost::none, then it is either because the OCSP
        // cert status is 'Unknown', or the status is 'Good', but the response
        // containing this status cannot be cached due to a missing nextUpdate
        // time. If the status is Unknown, we still let the client connect to
        // the server since the server certificate is not definitively revoked
        if (!swOcspFetchResponse.getValue()) {
            return {Status::OK(), boost::none};
        }

        return {swOcspFetchResponse.getValue()->statusOfResponse,
                swOcspFetchResponse.getValue()->refreshTime};
    };

    auto& cache = OCSPCache::get(getGlobalServiceContext());

    auto refetchIfInvalidAndReturn =
        [&cache, cacheKey, convert, validate](
            std::pair<Status, boost::optional<Date_t>> validatedResponse) mutable -> Future<void> {
        if (!validatedResponse.first.isOK() || !validatedResponse.second) {
            return validatedResponse.first;
        }

        auto timeNow = Date_t::now();

        if (validatedResponse.second.get() < timeNow) {
            cache.invalidateKey(cacheKey);
            auto semifuture = cache.acquireAsync(cacheKey);
            return convert(std::move(semifuture))
                .onCompletion(validate)
                .then([](std::pair<Status, boost::optional<Date_t>> validateResult) {
                    return validateResult.first;
                });
        }

        return validatedResponse.first;
    };

    auto semifuture = cache.acquireAsync(cacheKey);
    return convert(std::move(semifuture)).onCompletion(validate).then(refetchIfInvalidAndReturn);
}

using StoreCtxVerifiedChain = std::unique_ptr<STACK_OF(X509), X509StackDeleter>;

/** getCertificateForContext provides access to the X509* used by the provided SSL_CTX*.
 * OpenSSL 1.0.2 provides SSL_CTX_get0_certificate, which provides direct access to the pointer.
 * OpenSSL 1.0.1 only exposes the pointer on a per-connection basis via SSL_get_certificate.
 * We must provide different implementations depending on the symbols available at compile-time.
 * On 1.0.1, we must ensure that the lifetime of SSL object is longer than the X509 pointer we're
 * inspecting.
 */
#if OPENSSL_VERSION_NUMBER < 0x10002000L
std::tuple<UniqueSSL, X509*> getCertificateForContext(SSL_CTX* context) {
    UniqueSSL ssl(SSL_new(context));
    X509* ret = SSL_get_certificate(ssl.get());
    return std::make_tuple(std::move(ssl), ret);
}
#else
std::tuple<X509*> getCertificateForContext(SSL_CTX* context) {
    return std::make_tuple(SSL_CTX_get0_certificate(context));
}
#endif

#ifdef MONGO_CONFIG_OCSP_STAPLING_ENABLED
Status SSLManagerOpenSSL::stapleOCSPResponse(SSL_CTX* context, bool asyncOCSPStaple) {
    if (MONGO_unlikely(disableStapling.shouldFail()) || !tlsOCSPEnabled) {
        return Status::OK();
    }

    if (!isSSLServer || isTransient()) {
        return Status::OK();
    }

    return _fetcher.start(context, asyncOCSPStaple);
}
#else
Status SSLManagerOpenSSL::stapleOCSPResponse(SSL_CTX* context, bool asyncOCSPStaple) {
    return Status::OK();
}
#endif  // MONGO_CONFIG_OCSP_STAPLING_ENABLED

Status OCSPFetcher::start(SSL_CTX* context, bool asyncOCSPStaple) {
    // Increment the ref count on SSL_CTX by creating a SSL object so that our context lives with
    // the OCSPFetcher
    _ssl = UniqueSSL(SSL_new(context));
    _ca = _manager->getOCSPCertStore();
    invariant(_ca);
    _context = context;
    _ownedByContext = _manager->getSSLContextOwner();
    // Verify the consistent link between manager and the context that owns it.
    invariant(_ownedByContext.lock()->manager.get() == _manager);

    auto certificateHolder = getCertificateForContext(_context);
    _cert = std::get<X509*>(certificateHolder);
    if (!_cert) {
        return getSSLFailure(
            "Could not staple because could not get certificate from SSL Context.");
    }

    UniqueOpenSSLStringStack aiaOCSP(X509_get1_ocsp(_cert));

    if (!aiaOCSP) {
        return Status::OK();
    }

    auto pf = makePromiseFuture<void>();

    auto promisePtr = &pf.promise;
    if (asyncOCSPStaple) {
        promisePtr = nullptr;
    }

    fetchAndStaple(promisePtr)
        .getAsync([this, sm = _manager->shared_from_this()](
                      StatusWith<Milliseconds> swDurationInitial) mutable {
            if (!swDurationInitial.isOK()) {
                LOGV2_WARNING(5512202,
                              "Server was unable to staple OCSP Response",
                              "reason"_attr = swDurationInitial.getStatus());
            }
            startPeriodicJob(swDurationInitial);
        });

    SSL_CTX_set_tlsext_status_cb(context, ocspServerCallback);
    SSL_CTX_set_tlsext_status_arg(context, static_cast<void*>(_manager));

    if (!asyncOCSPStaple) {
        Status stapleStatus = pf.future.getNoThrow();
        if (stapleStatus == ErrorCodes::OCSPCertificateStatusRevoked) {
            return Status(ErrorCodes::OCSPCertificateStatusRevoked,
                          str::stream()
                              << "Certificate is revoked"
                              << SSLManagerInterface::getSSLErrorMessage(ERR_get_error()));
        }
    }

    return Status::OK();
}

Milliseconds getPeriodForStapleJob(StatusWith<Milliseconds> swDuration) {
    if (!swDuration.isOK()) {
        return kOCSPUnknownStatusRefreshRate;
    } else {
        return swDuration.getValue();
    }
}

void OCSPFetcher::startPeriodicJob(StatusWith<Milliseconds> swDurationInitial) {
    stdx::lock_guard<Latch> lock(_staplingMutex);

    if (_ocspStaplingAnchor) {
        return;
    }

    if (_isShutdownConditionLocked(lock)) {
        return;
    }

    _ocspStaplingAnchor =
        getGlobalServiceContext()->getPeriodicRunner()->makeJob(PeriodicRunner::PeriodicJob(
            "OCSP Fetch and Staple",
            [this, sm = _manager->shared_from_this()](Client* client) { doPeriodicJob(); },
            getPeriodForStapleJob(swDurationInitial)));

    _ocspStaplingAnchor.start();
}

void OCSPFetcher::doPeriodicJob() {
    fetchAndStaple(nullptr).getAsync(
        [this, sm = _manager->shared_from_this()](StatusWith<Milliseconds> swDuration) {
            if (!swDuration.isOK()) {
                LOGV2_WARNING(5512201,
                              "Server was unable to staple OCSP Response",
                              "reason"_attr = swDuration.getStatus());
            }

            stdx::lock_guard<Latch> lock(this->_staplingMutex);

            if (_isShutdownConditionLocked(lock)) {
                return;
            }

            this->_ocspStaplingAnchor.setPeriod(getPeriodForStapleJob(swDuration));
        });
}

bool OCSPFetcher::_isShutdownConditionLocked(WithLock lock) {
    if (_shutdown) {
        return true;
    }

    const auto lockedContext = _ownedByContext.lock();
    const auto lockedContextFromManager = _manager->getSSLContextOwner().lock();
    if (!lockedContext) {
        LOGV2(5760101,
              "SSLConnectionContext that should own the manager for the active OCSPFetcher is "
              "deleted");
        _shutdownLocked(lock);
        return true;
    }

    if (lockedContext.get() != lockedContextFromManager.get()) {
        LOGV2_WARNING(5760102,
                      "SSLConnectionContext that should own the manager for the active OCSPFetcher "
                      "doesn't match");
        _shutdownLocked(lock);
        return true;
    }

    return false;
}

#if OPENSSL_VERSION_NUMBER < 0x10100000L
void sslContextGetOtherCerts(SSL_CTX* ctx, STACK_OF(X509) * *sk) {
    SSL_CTX_get_extra_chain_certs(ctx, sk);
}
#else
void sslContextGetOtherCerts(SSL_CTX* ctx, STACK_OF(X509) * *sk) {
    SSL_CTX_get0_chain_certs(ctx, sk);
}
#endif

Future<Milliseconds> OCSPFetcher::fetchAndStaple(Promise<void>* promise) {

    LOGV2_INFO(577164, "OCSP fetch/staple started");

    // Generate a new verified X509StoreContext to get our own certificate chain
    UniqueX509StoreCtx storeCtx(X509_STORE_CTX_new());
    if (!storeCtx) {
        return getSSLFailure("Could not create X509 store.");
    }

    X509_STORE* store = _ca ? _ca.get() : SSL_CTX_get_cert_store(_context);
    if (X509_STORE_CTX_init(storeCtx.get(), store, NULL, NULL) == 0) {
        return getSSLFailure("Could not initialize the X509 Store Context.");
    }

    X509_STORE_CTX_set_cert(storeCtx.get(), _cert);
    STACK_OF(X509) * sk;

    sslContextGetOtherCerts(_context, &sk);
    X509_STORE_CTX_set_chain(storeCtx.get(), sk);

    if (X509_verify_cert(storeCtx.get()) <= 0) {
        return getSSLFailure("Could not verify X509 certificate store for OCSP Stapling.");
    }

    // Extract the chain from the verified X509StoreCtx
    StoreCtxVerifiedChain intermediateCerts(X509_STORE_CTX_get1_chain(storeCtx.get()));

    // Continue with OCSP Stapling logic
    auto swOCSPContext = extractOcspUris(_context, _cert, intermediateCerts.get());
    if (!swOCSPContext.isOK()) {
        LOGV2_WARNING(23232, "Could not staple OCSP response to outgoing certificate.");
        return swOCSPContext.getStatus();
    }

    return dispatchOCSPRequests(_context,
                                _ca,
                                std::move(intermediateCerts),
                                std::move(swOCSPContext.getValue()),
                                OCSPPurpose::kStaple)
        .onCompletion(
            [this, promise](StatusWith<OCSPFetchResponse> swResponse) mutable -> Milliseconds {
                LOGV2_INFO(577165, "OCSP fetch/staple completion");
                stdx::lock_guard<Latch> lock(this->_staplingMutex);

                if (_isShutdownConditionLocked(lock)) {
                    return kOCSPUnknownStatusRefreshRate;
                }

                // protect against pf going out of scope when asynchronous
                if (promise != nullptr) {
                    promise->setWith([&] {
                        return (swResponse == ErrorCodes::OCSPCertificateStatusUnknown)
                            ? Status::OK()
                            : swResponse.getValue().statusOfResponse;
                    });
                }

                LOGV2_INFO(577163, "OCSP response", "status"_attr = swResponse.getStatus());
                return _manager->updateOcspStaplingContextWithResponse(std::move(swResponse));
            });
}

void OCSPFetcher::shutdown() {
    stdx::lock_guard<Mutex> lock(_staplingMutex);
    _shutdownLocked(lock);
}

void OCSPFetcher::_shutdownLocked(WithLock) {
    _shutdown = true;

    if (_ocspStaplingAnchor.isValid()) {
        _ocspStaplingAnchor.stop();
        _ocspStaplingAnchor.detach();
    }
}

void SSLManagerOpenSSL::stopJobs() {
    _fetcher.shutdown();
}

Milliseconds SSLManagerOpenSSL::updateOcspStaplingContextWithResponse(
    StatusWith<OCSPFetchResponse> swResponse) {
    stdx::lock_guard<mongo::Mutex> guard(_sharedResponseMutex);

    if (!swResponse.isOK() || !swResponse.getValue().cacheable()) {
        if (!swResponse.isOK()) {
            LOGV2_WARNING(23233, "Could not staple OCSP response to outgoing certificate.");
        } else {
            LOGV2_WARNING(6144500,
                          "Server will not staple the OCSP response because it is missing a "
                          "nextUpdate time.");
        }

        Milliseconds nextRefreshDuration = _fetcherBackoff.getNextRefreshDuration();

        if (_ocspStaplingContext && _ocspStaplingContext->sharedResponseForServer &&
            _ocspStaplingContext->sharedResponseNextUpdate <
                (Date_t::now() + nextRefreshDuration)) {

            _ocspStaplingContext = std::make_shared<OCSPStaplingContext>();

            LOGV2_WARNING(4633601, "Server will remove and not staple the expiring OCSP Response.");
        }

        return nextRefreshDuration;
    }

    _fetcherBackoff = OCSPRefreshBackoff();

    _ocspStaplingContext = std::make_shared<OCSPStaplingContext>(
        std::move(swResponse.getValue().response), swResponse.getValue().nextStapleRefresh());

    return swResponse.getValue().fetchNewResponseDuration();
}

bool SSLManagerOpenSSL::isTransient() const {
    return _transientSSLParams.has_value();
}

std::string SSLManagerOpenSSL::getTargetedClusterConnectionString() const {
    if (_transientSSLParams.has_value()) {
        return (*_transientSSLParams).targetedClusterConnectionString.toString();
    }
    return {};
}

Status SSLManagerOpenSSL::initSSLContext(SSL_CTX* context,
                                         const SSLParams& params,
                                         ConnectionDirection direction) {
    if (isTransient()) {
        LOGV2_DEBUG(5270602,
                    2,
                    "Initializing transient egress SSL context",
                    "targetClusterConnectionString"_attr =
                        (*_transientSSLParams).targetedClusterConnectionString);
    }

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

#if OPENSSL_VERSION_NUMBER >= 0x10100000
    // OpenSSL pre-1.1.0 isn't ABI compatable enough to ever work, so skip it.
#ifndef SSL_OP_NO_RENEGOTIATION
#define SSL_OP_NO_RENEGOTIATION 0x40000000U
#endif
    if (OpenSSL_version_num() >= 0x10100080) {
        /* SSL_OP_NO_RENEGOTIATION added in 1.1.0h (0x10100080)
         * but we might be compiling with 1.1.0(a-g).
         * Allow this option to be specified at runtime
         * in this specific window.
         */
        options |= SSL_OP_NO_RENEGOTIATION;
    }
#endif

    ::SSL_CTX_set_options(context, options);

    if (0 == ::SSL_CTX_set_cipher_list(context, params.sslCipherConfig.c_str())) {
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "Can not set supported cipher suites with config string \""
                                    << params.sslCipherConfig
                                    << "\": " << getSSLErrorMessage(ERR_get_error()));
    }

    if (!params.sslCipherSuiteConfig.empty()) {
        // OpenSSL versions older than version 1.1.1 are not allowed to configure their cipher
        // suites using the sslCipherSuiteConfig flag.
        if (0 == ::SSL_CTX_set_ciphersuites(context, params.sslCipherSuiteConfig.c_str())) {
            return Status(ErrorCodes::InvalidSSLConfiguration,
                          str::stream()
                              << "Can not set supported cipher suites with config string \""
                              << params.sslCipherSuiteConfig
                              << "\": " << getSSLErrorMessage(ERR_get_error()));
        }
    }

    // We use the address of the context as the session id context.
    if (0 ==
        ::SSL_CTX_set_session_id_context(
            context, reinterpret_cast<unsigned char*>(&context), sizeof(context))) {
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "Can not store ssl session id context: "
                                    << getSSLErrorMessage(ERR_get_error()));
    }

    // We should accept all SNI extensions advertised by clients
    if (1 != SSL_CTX_set_tlsext_servername_callback(context, &SSLManagerOpenSSL::servername_cb)) {
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "Can not set servername callback: "
                                    << getSSLErrorMessage(ERR_get_error()));
    }


    if (direction == ConnectionDirection::kOutgoing && _transientSSLParams) {

        // Transient params for outgoing connection have priority over global params.
        if (!_setupPEMFromMemoryPayload(
                context,
                (*_transientSSLParams).sslClusterPEMPayload,
                &_clusterPEMPassword,
                (*_transientSSLParams).targetedClusterConnectionString.toString())) {
            return Status(ErrorCodes::InvalidSSLConfiguration,
                          str::stream() << "Can not set up transient ssl cluster certificate for "
                                        << (*_transientSSLParams).targetedClusterConnectionString);
        }

        auto status =
            _parseAndValidateCertificateFromMemory((*_transientSSLParams).sslClusterPEMPayload,
                                                   &_clusterPEMPassword,
                                                   &_sslConfiguration.clientSubjectName,
                                                   false,
                                                   nullptr);
        if (!status.isOK()) {
            return status.withContext("Could not validate transient certificate");
        }

    } else if (direction == ConnectionDirection::kOutgoing && params.tlsWithholdClientCertificate) {
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

    if (tlsOCSPEnabled) {
        if (direction == SSLManagerInterface::ConnectionDirection::kOutgoing) {
            // This should only induce an extra network call if there is no stapled response
            // and there is no CRL.
            SSL_CTX_set_tlsext_status_cb(context, ocspClientCallback);
            SSL_CTX_set_tlsext_status_arg(context, nullptr);
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
                LOGV2_ERROR(23240,
                            "Failed to set default DH parameters: {error}",
                            "Failed to set default DH parameters",
                            "error"_attr = getSSLErrorMessage(ERR_get_error()));
            }
        }
    }

    return Status::OK();
}

void SSLManagerOpenSSL::registerOwnedBySSLContext(
    std::weak_ptr<const SSLConnectionContext> ownedByContext) {
    _ownedByContext = ownedByContext;
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

Status SSLManagerOpenSSL::_parseAndValidateCertificate(const std::string& keyFile,
                                                       PasswordFetcher* keyPassword,
                                                       SSLX509Name* subjectName,
                                                       bool verifyHasSubjectAlternativeName,
                                                       Date_t* serverCertificateExpirationDate) {
    UniqueBIO inBio(BIO_new(BIO_s_file()));
    if (!inBio) {
        return Status(
            ErrorCodes::InvalidSSLConfiguration,
            "Failed to allocate BIO object. error: {}"_format(getSSLErrorMessage(ERR_get_error())));
    }

    if (BIO_read_filename(inBio.get(), keyFile.c_str()) <= 0) {
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      "Cannot read key file '{}' when setting subject name. error: {}"_format(
                          keyFile, getSSLErrorMessage(ERR_get_error())));
    }

    return _parseAndValidateCertificateFromBIO(std::move(inBio),
                                               keyPassword,
                                               keyFile,
                                               subjectName,
                                               verifyHasSubjectAlternativeName,
                                               serverCertificateExpirationDate);
}

Status SSLManagerOpenSSL::_parseAndValidateCertificateFromMemory(
    StringData buffer,
    PasswordFetcher* keyPassword,
    SSLX509Name* subjectName,
    bool verifyHasSubjectAlternativeName,
    Date_t* serverCertificateExpirationDate) {
    logv2::DynamicAttributes errorAttrs;

#if OPENSSL_VERSION_NUMBER <= 0x1000114fL
    UniqueBIO inBio(BIO_new_mem_buf(const_cast<char*>(buffer.rawData()), buffer.size()));
#else
    UniqueBIO inBio(BIO_new_mem_buf(buffer.rawData(), buffer.size()));
#endif

    if (!inBio) {
        CaptureSSLErrorInAttrs capture(errorAttrs);
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      "Failed to allocate BIO object from in-memory payload. error: {}"_format(
                          getSSLErrorMessage(ERR_get_error())));
    }

    return _parseAndValidateCertificateFromBIO(std::move(inBio),
                                               keyPassword,
                                               "transient"_sd,
                                               subjectName,
                                               verifyHasSubjectAlternativeName,
                                               serverCertificateExpirationDate);
}

Status SSLManagerOpenSSL::_parseAndValidateCertificateFromBIO(
    UniqueBIO inBio,
    PasswordFetcher* keyPassword,
    StringData fileNameForLogging,
    SSLX509Name* subjectName,
    bool verifyHasSubjectAlternativeName,
    Date_t* serverCertificateExpirationDate) {
    UniqueX509 x509(PEM_read_bio_X509(
        inBio.get(), nullptr, &SSLManagerOpenSSL::password_cb, static_cast<void*>(&keyPassword)));
    if (x509 == nullptr) {
        return Status(
            ErrorCodes::InvalidSSLConfiguration,
            "Cannot retrieve certificate from keyfile '{}' when setting subject name. error: {}"_format(
                fileNameForLogging, getSSLErrorMessage(ERR_get_error())));
    }

    *subjectName = getCertificateSubjectX509Name(x509.get());

    if (verifyHasSubjectAlternativeName) {
        bool hasSan = false;
        STACK_OF(GENERAL_NAME)* sanNames = static_cast<STACK_OF(GENERAL_NAME)*>(
            X509_get_ext_d2i(x509.get(), NID_subject_alt_name, nullptr, nullptr));
        if (nullptr != sanNames) {
            int sanNamesCount = sk_GENERAL_NAME_num(sanNames);
            hasSan = (0 != sanNamesCount);
            sk_GENERAL_NAME_pop_free(sanNames, GENERAL_NAME_free);
        }

        if (!hasSan) {
            LOGV2_WARNING_OPTIONS(551190,
                                  {logv2::LogTag::kStartupWarnings},
                                  "Server certificate has no compatible Subject Alternative Name. "
                                  "This may prevent TLS clients from connecting");
        }
    }

    auto notBeforeMillis = convertASN1ToMillis(X509_get_notBefore(x509.get()));
    if (notBeforeMillis == Date_t()) {
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      "notBefore certificate date conversion failed");
    }

    auto notAfterMillis = convertASN1ToMillis(X509_get_notAfter(x509.get()));
    if (notAfterMillis == Date_t()) {
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      "notAfter certificate date conversion failed");
    }

    auto now = Date_t::now();
    if ((notBeforeMillis > now) || (now > notAfterMillis)) {
        return Status(
            ErrorCodes::InvalidSSLConfiguration,
            "The provided SSL certificate is expired or not yet valid. notBefore {}, notAfter {}"_format(
                notBeforeMillis.toString(), notAfterMillis.toString()));
    }

    if (serverCertificateExpirationDate != nullptr) {
        *serverCertificateExpirationDate = notAfterMillis;
    }

    return Status::OK();
}

// static
bool SSLManagerOpenSSL::_readCertificateChainFromMemory(
    SSL_CTX* context,
    const std::string& payload,
    PasswordFetcher* password,
    boost::optional<StringData> targetClusterURI) {

    logv2::DynamicAttributes errorAttrs;
    if (targetClusterURI) {
        errorAttrs.add("targetClusterURI", *targetClusterURI);
    }

    ERR_clear_error();  // Clear error stack for SSL_CTX_use_certificate().

    // Note: old versions of SSL take (void*) here but it's still R/O.
#if OPENSSL_VERSION_NUMBER <= 0x1000114fL
    UniqueBIO inBio(BIO_new_mem_buf(const_cast<char*>(payload.c_str()), payload.length()));
#else
    UniqueBIO inBio(BIO_new_mem_buf(payload.c_str(), payload.length()));
#endif

    if (!inBio) {
        CaptureSSLErrorInAttrs capture(errorAttrs);
        LOGV2_ERROR(5159905, "Failed to allocate BIO from in memory payload", errorAttrs);
        return false;
    }

    auto password_cb =
        &SSLManagerOpenSSL::always_error_password_cb;  // We don't expect a password to be required.
    void* userdata = static_cast<void*>(password);
    UniqueX509 x509cert(PEM_read_bio_X509_AUX(inBio.get(), NULL, password_cb, userdata));

    if (!x509cert) {
        CaptureSSLErrorInAttrs capture(errorAttrs);
        LOGV2_ERROR(5159906, "Failed to read the X509 certificate from memory", errorAttrs);
        return false;
    }

    CertInformationToLog debugInfo;
    _getX509CertInfo(x509cert, &debugInfo, boost::none, targetClusterURI);
    logCert(debugInfo, "", 5159903);

    // SSL_CTX_use_certificate increments the refcount on cert.
    if (1 != SSL_CTX_use_certificate(context, x509cert.get())) {
        CaptureSSLErrorInAttrs capture(errorAttrs);
        LOGV2_ERROR(5159907, "Failed to use the X509 certificate loaded from memory", errorAttrs);
        return false;
    }

    // If we could set up our certificate, now proceed to the CA certificates.
    UniqueX509 ca;
#if OPENSSL_VERSION_NUMBER >= 0x100010fFL
    SSL_CTX_clear_chain_certs(context);
#else
    SSL_CTX_clear_extra_chain_certs(context);
#endif
    while ((ca = UniqueX509(PEM_read_bio_X509(inBio.get(), NULL, password_cb, userdata)))) {
#if OPENSSL_VERSION_NUMBER >= 0x100010fFL
        if (1 != SSL_CTX_add1_chain_cert(context, ca.get())) {
#else
        if (1 != SSL_CTX_add_extra_chain_cert(context, ca.get())) {
#endif
            CaptureSSLErrorInAttrs capture(errorAttrs);
            LOGV2_ERROR(
                5159908, "Failed to use the CA X509 certificate loaded from memory", errorAttrs);
            return false;
        }
        _getX509CertInfo(ca, &debugInfo, boost::none, targetClusterURI);
        logCert(debugInfo, "", 5159902);
#if OPENSSL_VERSION_NUMBER < 0x100010fFL
        ca.release();  // Older version add_extra_chain_cert takes over the pointer without
                       // incrementing refcount. Avoid double free.
#endif
    }
    // When the while loop ends, it's usually just EOF.
    auto err = ERR_peek_last_error();
    if (ERR_GET_LIB(err) == ERR_LIB_PEM && ERR_GET_REASON(err) == PEM_R_NO_START_LINE) {
        ERR_clear_error();
    } else {
        CaptureSSLErrorInAttrs capture(errorAttrs);
        LOGV2_ERROR(
            5159909, "Error remained after scanning all X509 certificates from memory", errorAttrs);
        return false;  // Some real error.
    }

    return true;
}

bool SSLManagerOpenSSL::_setupPEM(SSL_CTX* context,
                                  const std::string& keyFile,
                                  PasswordFetcher* password) const {
    logv2::DynamicAttributes errorAttrs;
    errorAttrs.add("keyFile", keyFile);

    if (SSL_CTX_use_certificate_chain_file(context, keyFile.c_str()) != 1) {
        CaptureSSLErrorInAttrs capture(errorAttrs);
        LOGV2_ERROR(23248, "Cannot read certificate file", errorAttrs);
        return false;
    }

    UniqueBIO inBio(BIO_new(BIO_s_file()));

    if (!inBio) {
        CaptureSSLErrorInAttrs capture(errorAttrs);
        LOGV2_ERROR(23249, "Failed to allocate BIO object", errorAttrs);
        return false;
    }

    if (BIO_read_filename(inBio.get(), keyFile.c_str()) <= 0) {
        CaptureSSLErrorInAttrs capture(errorAttrs);
        LOGV2_ERROR(23250, "Cannot read PEM key file", errorAttrs);
        return false;
    }
    return _setupPEMFromBIO(context, std::move(inBio), password, StringData{keyFile}, boost::none);
}

bool SSLManagerOpenSSL::_setupPEMFromMemoryPayload(SSL_CTX* context,
                                                   const std::string& payload,
                                                   PasswordFetcher* password,
                                                   StringData targetClusterURI) const {
    logv2::DynamicAttributes errorAttrs;
    errorAttrs.add("targetClusterURI", targetClusterURI);

    if (!_readCertificateChainFromMemory(context, payload, password, targetClusterURI)) {
        return false;
    }
#if OPENSSL_VERSION_NUMBER <= 0x1000114fL
    UniqueBIO inBio(BIO_new_mem_buf(const_cast<char*>(payload.c_str()), payload.length()));
#else
    UniqueBIO inBio(BIO_new_mem_buf(payload.c_str(), payload.length()));
#endif

    if (!inBio) {
        CaptureSSLErrorInAttrs capture(errorAttrs);
        LOGV2_ERROR(5159901, "Failed to allocate BIO object from in-memory payload", errorAttrs);
        return false;
    }

    return _setupPEMFromBIO(context, std::move(inBio), password, boost::none, targetClusterURI);
}

bool SSLManagerOpenSSL::_setupPEMFromBIO(SSL_CTX* context,
                                         UniqueBIO inBio,
                                         PasswordFetcher* password,
                                         boost::optional<StringData> keyFile,
                                         boost::optional<StringData> targetClusterURI) const {
    logv2::DynamicAttributes errorAttrs;
    if (keyFile) {
        errorAttrs.add("keyFile", *keyFile);
    }
    if (targetClusterURI) {
        errorAttrs.add("targetClusterURI", *targetClusterURI);
    }

    // Obtain the private key, using our callback to acquire a decryption password if necessary.
    auto password_cb = &SSLManagerOpenSSL::password_cb;
    void* userdata = static_cast<void*>(password);
    EVP_PKEY* privateKey = PEM_read_bio_PrivateKey(inBio.get(), nullptr, password_cb, userdata);
    if (!privateKey) {
        CaptureSSLErrorInAttrs capture(errorAttrs);
        LOGV2_ERROR(23251, "Cannot read PEM key", errorAttrs);
        return false;
    }
    const ScopeGuard privateKeyGuard([&privateKey]() { EVP_PKEY_free(privateKey); });

    if (SSL_CTX_use_PrivateKey(context, privateKey) != 1) {
        CaptureSSLErrorInAttrs capture(errorAttrs);
        LOGV2_ERROR(23252, "Cannot use PEM key", errorAttrs);
        return false;
    }

    // Verify that the certificate and the key go together.
    if (SSL_CTX_check_private_key(context) != 1) {
        CaptureSSLErrorInAttrs capture(errorAttrs);
        LOGV2_ERROR(23253, "SSL certificate validation failed", errorAttrs);
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

Status SSLManagerOpenSSL::_setupSystemCA(SSL_CTX* context) {
    // The OpenSSL libraries should have been configured
    // with default locations for CA certificates.
    if (SSL_CTX_set_default_verify_paths(context) != 1) {
        return {
            ErrorCodes::InvalidSSLConfiguration,
            str::stream() << "error loading system CA certificates "
                          << "(default certificate file: " << X509_get_default_cert_file() << ", "
                          << "default certificate path: " << X509_get_default_cert_dir() << ")"};
    }

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
        LOGV2_ERROR(23254,
                    "cannot read CRL file: {crlFile} {error}",
                    "Cannot read CRL file",
                    "crlFile"_attr = crlFile,
                    "error"_attr = getSSLErrorMessage(ERR_get_error()));
        return false;
    }

    if (status == 1) {
        LOGV2(4652601, "ssl imported 1 revoked certificate from the revocation list.");
    } else {
        LOGV2(4652602,
              "ssl imported {numberCerts} revoked certificates from the revocation list",
              "SSL imported revoked certificates from the revocation list",
              "numberCerts"_attr = status);
    }

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
            LOGV2_DEBUG(23228, 3, "Failed to write network data to the SSL BIO layer");
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

    int ret;
    if (!undotted.empty()) {
        // only have TLS advertise host name as SNI if it is not an IP address
        std::array<uint8_t, INET6_ADDRSTRLEN> unusedBuf;
        if ((inet_pton(AF_INET, undotted.c_str(), unusedBuf.data()) == 0) &&
            (inet_pton(AF_INET6, undotted.c_str(), unusedBuf.data()) == 0)) {
            ret = ::SSL_set_tlsext_host_name(sslConn->ssl, undotted.c_str());
            if (ret != 1) {
                _handleSSLError(sslConn.get(), ret);
            }
        }
    }

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

namespace {
Status _validatePeerRoles(const stdx::unordered_set<RoleName>& embeddedRoles, SSL* conn) {
    if (embeddedRoles.empty()) {
        // Nothing offered, nothing to restrict.
        return Status::OK();
    }

    if (!sslGlobalParams.tlsCATrusts) {
        // Nothing restricted.
        return Status::OK();
    }

    const auto& tlsCATrusts = sslGlobalParams.tlsCATrusts.get();
    if (tlsCATrusts.empty()) {
        // Nothing permitted.
        return {ErrorCodes::BadValue,
                "tlsCATrusts parameter prohibits role based authorization via X509 certificates"};
    }

    auto stack = SSLgetVerifiedChain(conn);
    if (!stack || !sk_X509_num(stack.get())) {
        return {ErrorCodes::BadValue, "Unable to obtain certificate chain"};
    }

    auto root = sk_X509_value(stack.get(), sk_X509_num(stack.get()) - 1);
    auto digest = SHA256Block::HashType();
    if (!X509_digest(root, EVP_sha256(), digest.data(), nullptr)) {
        return {ErrorCodes::BadValue, "Unable to digest root certificate"};
    }

    SHA256Block sha256(digest);
    auto it = tlsCATrusts.find(sha256);
    if (it == tlsCATrusts.end()) {
        return {
            ErrorCodes::BadValue,
            str::stream() << "CA: " << sha256.toHexString()
                          << " is not authorized to grant any roles due to tlsCATrusts parameter"};
    }

    auto allowedRoles = it->second;
    // See TLSCATrustsSetParameter::set() for a description of tlsCATrusts format.
    if (allowedRoles.count(RoleName())) {
        // CA is authorized for all role assignments.
        return Status::OK();
    }

    for (const auto& role : embeddedRoles) {
        // Check for exact match or wildcard matches.
        if (!allowedRoles.count(role) && !allowedRoles.count(RoleName(role.getRole(), "")) &&
            !allowedRoles.count(RoleName("", role.getDB()))) {
            return {ErrorCodes::BadValue,
                    str::stream() << "CA: " << sha256.toHexString()
                                  << " is not authorized to grant role " << role
                                  << " due to tlsCATrusts parameter"};
        }
    }

    return Status::OK();
}

}  // namespace

Future<SSLPeerInfo> SSLManagerOpenSSL::parseAndValidatePeerCertificate(
    SSL* conn,
    boost::optional<std::string> sni,
    const std::string& remoteHost,
    const HostAndPort& hostForLogging,
    const ExecutorPtr& reactor) {

    auto tlsVersionStatus = mapTLSVersion(conn);
    if (!tlsVersionStatus.isOK()) {
        return tlsVersionStatus.getStatus();
    }

    recordTLSVersion(tlsVersionStatus.getValue(), hostForLogging);

    if (!_sslConfiguration.hasCA && isSSLServer)
        return SSLPeerInfo(sni);

    UniqueX509 peerCert(SSL_get_peer_certificate(conn));

    if (nullptr == peerCert) {  // no certificate presented by peer
        if (_weakValidation) {
            // do not give warning if certificate warnings are  suppressed
            if (!_suppressNoCertificateWarning) {
                LOGV2_WARNING(23234,
                              "no SSL certificate provided by peer",
                              "No SSL certificate provided by peer");
            }
            return SSLPeerInfo(sni);
        } else {
            LOGV2_ERROR(23255,
                        "no SSL certificate provided by peer; connection rejected",
                        "No SSL certificate provided by peer; connection rejected");
            return Status(ErrorCodes::SSLHandshakeFailed,
                          "no SSL certificate provided by peer; connection rejected");
        }
    }

    long result = SSL_get_verify_result(conn);

    if (result != X509_V_OK) {
        if (_allowInvalidCertificates) {
            LOGV2_WARNING(23235,
                          "SSL peer certificate validation failed: {reason}",
                          "SSL peer certificate validation failed",
                          "reason"_attr = X509_verify_cert_error_string(result));
            return SSLPeerInfo(sni);
        } else {
            str::stream msg;
            msg << "SSL peer certificate validation failed: "
                << X509_verify_cert_error_string(result);
            LOGV2_ERROR(23256,
                        "{error}",
                        "SSL peer certificate validation failed",
                        "error"_attr = msg.ss.str());
            return Status(ErrorCodes::SSLHandshakeFailed, msg);
        }
    }

    Future<void> ocspFuture;

    // The check to ensure that remoteHost is empty is to ensure that we only run OCSP
    // verification when we are a client, never as a server.
    if (tlsOCSPEnabled && !remoteHost.empty() && !_allowInvalidCertificates) {

        ocspFuture = ocspClientVerification(conn, reactor);
    }

    // TODO: check optional cipher restriction, using cert.
    auto peerSubject = getCertificateSubjectX509Name(peerCert.get());
    const auto cipher = SSL_get_current_cipher(conn);
    if (!serverGlobalParams.quiet.load() && gEnableDetailedConnectionHealthMetricLogLines) {
        LOGV2_INFO(6723801,
                   "Accepted TLS connection from peer",
                   "peerSubject"_attr = peerSubject,
                   "cipher"_attr = SSL_CIPHER_get_name(cipher));
    }

    auto swParsedPeerExtensions = _parsePeerExtensions(peerCert.get());
    if (!swParsedPeerExtensions.isOK()) {
        return Future<SSLPeerInfo>::makeReady(swParsedPeerExtensions.getStatus());
    }
    auto parsedPeerExtensions = std::move(swParsedPeerExtensions.getValue());

    if (auto status = _validatePeerRoles(parsedPeerExtensions.roles, conn); !status.isOK()) {
        return status;
    }

    // Server side.
    if (remoteHost.empty()) {
        const auto exprThreshold = tlsX509ExpirationWarningThresholdDays;
        if (exprThreshold > 0) {
            const auto expiration = X509_get0_notAfter(peerCert.get());
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
            LOGV2_WARNING(23236, "Client connecting with server's own TLS certificate");
        }

        // void futures are default constructed as ready futures.
        return std::move(ocspFuture)
            .then([peerSubject,
                   sni,
                   roles = std::move(parsedPeerExtensions.roles),
                   clusterMembership = std::move(parsedPeerExtensions.clusterMembership)] {
                SSLPeerInfo info(peerSubject, sni, roles);
                info.setClusterMembership(clusterMembership);
                return info;
            });
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
        X509_get_ext_d2i(peerCert.get(), NID_subject_alt_name, nullptr, nullptr));

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
                    LOGV2_WARNING(23237,
                                  "You have an IP Address in the DNS Name field on your "
                                  "certificate. This formulation is deprecated.");
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
                auto ipAddress =
                    SockAddr(reinterpret_cast<struct sockaddr*>(&ss), sizeof(ss)).getAddr();
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
            LOGV2_WARNING(23238,
                          "The server certificate does not match the host name. Hostname: "
                          "{remoteHost} does not match {certificateNames}",
                          "The server certificate does not match the remote host name",
                          "remoteHost"_attr = remoteHost,
                          "certificateNames"_attr = certificateNames.str());
        } else {
            LOGV2_ERROR(23257,
                        "The server certificate does not match the host name. Hostname: "
                        "{remoteHost} does not match {certificateNames}",
                        "The server certificate does not match the remote host name",
                        "remoteHost"_attr = remoteHost,
                        "certificateNames"_attr = certificateNames.str());
            return Future<SSLPeerInfo>::makeReady(Status(ErrorCodes::SSLHandshakeFailed, msg));
        }
    }

    return std::move(ocspFuture).then([this, peerSubject]() { return SSLPeerInfo(peerSubject); });
}

SSLPeerInfo SSLManagerOpenSSL::parseAndValidatePeerCertificateDeprecated(
    const SSLConnectionInterface* connInterface,
    const std::string& remoteHost,
    const HostAndPort& hostForLogging) {
    const SSLConnectionOpenSSL* conn = checked_cast<const SSLConnectionOpenSSL*>(connInterface);

    auto swPeerSubjectName =
        parseAndValidatePeerCertificate(conn->ssl, boost::none, remoteHost, hostForLogging, nullptr)
            .getNoThrow();
    // We can't use uassertStatusOK here because we need to throw a NetworkException.
    if (!swPeerSubjectName.isOK()) {
        throwSocketError(SocketErrorKind::CONNECT_ERROR, swPeerSubjectName.getStatus().reason());
    }
    return swPeerSubjectName.getValue();
}

StatusWith<SSLManagerOpenSSL::ParsedPeerExtensions> SSLManagerOpenSSL::_parsePeerExtensions(
    X509* peerCert) const {
    // exts is owned by the peerCert
    const STACK_OF(X509_EXTENSION)* exts = X509_get0_extensions(peerCert);

    int extCount = 0;
    if (exts) {
        extCount = sk_X509_EXTENSION_num(exts);
    }

    ASN1_OBJECT* rolesObj = OBJ_nid2obj(_getMongoDbRolesNID());
    ASN1_OBJECT* clusterMembershipObj = OBJ_nid2obj(_getMongoDbClusterMembershipNID());

    // Search all certificate extensions for our own
    ParsedPeerExtensions parsed;
    for (int i = 0; i < extCount; i++) {
        X509_EXTENSION* ex = sk_X509_EXTENSION_value(exts, i);
        ASN1_OBJECT* obj = X509_EXTENSION_get_object(ex);

        if (!OBJ_cmp(obj, rolesObj)) {
            // mongodbRoles extension.
            ASN1_OCTET_STRING* data = X509_EXTENSION_get_data(ex);
            auto ptr = reinterpret_cast<char*>(data->data);
            auto swRoles = parsePeerRoles(ConstDataRange(ptr, ptr + data->length));
            if (!swRoles.isOK()) {
                return std::move(swRoles.getStatus());
            }
            parsed.roles = std::move(swRoles.getValue());
        } else if (!OBJ_cmp(obj, clusterMembershipObj)) {
            // mongodbClusterMembership extension.
            ASN1_OCTET_STRING* data = X509_EXTENSION_get_data(ex);
            auto ptr = reinterpret_cast<char*>(data->data);
            auto swMembership = parseDERString(ConstDataRange(ptr, ptr + data->length));
            if (!swMembership.isOK()) {
                return std::move(swMembership.getStatus());
            }
            parsed.clusterMembership = std::move(swMembership.getValue());
        }
    }

    return parsed;
}

StatusWith<boost::optional<std::vector<DERInteger>>> SSLManagerOpenSSL::_parseTLSFeature(
    X509* peerCert) const {
    // exts is owned by the peerCert
    const STACK_OF(X509_EXTENSION)* exts = X509_get0_extensions(peerCert);

    int extCount = 0;
    if (exts) {
        extCount = sk_X509_EXTENSION_num(exts);
    }

    ASN1_OBJECT* featuresObj = OBJ_nid2obj(NID_tlsfeature);
    for (int i = 0; i < extCount; i++) {
        X509_EXTENSION* ex = sk_X509_EXTENSION_value(exts, i);
        ASN1_OBJECT* obj = X509_EXTENSION_get_object(ex);

        if (!OBJ_cmp(obj, featuresObj)) {
            // We've found an extension which has the features OID
            ASN1_OCTET_STRING* data = X509_EXTENSION_get_data(ex);
            return parseTLSFeature(
                ConstDataRange(reinterpret_cast<char*>(data->data),
                               reinterpret_cast<char*>(data->data) + data->length));
        }
    }
    return boost::none;
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
            LOGV2_ERROR(23258,
                        "SSL: {error}, possibly timed out during connect",
                        "SSL: possibly timed out during connect",
                        "error"_attr = code);
            break;

        case SSL_ERROR_ZERO_RETURN:
            // TODO: Check if we can avoid throwing an exception for this condition
            // If so, change error() back to LOG(3)
            LOGV2_ERROR(23259, "SSL network connection closed");
            break;
        case SSL_ERROR_SYSCALL:
            // If ERR_get_error returned 0, the error queue is empty
            // check the return value of the actual SSL operation
            if (err != 0) {
                LOGV2_ERROR(
                    23260, "SSL: {error}", "SSL error", "error"_attr = getSSLErrorMessage(err));
            } else if (ret == 0) {
                LOGV2_ERROR(23261, "Unexpected EOF encountered during SSL communication");
            } else {
                auto ec = lastSystemError();
                LOGV2_ERROR(23262,
                            "The SSL BIO reported an I/O error {error}",
                            "The SSL BIO reported an I/O error",
                            "error"_attr = errorMessage(ec));
            }
            break;
        case SSL_ERROR_SSL: {
            LOGV2_ERROR(23263, "SSL: {error}", "SSL error", "error"_attr = getSSLErrorMessage(err));
            break;
        }

        default:
            LOGV2_ERROR(23264, "unrecognized SSL error");
            break;
    }
    _flushNetworkBIO(conn);
    throwSocketError(errToThrow, conn->socket->remoteString());
}

UniqueX509 SSLManagerOpenSSL::_getX509Object(StringData keyFile,
                                             const PasswordFetcher* keyPassword) const {
    BIO* inBIO = BIO_new(BIO_s_file());
    if (inBIO == nullptr) {
        uasserted(4913000, "failed to allocate BIO object");
    }

    ON_BLOCK_EXIT([&] { BIO_free(inBIO); });
    if (BIO_read_filename(inBIO, keyFile.toString().c_str()) <= 0) {
        uasserted(4913001,
                  str::stream() << "cannot read key file when setting subject name: " << keyFile
                                << " " << SSLManagerInterface::getSSLErrorMessage(ERR_get_error()));
    }

    UniqueX509 x509(PEM_read_bio_X509(
        inBIO, nullptr, &SSLManagerOpenSSL::password_cb, static_cast<void*>(&keyPassword)));
    if (x509 == nullptr) {
        uasserted(4913002,
                  str::stream() << "cannot retrieve certificate from keyfile: " << keyFile << " "
                                << SSLManagerInterface::getSSLErrorMessage(ERR_get_error()));
    }
    return x509;
}

constexpr size_t kSHA1HashBytes = 20;

// static
void SSLManagerOpenSSL::_getX509CertInfo(UniqueX509& x509,
                                         CertInformationToLog* info,
                                         boost::optional<StringData> keyFile,
                                         boost::optional<StringData> targetClusterURI) {
    if (!x509) {
        return;
    }

    info->subject = getCertificateSubjectX509Name(x509.get());
    info->issuer = convertX509ToSSLX509Name(X509_get_issuer_name(x509.get()));

    info->thumbprint.resize(kSHA1HashBytes);
    int result = X509_digest(
        x509.get(), EVP_sha1(), reinterpret_cast<unsigned char*>(info->thumbprint.data()), nullptr);
    uassert(6014700, "X509 digest failed", result == 1);

    info->hexEncodedThumbprint = hexblob::encode(info->thumbprint.data(), info->thumbprint.size());

    auto notBeforeMillis = convertASN1ToMillis(X509_get_notBefore(x509.get()));

    uassert(4913003, "date conversion failed", notBeforeMillis != Date_t());

    info->validityNotBefore = notBeforeMillis;

    auto notAfterMillis = convertASN1ToMillis(X509_get_notAfter(x509.get()));

    info->validityNotAfter = notAfterMillis;

    uassert(4913004, "date conversion failed", notAfterMillis != Date_t());

    if (keyFile)
        info->keyFile = keyFile->toString();
    if (targetClusterURI)
        info->targetClusterURI = targetClusterURI->toString();
}


void SSLManagerOpenSSL::_getCRLInfo(StringData crlFile, CRLInformationToLog* info) const {
    BIO* inBIO = BIO_new(BIO_s_file());
    if (inBIO == nullptr) {
        uasserted(4913005, "failed to allocate BIO object");
    }

    ON_BLOCK_EXIT([&] { BIO_free(inBIO); });

    if (BIO_read_filename(inBIO, crlFile.toString().c_str()) <= 0) {
        uasserted(4913006,
                  str::stream() << "cannot read crl file when setting subject name: " << crlFile
                                << " " << SSLManagerInterface::getSSLErrorMessage(ERR_get_error()));
    }


    X509_CRL* x509 =
        PEM_read_bio_X509_CRL(inBIO, nullptr, &SSLManagerOpenSSL::password_cb, nullptr);
    if (x509 == nullptr) {
        uasserted(4913007,
                  str::stream() << "cannot retrieve CRL information from crl file: " << crlFile
                                << " " << SSLManagerInterface::getSSLErrorMessage(ERR_get_error()));
    }
    ON_BLOCK_EXIT([&] { X509_CRL_free(x509); });

    info->thumbprint.resize(kSHA1HashBytes);
    X509_CRL_digest(
        x509, EVP_sha1(), reinterpret_cast<unsigned char*>(info->thumbprint.data()), nullptr);

    auto notBeforeMillis = convertASN1ToMillis(X509_CRL_get_lastUpdate(x509));

    uassert(4913008, "date conversion failed", notBeforeMillis != Date_t());

    info->validityNotBefore = notBeforeMillis;

    auto notAfterMillis = convertASN1ToMillis(X509_CRL_get_nextUpdate(x509));

    uassert(4913009, "date conversion failed", notAfterMillis != Date_t());

    info->validityNotAfter = notAfterMillis;
}


SSLInformationToLog SSLManagerOpenSSL::getSSLInformationToLog() const {
    SSLInformationToLog info;
    if (!(sslGlobalParams.sslPEMKeyFile.empty())) {
        UniqueX509 serverX509Cert =
            _getX509Object(sslGlobalParams.sslPEMKeyFile, &_serverPEMPassword);
        _getX509CertInfo(
            serverX509Cert, &info.server, StringData{sslGlobalParams.sslPEMKeyFile}, boost::none);
    }

    if (!(sslGlobalParams.sslClusterFile.empty())) {
        CertInformationToLog clusterInfo;
        UniqueX509 clusterX509Cert =
            _getX509Object(sslGlobalParams.sslClusterFile, &_clusterPEMPassword);
        _getX509CertInfo(
            clusterX509Cert, &clusterInfo, StringData{sslGlobalParams.sslClusterFile}, boost::none);
        info.cluster = clusterInfo;
    } else {
        info.cluster = boost::none;
    }

    if (!sslGlobalParams.sslCRLFile.empty()) {
        CRLInformationToLog crlInfo;
        _getCRLInfo(getSSLGlobalParams().sslCRLFile, &crlInfo);
        info.crl = crlInfo;
    } else {
        info.crl = boost::none;
    }

    return info;
}

}  // namespace mongo
