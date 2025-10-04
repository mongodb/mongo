/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/crypto/jws_validator.h"
#include "mongo/crypto/jwt_types_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/str.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/opensslv.h>
#include <openssl/ossl_typ.h>
#include <openssl/rsa.h>
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/core_names.h>
#include <openssl/params.h>
#endif

#if OPENSSL_VERSION_NUMBER < 0x10100000L || \
    (defined(LIBRESSL_VERSION_NUMBER) && LIBRESSL_VERSION_NUMBER < 0x2070000fL)
namespace {
// Copies of OpenSSL 1.1.0 and later define new EVP digest routines. We must
// polyfill used definitions to interact with older OpenSSL versions.
#define RSA_PSS_SALTLEN_DIGEST -1

EVP_MD_CTX* EVP_MD_CTX_new() {
    return EVP_MD_CTX_create();
}

void EVP_MD_CTX_free(EVP_MD_CTX* ctx) {
    EVP_MD_CTX_destroy(ctx);
}

int RSA_set0_key(RSA* r, BIGNUM* n, BIGNUM* e, BIGNUM* d) {
    /* If the fields n and e in r are NULL, the corresponding input
     * parameters MUST be non-NULL for n and e.  d may be
     * left NULL (in case only the public key is used).
     */
    if ((r->n == NULL && n == NULL) || (r->e == NULL && e == NULL))
        return 0;

    if (n != NULL) {
        BN_free(r->n);
        r->n = n;
    }
    if (e != NULL) {
        BN_free(r->e);
        r->e = e;
    }
    if (d != NULL) {
        BN_free(r->d);
        r->d = d;
    }

    return 1;
}

int ECDSA_SIG_set0(ECDSA_SIG* sig, BIGNUM* r, BIGNUM* s) {
    if (r == NULL || s == NULL)
        return 0;
    BN_clear_free(sig->r);
    BN_clear_free(sig->s);
    sig->r = r;
    sig->s = s;
    return 1;
}
}  // namespace
#endif

namespace mongo::crypto {
namespace {

static void uassertOpenSSL(StringData context, bool success) {
    uassert(ErrorCodes::OperationFailed,
            str::stream() << context << ": "
                          << SSLManagerInterface::getSSLErrorMessage(ERR_get_error()),
            success);
}

using UniqueRSA = std::unique_ptr<RSA, OpenSSLDeleter<decltype(RSA_free), RSA_free>>;

// Used for RSA and EC keys. We use EVP_PKEY_free as a higher-level API to EC_free
using UniqueEVPPKey =
    std::unique_ptr<EVP_PKEY, OpenSSLDeleter<decltype(EVP_PKEY_free), EVP_PKEY_free>>;

using UniqueECKey = std::unique_ptr<EC_KEY, OpenSSLDeleter<decltype(EC_KEY_free), EC_KEY_free>>;

// Used for EC keys to obtain the public key from the EC curve and X and Y coordinates
using UniqueEVPPKeyCtx =
    std::unique_ptr<EVP_PKEY_CTX, OpenSSLDeleter<decltype(EVP_PKEY_CTX_free), EVP_PKEY_CTX_free>>;

// Used for constructing the EC signature from the R and S components
using UniqueECDSASIG =
    std::unique_ptr<ECDSA_SIG, OpenSSLDeleter<decltype(ECDSA_SIG_free), ECDSA_SIG_free>>;

using UniqueEVPMDCtx =
    std::unique_ptr<EVP_MD_CTX, OpenSSLDeleter<decltype(EVP_MD_CTX_free), ::EVP_MD_CTX_free>>;
using UniqueBIGNUM = std::unique_ptr<BIGNUM, OpenSSLDeleter<decltype(BN_free), BN_free>>;

class JWSValidatorOpenSSLRSA : public JWSValidator {
public:
    JWSValidatorOpenSSLRSA(StringData algorithm, const BSONObj& key) : _key(EVP_PKEY_new()) {
        uassert(7095402, "Unknown hashing algorithm", algorithm == "RSA");

        auto RSAKey = JWKRSA::parse(key, IDLParserContext("JWKRSA"));

        const auto* pubKeyNData =
            reinterpret_cast<const unsigned char*>(RSAKey.getModulus().data());
        UniqueBIGNUM n(BN_bin2bn(pubKeyNData, RSAKey.getModulus().size(), nullptr));
        uassertOpenSSL("Failed creating modulus", n.get() != nullptr);

        const auto* pubKeyEData =
            reinterpret_cast<const unsigned char*>(RSAKey.getPublicExponent().data());
        UniqueBIGNUM e(BN_bin2bn(pubKeyEData, RSAKey.getPublicExponent().size(), nullptr));
        uassertOpenSSL("Failed creating exponent", e.get() != nullptr);

        UniqueRSA rsa(RSA_new());
        uassertOpenSSL("Failed creating RSAKey", rsa.get() != nullptr);
        uassertOpenSSL("RSA key setup failed",
                       RSA_set0_key(rsa.get(), n.get(), e.get(), nullptr) == 1);
        (void)n.release();  // Now owned by rsa, cast to void to explicitly ignore the return value.
        (void)e.release();  // Now owned by rsa, cast to void to explicitly ignore the return value.

        uassertOpenSSL("Failed creating EVP_PKey", _key.get() != nullptr);
        uassertOpenSSL("EVP_PKEY assignment failed",
                       EVP_PKEY_assign_RSA(_key.get(), rsa.get()) == 1);
        (void)rsa
            .release();  // Now owned by _key, cast to void to explicitly ignore the return value.
    }

