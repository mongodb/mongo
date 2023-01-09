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

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/crypto/jws_validator.h"
#include "mongo/crypto/jwt_types_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/ssl_manager.h"

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>

#if OPENSSL_VERSION_NUMBER < 0x10100000L || \
    (defined(LIBRESSL_VERSION_NUMBER) && LIBRESSL_VERSION_NUMBER < 0x2070000fL)
namespace {
// Copies of OpenSSL 1.1.0 and later define new EVP digest routines. We must
// polyfill used definitions to interact with older OpenSSL versions.
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

}  // namespace
#endif

namespace mongo::crypto {
namespace {

using UniqueRSA = std::unique_ptr<RSA, OpenSSLDeleter<decltype(RSA_free), RSA_free>>;
using UniqueEVPPKey =
    std::unique_ptr<EVP_PKEY, OpenSSLDeleter<decltype(EVP_PKEY_free), EVP_PKEY_free>>;
using UniqueEVPMDCtx =
    std::unique_ptr<EVP_MD_CTX, OpenSSLDeleter<decltype(EVP_MD_CTX_free), ::EVP_MD_CTX_free>>;
using UniqueBIGNUM = std::unique_ptr<BIGNUM, OpenSSLDeleter<decltype(BN_free), BN_free>>;

class JWSValidatorOpenSSLRSA : public JWSValidator {
public:
    JWSValidatorOpenSSLRSA(StringData algorithm, const BSONObj& key) : _key(EVP_PKEY_new()) {
        uassert(7095402, "Unknown hashing algorithm", algorithm == "RSA");

        auto RSAKey = JWKRSA::parse(IDLParserContext("JWKRSA"), key);

        const auto* pubKeyNData =
            reinterpret_cast<const unsigned char*>(RSAKey.getModulus().rawData());
        UniqueBIGNUM n(BN_bin2bn(pubKeyNData, RSAKey.getModulus().size(), nullptr));
        uassertOpenSSL("Failed creating modulus", n.get() != nullptr);

        const auto* pubKeyEData =
            reinterpret_cast<const unsigned char*>(RSAKey.getPublicExponent().rawData());
        UniqueBIGNUM e(BN_bin2bn(pubKeyEData, RSAKey.getPublicExponent().size(), nullptr));
        uassertOpenSSL("Failed creating exponent", e.get() != nullptr);

        UniqueRSA rsa(RSA_new());
        uassertOpenSSL("Failed creating RSAKey", rsa.get() != nullptr);
        uassertOpenSSL("RSA key setup failed",
                       RSA_set0_key(rsa.get(), n.get(), e.get(), nullptr) == 1);
        n.release();  // Now owned by rsa
        e.release();  // Now owned by rsa

        uassertOpenSSL("Failed creating EVP_PKey", _key.get() != nullptr);
        uassertOpenSSL("EVP_PKEY assignment failed",
                       EVP_PKEY_assign_RSA(_key.get(), rsa.get()) == 1);
        rsa.release();  // Now owned by _key
    }

    Status validate(StringData algorithm, StringData payload, StringData signature) const final {
        const EVP_MD* alg = getHashingAlg(algorithm);
        uassert(7095403, str::stream() << "Unknown hashing algorithm: '" << algorithm << "'", alg);

        UniqueEVPMDCtx ctx(EVP_MD_CTX_new());
        uassertOpenSSL("DigestVerifyInit failed",
                       EVP_DigestVerifyInit(ctx.get(), nullptr, alg, nullptr, _key.get()) == 1);
        uassertOpenSSL(
            "DigestVerifyUpdate failed",
            EVP_DigestVerifyUpdate(ctx.get(),
                                   reinterpret_cast<const unsigned char*>(payload.rawData()),
                                   payload.size()) == 1);

        int verifyRes = EVP_DigestVerifyFinal(
            ctx.get(),
            const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(signature.rawData())),
            signature.size());
        if (verifyRes == 0) {
            return {ErrorCodes::InvalidSignature, "OpenSSL: Signature is invalid"};
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
    static const EVP_MD* getHashingAlg(StringData alg) {
        if (alg == kRS256) {
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

    static void uassertOpenSSL(StringData context, bool success) {
        uassert(ErrorCodes::OperationFailed,
                str::stream() << context << ": "
                              << SSLManagerInterface::getSSLErrorMessage(ERR_get_error()),
                success);
    }

    UniqueEVPPKey _key;
};
}  // namespace

StatusWith<std::unique_ptr<JWSValidator>> JWSValidator::create(StringData algorithm,
                                                               const BSONObj& key) try {
    return std::make_unique<JWSValidatorOpenSSLRSA>(algorithm, key);
} catch (const DBException& e) {
    return e.toStatus();
}
}  // namespace mongo::crypto
