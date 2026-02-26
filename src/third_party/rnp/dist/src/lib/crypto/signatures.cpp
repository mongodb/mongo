/*
 * Copyright (c) 2018-2022, [Ribose Inc](https://www.ribose.com).
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <string.h>
#include <cassert>
#include "crypto/signatures.h"
#include "librepgp/stream-packet.h"
#include "librepgp/stream-sig.h"
#include "librepgp/stream-key.h"
#include "utils.h"
#include "sec_profile.hpp"

/**
 * @brief Add signature fields to the hash context and finish it.
 * @param hash initialized hash context fed with signed data (document, key, etc).
 *             It is finalized in this function.
 * @param sig populated or loaded signature
 * @param hbuf buffer to store the resulting hash. Must be large enough for hash output.
 * @param hlen on success will be filled with the hash size, otherwise zeroed
 * @param hdr literal packet header for attached signatures or NULL otherwise.
 * @return RNP_SUCCESS on success or some error otherwise
 */
static rnp::secure_bytes
signature_hash_finish(const pgp::pkt::Signature &sig,
                      rnp::Hash &                hash,
                      const pgp_literal_hdr_t *  hdr)
{
    hash.add(sig.hashed_data);
    switch (sig.version) {
    case PGP_V4:
#if defined(ENABLE_CRYPTO_REFRESH)
    case PGP_V6:
#endif
    {
        uint8_t trailer[6] = {0x00, 0xff, 0x00, 0x00, 0x00, 0x00};
        trailer[0] = sig.version;
        write_uint32(&trailer[2], sig.hashed_data.size());
        hash.add(trailer, 6);
        break;
    }
    case PGP_V5: {
        uint64_t hash_len = sig.hashed_data.size();
        if (sig.is_document()) {
            uint8_t doc_trailer[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
            /* This data is not added to the hash_len as per spec */
            if (hdr) {
                doc_trailer[0] = hdr->format;
                doc_trailer[1] = hdr->fname_len;
                write_uint32(&doc_trailer[2], hdr->timestamp);
                hash.add(doc_trailer, 2);
                hash.add(hdr->fname, hdr->fname_len);
                hash.add(&doc_trailer[2], 4);
            } else {
                hash.add(doc_trailer, 6);
            }
        }
        uint8_t trailer[10] = {0x05, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        write_uint64(&trailer[2], hash_len);
        hash.add(trailer, 10);
        break;
    }
    default:
        break;
    }
    rnp::secure_bytes res(hash.size());
    hash.finish(res.data());
    return res;
}

std::unique_ptr<rnp::Hash>
signature_init(const pgp_key_pkt_t &key, const pgp::pkt::Signature &sig)
{
    auto hash = rnp::Hash::create(sig.halg);

#if defined(ENABLE_CRYPTO_REFRESH)
    if (key.version == PGP_V6) {
        hash->add(sig.salt);
    }
#endif

    if (key.material->alg() == PGP_PKA_SM2) {
        auto &sm2 = dynamic_cast<pgp::SM2KeyMaterial &>(*key.material);
        sm2.compute_za(*hash);
    }
    return hash;
}

void
signature_calculate(pgp::pkt::Signature &    sig,
                    pgp::KeyMaterial &       seckey,
                    rnp::Hash &              hash,
                    rnp::SecurityContext &   ctx,
                    const pgp_literal_hdr_t *hdr)
{
    /* Finalize hash first, since function is required to do this */
    auto hval = signature_hash_finish(sig, hash, hdr);
    if (!seckey.secret()) {
        RNP_LOG("Secret key is required.");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
    if (sig.palg != seckey.alg()) {
        RNP_LOG("Signature and secret key do not agree on algorithm type.");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
    /* Validate key material if didn't before */
    seckey.validate(ctx, false);
    if (!seckey.valid()) {
        RNP_LOG("Attempt to sign with invalid key material.");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }

    /* Copy left 16 bits to signature */
    std::copy(hval.begin(), hval.begin() + 2, sig.lbits.begin());

    /* Some algos require used hash algorithm for signing */
    auto material = pgp::SigMaterial::create(sig.palg, sig.halg);
    if (!material) {
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
    /* Sign */
    auto ret = seckey.sign(ctx, *material, hval);
    if (ret) {
        throw rnp::rnp_exception(ret);
    }
    sig.write_material(*material);
}

rnp::SigValidity
signature_validate(const pgp::pkt::Signature & sig,
                   const pgp::KeyMaterial &    key,
                   rnp::Hash &                 hash,
                   const rnp::SecurityContext &ctx,
                   const pgp_literal_hdr_t *   hdr)
{
    rnp::SigValidity res;
    if (sig.palg != key.alg()) {
        RNP_LOG(
          "Signature and key do not agree on algorithm type: %d vs %d", sig.palg, key.alg());
        res.add_error(RNP_ERROR_SIG_PUB_ALG_MISMATCH);
    }

    /* Check signature security */
    auto action =
      sig.is_document() ? rnp::SecurityAction::VerifyData : rnp::SecurityAction::VerifyKey;
    if (ctx.profile.hash_level(sig.halg, sig.creation(), action) <
        rnp::SecurityLevel::Default) {
        RNP_LOG("Insecure hash algorithm %d, marking signature as invalid.", sig.halg);
        res.add_error(RNP_ERROR_SIG_WEAK_HASH);
    }

#if defined(ENABLE_PQC)
    /* check that hash matches key requirements */
    if (!key.sig_hash_allowed(hash.alg())) {
        RNP_LOG("Signature invalid since hash algorithm requirements are not met for the "
                "given key.");
        res.add_error(RNP_ERROR_SIG_HASH_ALG_MISMATCH);
    }
#endif

    /* Finalize hash */
    auto hval = signature_hash_finish(sig, hash, hdr);

    /* compare lbits */
    if (memcmp(hval.data(), sig.lbits.data(), 2)) {
        RNP_LOG("wrong lbits");
        res.add_error(RNP_ERROR_SIG_LBITS_MISMATCH);
    }

    /* validate signature */
    /* We check whether material could be parsed during the signature parsing */
    auto material = sig.parse_material();
    assert(material);
    auto ret = key.verify(ctx, *material, hval);
    if (ret) {
        res.add_error(ret);
    }
    return res;
}