    Status validate(StringData algorithm, StringData payload, StringData signature) const final {
        const EVP_MD* alg = getHashingAlg(algorithm);
        uassert(7095403, str::stream() << "Unknown hashing algorithm: '" << algorithm << "'", alg);

        UniqueEVPMDCtx ctx(EVP_MD_CTX_new());

        EVP_PKEY_CTX* pctx = nullptr;

        uassertOpenSSL("DigestVerifyInit failed",
                       EVP_DigestVerifyInit(ctx.get(), &pctx, alg, nullptr, _key.get()) == 1);

        if (algorithm == kPS256) {
            uassertOpenSSL("EVP_PKEY_CTX_set_rsa_padding failed",
                           EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) == 1);
            uassertOpenSSL("EVP_PKEY_CTX_set_rsa_mgf1_md failed",
                           EVP_PKEY_CTX_set_rsa_mgf1_md(pctx, alg) == 1);
            uassertOpenSSL("EVP_PKEY_CTX_set_rsa_pss_saltlen",
                           EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST) == 1);
        }

        uassertOpenSSL(
            "DigestVerifyUpdate failed",
            EVP_DigestVerifyUpdate(ctx.get(),
                                   reinterpret_cast<const unsigned char*>(payload.data()),
                                   payload.size()) == 1);

        int verifyRes = EVP_DigestVerifyFinal(
            ctx.get(),
            const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(signature.data())),
            signature.size());
        if (verifyRes == 0) {
            return {ErrorCodes::InvalidSignature, "OpenSSL: RSA Signature is invalid"};
        } else if (verifyRes != 1) {
            return {ErrorCodes::UnknownError,
                    SSLManagerInterface::getSSLErrorMessage(ERR_get_error())};
        }
        return Status::OK();
    }

private:
    static constexpr auto kRS256 = "RS256"_sd;
    static constexpr auto kRS384 = "RS384"_sd;
    static constexpr auto kRS512 = "RS512"_sd;
    static constexpr auto kPS256 = "PS256"_sd;

    static const EVP_MD* getHashingAlg(StringData alg) {
        if (alg == kRS256 || alg == kPS256) {
            return EVP_sha256();
        }
        if (alg == kRS384) {
            return EVP_sha384();
        }
        if (alg == kRS512) {
            return EVP_sha512();
        }
        return nullptr;
    }

    UniqueEVPPKey _key;
};

class JWSValidatorOpenSSLEC : public JWSValidator {
public:
    JWSValidatorOpenSSLEC(StringData algorithm, const BSONObj& key) : _key(EVP_PKEY_new()) {
        uassert(10858400, "Unknown EC hashing algorithm", algorithm == "EC");

        auto ECKey = JWKEC::parse(key, IDLParserContext("JWKEC"));

        // The X-coordinate of the private key
        const auto* xCoordinateData =
            reinterpret_cast<const unsigned char*>(ECKey.getXcoordinate().data());
        UniqueBIGNUM x(BN_bin2bn(xCoordinateData, ECKey.getXcoordinate().size(), nullptr));
        uassertOpenSSL("Failed creating X coordinate for EC curve", x.get() != nullptr);

        // The Y-coordinate of the private key
        const auto* yCoordinateData =
            reinterpret_cast<const unsigned char*>(ECKey.getYcoordinate().data());
        UniqueBIGNUM y(BN_bin2bn(yCoordinateData, ECKey.getYcoordinate().size(), nullptr));
        uassertOpenSSL("Failed creating y coordinate for EC curve", y.get() != nullptr);

        // Obtain the public key from the X and Y coordinates
        // SEC1 uncompressed public key format: 0x04||Qx||Qy
        size_t x_field_len = ECKey.getXcoordinate().size();
        size_t y_field_len = ECKey.getYcoordinate().size();

        // Ensure the size of the X and Y coordinates are the same
        uassertOpenSSL("Invalid sizes for EC X and Y coordinates", x_field_len == y_field_len);

        // Obtain the public key from the X and Y coordinates in EVP_PKEY format so that we can use
        // it for signature validation in EVP_DigestVerifyFinal
        EVP_PKEY* pkey = createECKeyFromCurveAndCoordinates(
            ECKey.getCurve().data(), ECKey.getXcoordinate(), ECKey.getYcoordinate());
        uassertOpenSSL("Failed to create EC EVP_PKEY from user data", pkey != nullptr);
        _key.reset(pkey);
        uassertOpenSSL("Failed to create EC EVP_PKEY from user data", _key != nullptr);
    }

    Status validate(StringData algorithm, StringData payload, StringData signature) const final {

        std::vector<unsigned char> der_sig = DEREncodeECSignature(signature);
        uassertOpenSSL("Failed to DER encode EC signature", der_sig.size() > 0);

        const EVP_MD* alg = getHashingAlg(algorithm);
        if (!alg) {
            return {ErrorCodes::UnsupportedFormat, "Unsupported EC algorithm"};
        }

        UniqueEVPMDCtx ctx(EVP_MD_CTX_new());
        EVP_PKEY_CTX* pctx = nullptr;

        uassertOpenSSL("DigestVerifyInit failed",
                       EVP_DigestVerifyInit(ctx.get(), &pctx, alg, nullptr, _key.get()) == 1);
        uassertOpenSSL(
            "DigestVerifyUpdate failed",
            EVP_DigestVerifyUpdate(ctx.get(),
                                   reinterpret_cast<const unsigned char*>(payload.data()),
                                   payload.size()) == 1);
        int verifyRes = EVP_DigestVerifyFinal(
            ctx.get(),
            const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(der_sig.data())),
            der_sig.size());
        if (verifyRes <= 0) {
            return {ErrorCodes::InvalidSignature, "OpenSSL: EC Signature is invalid"};
        } else if (verifyRes != 1) {
            return {ErrorCodes::UnknownError,
                    SSLManagerInterface::getSSLErrorMessage(ERR_get_error())};
        }
        return Status::OK();
    }

private:
    static constexpr auto kES256 = "ES256"_sd;
    static constexpr auto kES384 = "ES384"_sd;

    // Depending on the format of the JWT, the "alg" parameter can either be the algorithm (e.g.
    // ES256) or curve (e.g P-256). Not all JWT tokens have both values, so we accept either one
    // interchangeably
    static const EVP_MD* getHashingAlg(StringData alg) {
        if ((alg == kES256) || (alg == "P-256")) {
            return EVP_sha256();
        }
        if ((alg == kES384) || (alg == "P-384")) {
            return EVP_sha384();
        }
        return nullptr;
    }

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    // Obtain the Elliptical curve name given the algorithm
    // - ES256 uses P-256, also known as prime256v1
    // - ES384 uses P-384, also known as secp384r1
    static const char* getECCurveName(const std::string& alg) {
        if ((alg == kES256) || (alg == "P-256")) {
            // Uses P-256 elliptic curve
            return SN_X9_62_prime256v1;
        }
        if ((alg == kES384) || (alg == "P-384")) {
            // Uses P-384 elliptic curve (also known as secp384r1)
            return SN_secp384r1;
        }
        // Only the P-256 and P-384 EC curves are supported
        return nullptr;
    }
#else

    // Used for pre-OpenSSL 3.0 compatibility
    // Returns the NID for the given curve name
    // Returns 0 if the curve name is not recognized
    static int getNIDfromAlgorithm(const std::string& alg) {
        if ((alg == kES256) || (alg == "P-256")) {
            return NID_X9_62_prime256v1;
        }
        if ((alg == kES384) || (alg == "P-384")) {
            return NID_secp384r1;
        }
        return 0;
    }
#endif

    // Creates an EC public key from the curve name and the X and Y coordinates
    static EVP_PKEY* createECKeyFromCurveAndCoordinates(const std::string& curve,
                                                        StringData xCoordinate,
                                                        StringData yCoordinate) {
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        EVP_PKEY* pkey = nullptr;
        const auto* xCoordinateData = reinterpret_cast<const unsigned char*>(xCoordinate.data());
        UniqueBIGNUM x(BN_bin2bn(xCoordinateData, xCoordinate.size(), nullptr));
        uassertOpenSSL("Failed creating X coordinate for EC curve", x.get() != nullptr);

        // The Y-coordinate of the private key
        const auto* yCoordinateData = reinterpret_cast<const unsigned char*>(yCoordinate.data());
        UniqueBIGNUM y(BN_bin2bn(yCoordinateData, yCoordinate.size(), nullptr));
        uassertOpenSSL("Failed creating y coordinate for EC curve", y.get() != nullptr);

        // Obtain the public key from the X and Y coordinates
        // SEC1 uncompressed public key format: 0x04||Qx||Qy
        size_t x_field_len = xCoordinate.size();
        size_t y_field_len = yCoordinate.size();

        // Ensure the size of the X and Y coordinates are the same
        uassertOpenSSL("Invalid sizes for EC X and Y coordinates", x_field_len == y_field_len);

        const char* ec_group_name = getECCurveName(curve);
        uassertOpenSSL("Invalid EC group name", ec_group_name != nullptr);

        // Construct the public key
        std::vector<unsigned char> pub_key(1 + 2 * x_field_len);
        pub_key[0] = 0x04;
        std::memcpy(pub_key.data() + 1, xCoordinateData, x_field_len);
        std::memcpy(pub_key.data() + 1 + x_field_len, yCoordinateData, y_field_len);

        OSSL_PARAM params[3];
        params[0] = OSSL_PARAM_construct_utf8_string(
            OSSL_PKEY_PARAM_GROUP_NAME, const_cast<char*>(ec_group_name), 0);
        params[1] = OSSL_PARAM_construct_octet_string(
            OSSL_PKEY_PARAM_PUB_KEY, pub_key.data(), pub_key.size());
        params[2] = OSSL_PARAM_construct_end();

        UniqueEVPPKeyCtx gen_ctx(EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL));
        uassertOpenSSL("Failed to initialize public key algorithm context for EC Key",
                       gen_ctx.get() != nullptr);
        uassertOpenSSL("Failed to initialize EC public key from user data",
                       EVP_PKEY_fromdata_init(gen_ctx.get()) > 0);
        uassertOpenSSL("Failed to create EC EVP_PKEY from user data",
                       EVP_PKEY_fromdata(gen_ctx.get(), &pkey, EVP_PKEY_PUBLIC_KEY, params) > 0);
        uassertOpenSSL("Failed to create EC EVP_PKEY from user data", pkey != nullptr);
        return pkey;
#else

        // Used for pre-OpenSSL 3.0 compatibility
        int nid = getNIDfromAlgorithm(curve);
        uassertOpenSSL("Unknown EC curve", nid != 0);

        UniqueECKey eckey(EC_KEY_new_by_curve_name(nid));
        uassertOpenSSL("Failed to create EC Key", eckey != nullptr);

        UniqueBIGNUM bnQx(BN_bin2bn(
            reinterpret_cast<const unsigned char*>(xCoordinate.data()), xCoordinate.size(), NULL));
        UniqueBIGNUM bnQy(BN_bin2bn(
            reinterpret_cast<const unsigned char*>(yCoordinate.data()), yCoordinate.size(), NULL));

        uassertOpenSSL(
            "Failed to set EC public key X and Y coordinates",
            EC_KEY_set_public_key_affine_coordinates(eckey.get(), bnQx.get(), bnQy.get()) == 1);

        EVP_PKEY* pkey = EVP_PKEY_new();
        uassertOpenSSL("Failed to create EVP_PKEY", pkey != nullptr);
        uassertOpenSSL("Failed to assign EC key to EVP_PKEY",
                       EVP_PKEY_assign_EC_KEY(pkey, eckey.get()));

        (void)eckey.release();
        return pkey;
#endif
    }

    // Converts the EC signature from the R and S components to DER format
    // The signature consists of the combined R and S components, and the R and S is the
    // same size. So, the length of the signature must be an even number.
    // Split the signature to get back R and S
    // The DER format is a binary format that is used to represent the ECDSA signature
    // Returns a Status indicating success or failure
    // If the signature is invalid, returns an error status
    // If the signature is valid, returns a Status::OK()
    // The DER format is a binary format that is used to represent the ECDSA signature
    static std::vector<unsigned char> DEREncodeECSignature(StringData signature) {
        // The signature consists of the combined R and S components, and the R and S is the
        // same size. So, the length of the signature must be an even number. Split the
        // signature to get back R and S
        uassert(10858403, "OpenSSL: EC Signature length is invalid", (signature.size() % 2) == 0);

        int half = signature.size() / 2;
        std::string bin_sig{signature};
        std::string binR = bin_sig.substr(0, half);  // first half
        std::string binS = bin_sig.substr(half);     // second half
        UniqueBIGNUM bigR(
            BN_bin2bn(reinterpret_cast<const unsigned char*>(binR.data()), binR.size(), nullptr));
        uassertOpenSSL("Failed creating R from signature", bigR.get() != nullptr);
        UniqueBIGNUM bigS(
            BN_bin2bn(reinterpret_cast<const unsigned char*>(binS.data()), binS.size(), nullptr));
        uassertOpenSSL("Failed creating S from signature", bigS.get() != nullptr);

        // Create the ECDSA_SIG structure from the R and S components
        UniqueECDSASIG ecdsaSig(ECDSA_SIG_new());
        uassertOpenSSL("Failed creating ECDSA_SIG", ecdsaSig.get() != nullptr);
        ECDSA_SIG_set0(ecdsaSig.get(), bigR.get(), bigS.get());
        (void)bigR.release();  // Now owned by ecdsaSig, cast
        (void)bigS.release();  // Now owned by ecdsaSig, cast to void to explicitly ignore the
                               // return value.

        // Get the length of the DER signature we need to allocate space for
        int der_len = i2d_ECDSA_SIG(ecdsaSig.get(), nullptr);
        uassert(10858404, "Failed to get DER length for ECDSA_SIG", der_len > 0);

        // Allocate space for the DER signature and convert the ECDSA_SIG to DER format
        // The DER format is a binary format that is used to represent the ECDSA signature
        std::vector<unsigned char> der_sig(der_len);
        unsigned char* der_ptr = der_sig.data();
        der_len = i2d_ECDSA_SIG(ecdsaSig.get(), &der_ptr);
        uassert(10858405, "Failed to convert ECDSA_SIG to DER", der_len > 0);
        return der_sig;
    }

    UniqueEVPPKey _key;
};
}  // namespace

StatusWith<std::unique_ptr<JWSValidator>> JWSValidator::create(StringData algorithm,
                                                               const BSONObj& key) try {
    if (algorithm == "RSA"_sd) {
        return std::make_unique<JWSValidatorOpenSSLRSA>(algorithm, key);
    } else if (algorithm == "EC"_sd) {
        return std::make_unique<JWSValidatorOpenSSLEC>(algorithm, key);
    }
    uasserted(ErrorCodes::UnsupportedFormat,
              "Unknown signature algorithm" + std::string(algorithm.data()));
} catch (const DBException& e) {
    return e.toStatus();
}
}  // namespace mongo::crypto
