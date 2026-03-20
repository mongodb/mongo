/*
 * Copyright (c) 2017-2024, [Ribose Inc](https://www.ribose.com).
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

#include <memory>
#include <sstream>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <time.h>
#include "config.h"

#include <librepgp/stream-packet.h>
#include "key_store_g10.h"

#include "crypto/common.h"
#include "crypto/mem.h"
#include "crypto/cipher.hpp"
#include "key.hpp"
#include "time-utils.h"

#include "g23_sexp.hpp"
using namespace ext_key_format;
using namespace sexp;

#define G10_CBC_IV_SIZE 16

#define G10_OCB_NONCE_SIZE 12

#define G10_SHA1_HASH_SIZE 20

#define G10_PROTECTED_AT_SIZE 15

typedef struct format_info {
    pgp_symm_alg_t    cipher;
    pgp_cipher_mode_t cipher_mode;
    pgp_hash_alg_t    hash_alg;
    size_t            cipher_block_size;
    const char *      g10_type;
    size_t            iv_size;
    size_t            tag_length;
    bool              with_associated_data;
    bool              disable_padding;
} format_info;

static bool g10_calculated_hash(const pgp_key_pkt_t &key,
                                const char *         protected_at,
                                uint8_t *            checksum);

static const format_info formats[] = {{PGP_SA_AES_128,
                                       PGP_CIPHER_MODE_CBC,
                                       PGP_HASH_SHA1,
                                       16,
                                       "openpgp-s2k3-sha1-aes-cbc",
                                       G10_CBC_IV_SIZE,
                                       0,
                                       false,
                                       true},
                                      {PGP_SA_AES_256,
                                       PGP_CIPHER_MODE_CBC,
                                       PGP_HASH_SHA1,
                                       16,
                                       "openpgp-s2k3-sha1-aes256-cbc",
                                       G10_CBC_IV_SIZE,
                                       0,
                                       false,
                                       true},
                                      {PGP_SA_AES_128,
                                       PGP_CIPHER_MODE_OCB,
                                       PGP_HASH_SHA1,
                                       16,
                                       "openpgp-s2k3-ocb-aes",
                                       G10_OCB_NONCE_SIZE,
                                       16,
                                       true,
                                       true}};

static const id_str_pair g10_alg_aliases[] = {
  {PGP_PKA_RSA, "rsa"},
  {PGP_PKA_RSA, "openpgp-rsa"},
  {PGP_PKA_RSA, "oid.1.2.840.113549.1.1.1"},
  {PGP_PKA_RSA, "oid.1.2.840.113549.1.1.1"},
  {PGP_PKA_ELGAMAL, "elg"},
  {PGP_PKA_ELGAMAL, "elgamal"},
  {PGP_PKA_ELGAMAL, "openpgp-elg"},
  {PGP_PKA_ELGAMAL, "openpgp-elg-sig"},
  {PGP_PKA_DSA, "dsa"},
  {PGP_PKA_DSA, "openpgp-dsa"},
  {PGP_PKA_ECDSA, "ecc"},
  {PGP_PKA_ECDSA, "ecdsa"},
  {PGP_PKA_ECDH, "ecdh"},
  {PGP_PKA_EDDSA, "eddsa"},
  {0, NULL},
};

static const id_str_pair g10_curve_aliases[] = {
  {PGP_CURVE_NIST_P_256, "NIST P-256"},
  {PGP_CURVE_NIST_P_256, "1.2.840.10045.3.1.7"},
  {PGP_CURVE_NIST_P_256, "prime256v1"},
  {PGP_CURVE_NIST_P_256, "secp256r1"},
  {PGP_CURVE_NIST_P_256, "nistp256"},
  {PGP_CURVE_NIST_P_384, "NIST P-384"},
  {PGP_CURVE_NIST_P_384, "secp384r1"},
  {PGP_CURVE_NIST_P_384, "1.3.132.0.34"},
  {PGP_CURVE_NIST_P_384, "nistp384"},
  {PGP_CURVE_NIST_P_521, "NIST P-521"},
  {PGP_CURVE_NIST_P_521, "secp521r1"},
  {PGP_CURVE_NIST_P_521, "1.3.132.0.35"},
  {PGP_CURVE_NIST_P_521, "nistp521"},
  {PGP_CURVE_25519, "Curve25519"},
  {PGP_CURVE_25519, "1.3.6.1.4.1.3029.1.5.1"},
  {PGP_CURVE_ED25519, "Ed25519"},
  {PGP_CURVE_ED25519, "1.3.6.1.4.1.11591.15.1"},
  {PGP_CURVE_BP256, "brainpoolP256r1"},
  {PGP_CURVE_BP256, "1.3.36.3.3.2.8.1.1.7"},
  {PGP_CURVE_BP384, "brainpoolP384r1"},
  {PGP_CURVE_BP384, "1.3.36.3.3.2.8.1.1.11"},
  {PGP_CURVE_BP512, "brainpoolP512r1"},
  {PGP_CURVE_BP512, "1.3.36.3.3.2.8.1.1.13"},
  {PGP_CURVE_P256K1, "secp256k1"},
  {PGP_CURVE_P256K1, "1.3.132.0.10"},
  {0, NULL},
};

static const id_str_pair g10_curve_names[] = {
  {PGP_CURVE_NIST_P_256, "NIST P-256"},
  {PGP_CURVE_NIST_P_384, "NIST P-384"},
  {PGP_CURVE_NIST_P_521, "NIST P-521"},
  {PGP_CURVE_ED25519, "Ed25519"},
  {PGP_CURVE_25519, "Curve25519"},
  {PGP_CURVE_BP256, "brainpoolP256r1"},
  {PGP_CURVE_BP384, "brainpoolP384r1"},
  {PGP_CURVE_BP512, "brainpoolP512r1"},
  {PGP_CURVE_P256K1, "secp256k1"},
  {0, NULL},
};

static const format_info *
find_format(pgp_symm_alg_t cipher, pgp_cipher_mode_t mode, pgp_hash_alg_t hash_alg)
{
    for (size_t i = 0; i < ARRAY_SIZE(formats); i++) {
        if (formats[i].cipher == cipher && formats[i].cipher_mode == mode &&
            formats[i].hash_alg == hash_alg) {
            return &formats[i];
        }
    }
    return NULL;
}

static const format_info *
parse_format(const char *format, size_t format_len)
{
    for (size_t i = 0; i < ARRAY_SIZE(formats); i++) {
        if (strlen(formats[i].g10_type) == format_len &&
            !strncmp(formats[i].g10_type, format, format_len)) {
            return &formats[i];
        }
    }
    return NULL;
}

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

void
gnupg_sexp_t::add(unsigned u)
{
    char s[sizeof(STR(UINT_MAX)) + 1];
    snprintf(s, sizeof(s), "%u", u);
    push_back(std::make_shared<sexp_string_t>(s));
}

std::shared_ptr<gnupg_sexp_t>
gnupg_sexp_t::add_sub()
{
    auto res = std::make_shared<gnupg_sexp_t>();
    push_back(res);
    return res;
}

/*
 * Parse S-expression
 * https://people.csail.mit.edu/rivest/Sexp.txt
 * sexp library supports canonical and advanced transport formats
 * as well as base64 encoding of canonical
 */

bool
gnupg_sexp_t::parse(const char *r_bytes, size_t r_length, size_t depth)
{
    bool               res = false;
    std::istringstream iss(std::string(r_bytes, r_length));
    try {
        sexp_input_stream_t sis(&iss, depth);
        sexp_list_t::parse(sis.set_byte_size(8)->get_char());
        res = true;
    } catch (sexp_exception_t &e) {
        RNP_LOG("%s", e.what());
    }
    return res;
}

/*
 * Parse gnupg extended private key file ("G23")
 * https://github.com/gpg/gnupg/blob/main/agent/keyformat.txt
 */

bool
gnupg_extended_private_key_t::parse(const char *r_bytes, size_t r_length, size_t depth)
{
    bool               res = false;
    std::istringstream iss(std::string(r_bytes, r_length));
    try {
        ext_key_input_stream_t g23_is(&iss, depth);
        g23_is.scan(*this);
        res = true;
    } catch (sexp_exception_t &e) {
        RNP_LOG("%s", e.what());
    }
    return res;
}

static const sexp_list_t *
lookup_var(const sexp_list_t *list, const std::string &name) noexcept
{
    const sexp_list_t *res = nullptr;
    // We are looking for a list element  (condition 1)
    // that:
    //  -- has at least two SEXP elements (condition 2)
    //  -- has a SEXP string at 0 position (condition 3)
    //     matching given name            (condition 4)
    auto match = [name](const std::shared_ptr<sexp_object_t> &ptr) {
        bool r = false;
        auto r1 = ptr->sexp_list_view();
        if (r1 && r1->size() >= 2) { // conditions (1) and (2)
            auto r2 = r1->sexp_string_at(0);
            if (r2 && r2 == name) // conditions (3) and (4)
                r = true;
        }
        return r;
    };
    auto r3 = std::find_if(list->begin(), list->end(), std::move(match));
    if (r3 == list->end())
        RNP_LOG("Haven't got variable '%s'", name.c_str());
    else
        res = (*r3)->sexp_list_view();
    return res;
}

static const sexp_string_t *
lookup_var_data(const sexp_list_t *list, const std::string &name) noexcept
{
    const sexp_list_t *var = lookup_var(list, name);
    if (!var) {
        return NULL;
    }

    if (!var->at(1)->is_sexp_string()) {
        RNP_LOG("Expected block value");
        return NULL;
    }

    return var->sexp_string_at(1);
}

static bool
read_mpi(const sexp_list_t *list, const std::string &name, pgp::mpi &val) noexcept
{
    const sexp_string_t *data = lookup_var_data(list, name);
    if (!data) {
        return false;
    }

    /* strip leading zero */
    const auto &bytes = data->get_string();
    if ((bytes.size() > 1) && !bytes[0] && (bytes[1] & 0x80)) {
        val.assign(bytes.data() + 1, bytes.size() - 1);
    } else {
        val.assign(bytes.data(), bytes.size());
    }
    return true;
}

static bool
read_curve(const sexp_list_t *list, const std::string &name, pgp::ec::Key &key) noexcept
{
    const sexp_string_t *data = lookup_var_data(list, name);
    if (!data) {
        return false;
    }

    const auto &bytes = data->get_string();
    pgp_curve_t curve = static_cast<pgp_curve_t>(
      id_str_pair::lookup(g10_curve_aliases, (const char *) bytes.data(), PGP_CURVE_UNKNOWN));
    if (curve != PGP_CURVE_UNKNOWN) {
        key.curve = curve;
        return true;
    }
    RNP_LOG("Unknown curve: %.*s", (int) bytes.size(), (const char *) bytes.data());
    return false;
}

void
gnupg_sexp_t::add_mpi(const std::string &name, const pgp::mpi &mpi)
{
    auto sub_s_exp = add_sub();
    sub_s_exp->push_back(std::make_shared<sexp_string_t>(name));
    auto value_block = std::make_shared<sexp_string_t>();
    sub_s_exp->push_back(value_block);

    sexp_simple_string_t data;
    size_t               len = mpi.size();
    size_t               idx;

    for (idx = 0; (idx < len) && !mpi[idx]; idx++)
        ;

    if (idx < len) {
        if (mpi[idx] & 0x80) {
            data.append(0);
            data.std::basic_string<uint8_t>::append(mpi.data() + idx, len - idx);
        } else {
            data.assign(mpi.data() + idx, mpi.data() + len);
        }
        value_block->set_string(data);
    }
}

void
gnupg_sexp_t::add_curve(const std::string &name, pgp_curve_t kcurve)
{
    const char *curve = id_str_pair::lookup(g10_curve_names, kcurve, NULL);
    if (!curve) {
        RNP_LOG("unknown curve");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }

    auto psub_s_exp = add_sub();
    psub_s_exp->add(name);
    psub_s_exp->add(curve);

    if ((kcurve != PGP_CURVE_ED25519) && (kcurve != PGP_CURVE_25519)) {
        return;
    }

    psub_s_exp = add_sub();
    psub_s_exp->add("flags");
    psub_s_exp->add((kcurve == PGP_CURVE_ED25519) ? "eddsa" : "djb-tweak");
}

static bool
parse_pubkey(pgp_key_pkt_t &pubkey, const sexp_list_t *s_exp, pgp_pubkey_alg_t alg)
{
    pubkey.version = PGP_V4;
    pubkey.alg = alg;
    switch (alg) {
    case PGP_PKA_DSA: {
        pgp::dsa::Key dsa;
        if (!read_mpi(s_exp, "p", dsa.p) || !read_mpi(s_exp, "q", dsa.q) ||
            !read_mpi(s_exp, "g", dsa.g) || !read_mpi(s_exp, "y", dsa.y)) {
            return false;
        }
        pubkey.material = pgp::KeyMaterial::create(dsa);
        break;
    }
    case PGP_PKA_RSA:
    case PGP_PKA_RSA_ENCRYPT_ONLY:
    case PGP_PKA_RSA_SIGN_ONLY: {
        pgp::rsa::Key rsa;
        if (!read_mpi(s_exp, "n", rsa.n) || !read_mpi(s_exp, "e", rsa.e)) {
            return false;
        }
        pubkey.material = pgp::KeyMaterial::create(alg, rsa);
        break;
    }
    case PGP_PKA_ELGAMAL:
    case PGP_PKA_ELGAMAL_ENCRYPT_OR_SIGN: {
        pgp::eg::Key eg;
        if (!read_mpi(s_exp, "p", eg.p) || !read_mpi(s_exp, "g", eg.g) ||
            !read_mpi(s_exp, "y", eg.y)) {
            return false;
        }
        pubkey.material = pgp::KeyMaterial::create(alg, eg);
        break;
    }
    case PGP_PKA_ECDSA:
    case PGP_PKA_ECDH:
    case PGP_PKA_EDDSA: {
        pgp::ec::Key ec{};
        if (!read_curve(s_exp, "curve", ec) || !read_mpi(s_exp, "q", ec.p)) {
            return false;
        }
        if (ec.curve == PGP_CURVE_ED25519) {
            /* need to adjust it here since 'ecc' key type defaults to ECDSA */
            pubkey.alg = PGP_PKA_EDDSA;
        }
        pubkey.material = pgp::KeyMaterial::create(pubkey.alg, ec);
        break;
    }
    default:
        RNP_LOG("Unsupported public key algorithm: %d", (int) alg);
        return false;
    }

    return true;
}

static bool
parse_seckey(pgp_key_pkt_t &seckey, const sexp_list_t *s_exp)
{
    switch (seckey.alg) {
    case PGP_PKA_DSA: {
        pgp::mpi x;
        auto     key = dynamic_cast<pgp::DSAKeyMaterial *>(seckey.material.get());
        if (!key || !read_mpi(s_exp, "x", x)) {
            return false;
        }
        key->set_secret(x);
        x.forget();
        return true;
    }
    case PGP_PKA_RSA:
    case PGP_PKA_RSA_ENCRYPT_ONLY:
    case PGP_PKA_RSA_SIGN_ONLY: {
        pgp::mpi d, p, q, u;
        auto     key = dynamic_cast<pgp::RSAKeyMaterial *>(seckey.material.get());
        if (!key || !read_mpi(s_exp, "d", d) || !read_mpi(s_exp, "p", p) ||
            !read_mpi(s_exp, "q", q) || !read_mpi(s_exp, "u", u)) {
            return false;
        }
        key->set_secret(d, p, q, u);
        d.forget();
        p.forget();
        q.forget();
        u.forget();
        return true;
    }
    case PGP_PKA_ELGAMAL:
    case PGP_PKA_ELGAMAL_ENCRYPT_OR_SIGN: {
        pgp::mpi x;
        auto     key = dynamic_cast<pgp::EGKeyMaterial *>(seckey.material.get());
        if (!key || !read_mpi(s_exp, "x", x)) {
            return false;
        }
        key->set_secret(x);
        x.forget();
        return true;
    }
    case PGP_PKA_ECDSA:
    case PGP_PKA_ECDH:
    case PGP_PKA_EDDSA: {
        pgp::mpi x;
        auto     key = dynamic_cast<pgp::ECKeyMaterial *>(seckey.material.get());
        if (!key || !read_mpi(s_exp, "d", x)) {
            return false;
        }
        key->set_secret(x);
        x.forget();
        return true;
    }
    default:
        RNP_LOG("Unsupported public key algorithm: %d", (int) seckey.alg);
        return false;
    }
}

static bool
decrypt_protected_section(const sexp_simple_string_t &encrypted_data,
                          const pgp_key_pkt_t &       seckey,
                          const std::string &         password,
                          gnupg_sexp_t &              r_s_exp,
                          uint8_t *                   associated_data,
                          size_t                      associated_data_len)
{
    const format_info *     info = NULL;
    unsigned                keysize = 0;
    uint8_t                 derived_key[PGP_MAX_KEY_SIZE];
    uint8_t *               decrypted_data = NULL;
    size_t                  decrypted_data_len = 0;
    size_t                  output_written = 0;
    size_t                  input_consumed = 0;
    std::unique_ptr<Cipher> dec;
    bool                    ret = false;

    const char *decrypted_bytes;
    size_t      s_exp_len;

    // sanity checks
    const pgp_key_protection_t &prot = seckey.sec_protection;
    keysize = pgp_key_size(prot.symm_alg);
    if (!keysize) {
        RNP_LOG("parse_seckey: unknown symmetric algo");
        goto done;
    }
    // find the protection format in our table
    info = find_format(prot.symm_alg, prot.cipher_mode, prot.s2k.hash_alg);
    if (!info) {
        RNP_LOG("Unsupported format, alg: %d, cipher_mode: %d, hash: %d",
                prot.symm_alg,
                prot.cipher_mode,
                prot.s2k.hash_alg);
        goto done;
    }

    // derive the key
    if (pgp_s2k_iterated(prot.s2k.hash_alg,
                         derived_key,
                         keysize,
                         password.c_str(),
                         prot.s2k.salt,
                         prot.s2k.iterations)) {
        RNP_LOG("pgp_s2k_iterated failed");
        goto done;
    }

    // decrypt
    decrypted_data = (uint8_t *) malloc(encrypted_data.size());
    if (!decrypted_data) {
        /* LCOV_EXCL_START */
        RNP_LOG("can't allocate memory");
        goto done;
        /* LCOV_EXCL_END */
    }
    dec = Cipher::decryption(
      info->cipher, info->cipher_mode, info->tag_length, info->disable_padding);
    if (!dec || !dec->set_key(derived_key, keysize)) {
        goto done;
    }
    if (associated_data != nullptr && associated_data_len != 0) {
        if (!dec->set_ad(associated_data, associated_data_len)) {
            goto done;
        }
    }
    // Nonce shall be the last chunk of associated data
    if (!dec->set_iv(prot.iv, info->iv_size)) {
        goto done;
    }
    if (!dec->finish(decrypted_data,
                     encrypted_data.size(),
                     &output_written,
                     encrypted_data.data(),
                     encrypted_data.size(),
                     &input_consumed)) {
        goto done;
    }
    decrypted_data_len = output_written;
    s_exp_len = decrypted_data_len;
    decrypted_bytes = (const char *) decrypted_data;

    // parse and validate the decrypted s-exp

    if (!r_s_exp.parse(decrypted_bytes, s_exp_len, SXP_MAX_DEPTH)) {
        goto done;
    }
    if (!r_s_exp.size() || r_s_exp.at(0)->is_sexp_string()) {
        RNP_LOG("Hasn't got sub s-exp with key data.");
        goto done;
    }
    ret = true;
done:
    if (!ret) {
        r_s_exp.clear();
    }
    secure_clear(decrypted_data, decrypted_data_len);
    free(decrypted_data);
    return ret;
}

static bool
parse_protected_seckey(pgp_key_pkt_t &seckey, const sexp_list_t *list, const char *password)
{
    // find and validate the protected section
    const sexp_list_t *protected_key = lookup_var(list, "protected");
    if (!protected_key) {
        RNP_LOG("missing protected section");
        return false;
    }
    if (protected_key->size() != 4 || !protected_key->at(1)->is_sexp_string() ||
        protected_key->at(2)->is_sexp_string() || !protected_key->at(3)->is_sexp_string()) {
        RNP_LOG("Wrong protected format, expected: (protected mode (params) "
                "encrypted_octet_string)\n");
        return false;
    }

    // lookup the protection format
    auto &             fmt_bt = protected_key->sexp_string_at(1)->get_string();
    const format_info *format = parse_format((const char *) fmt_bt.data(), fmt_bt.size());
    if (!format) {
        RNP_LOG("Unsupported protected mode: '%.*s'\n",
                (int) fmt_bt.size(),
                (const char *) fmt_bt.data());
        return false;
    }

    // fill in some fields based on the lookup above
    pgp_key_protection_t &prot = seckey.sec_protection;
    prot.symm_alg = format->cipher;
    prot.cipher_mode = format->cipher_mode;
    prot.s2k.hash_alg = format->hash_alg;

    // locate and validate the protection parameters
    auto params = protected_key->sexp_list_at(2);
    if (params->size() != 2 || params->at(0)->is_sexp_string() ||
        !params->at(1)->is_sexp_string()) {
        RNP_LOG("Wrong params format, expected: ((hash salt no_of_iterations) iv)\n");
        return false;
    }

    // locate and validate the (hash salt no_of_iterations) exp
    auto alg = params->sexp_list_at(0);
    if (alg->size() != 3 || !alg->at(0)->is_sexp_string() || !alg->at(1)->is_sexp_string() ||
        !alg->at(2)->is_sexp_string()) {
        RNP_LOG("Wrong params sub-level format, expected: (hash salt no_of_iterations)\n");
        return false;
    }
    auto &hash_bt = alg->sexp_string_at(0)->get_string();
    if (hash_bt != "sha1") {
        RNP_LOG("Wrong hashing algorithm, should be sha1 but %.*s\n",
                (int) hash_bt.size(),
                (const char *) hash_bt.data());
        return false;
    }

    // fill in some constant values
    prot.s2k.hash_alg = PGP_HASH_SHA1;
    prot.s2k.usage = PGP_S2KU_ENCRYPTED_AND_HASHED;
    prot.s2k.specifier = PGP_S2KS_ITERATED_AND_SALTED;

    // check salt size
    auto &salt_bt = alg->sexp_string_at(1)->get_string();
    if (salt_bt.size() != PGP_SALT_SIZE) {
        RNP_LOG("Wrong salt size, should be %d but %d\n", PGP_SALT_SIZE, (int) salt_bt.size());
        return false;
    }

    // salt
    memcpy(prot.s2k.salt, salt_bt.data(), salt_bt.size());
    // s2k iterations
    auto iter = alg->sexp_string_at(2);
    prot.s2k.iterations = iter->as_unsigned();
    if (prot.s2k.iterations == UINT_MAX) {
        RNP_LOG("Wrong numbers of iteration, %.*s\n",
                (int) iter->get_string().size(),
                (const char *) iter->get_string().data());
        return false;
    }

    // iv
    auto &iv_bt = params->sexp_string_at(1)->get_string();
    if (iv_bt.size() != format->iv_size) {
        RNP_LOG("Wrong nonce size, should be %zu but %zu\n", format->iv_size, iv_bt.size());
        return false;
    }
    memcpy(prot.iv, iv_bt.data(), iv_bt.size());

    // we're all done if no password was provided (decryption not requested)
    if (!password) {
        seckey.material->clear_secret();
        return true;
    }

    // password was provided, so decrypt
    auto &       enc_bt = protected_key->sexp_string_at(3)->get_string();
    gnupg_sexp_t decrypted_s_exp;

    // Build associated data (AD) that is not included in the ciphertext but that should be
    // authenticated. gnupg builds AD as follows  (file 'protect.c' do_encryption/do_decryption
    // functions)
    //  -- "protected-private-key" section content
    //  -- less "protected" subsection
    //  -- serialized in canonical format
    std::string associated_data;
    if (format->with_associated_data) {
        std::ostringstream   oss(std::ios_base::binary);
        sexp_output_stream_t os(&oss);
        os.var_put_char('(');
        for_each(list->begin(), list->end(), [&](const std::shared_ptr<sexp_object_t> &obj) {
            if (obj->sexp_list_view() != protected_key)
                obj->print_canonical(&os);
        });
        os.var_put_char(')');
        associated_data = oss.str();
    }

    if (!decrypt_protected_section(
          enc_bt,
          seckey,
          password,
          decrypted_s_exp,
          format->with_associated_data ? (uint8_t *) associated_data.data() : nullptr,
          format->with_associated_data ? associated_data.length() : 0)) {
        return false;
    }
    // see if we have a protected-at section
    char protected_at[G10_PROTECTED_AT_SIZE] = {0};
    auto protected_at_data = lookup_var_data(list, "protected-at");
    if (protected_at_data) {
        if (protected_at_data->get_string().size() != G10_PROTECTED_AT_SIZE) {
            RNP_LOG("protected-at has wrong length: %zu, expected, %d\n",
                    protected_at_data->get_string().size(),
                    G10_PROTECTED_AT_SIZE);
            return false;
        }
        memcpy(protected_at,
               protected_at_data->get_string().data(),
               protected_at_data->get_string().size());
    }
    // parse MPIs
    if (!parse_seckey(seckey, decrypted_s_exp.sexp_list_at(0))) {
        RNP_LOG("failed to parse seckey");
        return false;
    }
    // check hash, if present
    if (decrypted_s_exp.size() > 1) {
        if (decrypted_s_exp.at(1)->is_sexp_string()) {
            RNP_LOG("Wrong hash block type.");
            return false;
        }
        auto sub_el = decrypted_s_exp.sexp_list_at(1);
        if (sub_el->size() < 3 || !sub_el->at(0)->is_sexp_string() ||
            !sub_el->at(1)->is_sexp_string() || !sub_el->at(2)->is_sexp_string()) {
            RNP_LOG("Wrong hash block structure.");
            return false;
        }

        auto &hkey = sub_el->sexp_string_at(0)->get_string();
        if (hkey != "hash") {
            RNP_LOG("Has got wrong hash block at encrypted key data.");
            return false;
        }
        auto &halg = sub_el->sexp_string_at(1)->get_string();
        if (halg != "sha1") {
            RNP_LOG("Supported only sha1 hash at encrypted private key.");
            return false;
        }
        uint8_t checkhash[G10_SHA1_HASH_SIZE];
        if (!g10_calculated_hash(seckey, protected_at, checkhash)) {
            RNP_LOG("failed to calculate hash");
            return false;
        }
        auto &hval = sub_el->sexp_string_at(2)->get_string();
        if (hval.size() != G10_SHA1_HASH_SIZE ||
            memcmp(checkhash, hval.data(), G10_SHA1_HASH_SIZE)) {
            RNP_LOG("Incorrect hash at encrypted private key.");
            return false;
        }
    }
    return true;
}

static bool
g23_parse_seckey(pgp_key_pkt_t &  seckey,
                 const uint8_t *  data,
                 size_t           data_len,
                 const char *     password,
                 pgp_pubkey_alg_t pubalg = PGP_PKA_NOTHING)
{
    gnupg_extended_private_key_t g23_extended_key;

    if (!g23_extended_key.parse((const char *) data, data_len, SXP_MAX_DEPTH)) {
        RNP_LOG("Failed to parse s-exp.");
        return false;
    }
    // Although the library parses full g23 extended key
    // we extract and use g10 part only
    const sexp_list_t &g10_key = g23_extended_key.key;

    /* expected format:
     *  (<type>
     *    (<algo>
     *	   (x <mpi>)
     *	   (y <mpi>)
     *    )
     *  )
     */

    if (g10_key.size() != 2 || !g10_key.at(0)->is_sexp_string() ||
        !g10_key.at(1)->is_sexp_list()) {
        RNP_LOG("Wrong format, expected: (<type> (...))");
        return false;
    }

    bool is_protected = false;

    auto &name = g10_key.sexp_string_at(0)->get_string();
    if (name == "private-key") {
        is_protected = false;
    } else if (name == "protected-private-key") {
        is_protected = true;
    } else {
        RNP_LOG("Unsupported top-level block: '%.*s'",
                (int) name.size(),
                (const char *) name.data());
        return false;
    }

    auto alg_s_exp = g10_key.sexp_list_at(1);
    if (alg_s_exp->size() < 2) {
        RNP_LOG("Wrong count of algorithm-level elements: %zu", alg_s_exp->size());
        return false;
    }

    if (!alg_s_exp->at(0)->is_sexp_string()) {
        RNP_LOG("Expected block with algorithm name, but has s-exp");
        return false;
    }

    bool ret = false;
    auto alg = pubalg;
    if (alg == PGP_PKA_NOTHING) {
        auto &alg_bt = alg_s_exp->sexp_string_at(0)->get_string();
        auto  alg_st = (const char *) alg_bt.data();
        alg = static_cast<pgp_pubkey_alg_t>(
          id_str_pair::lookup(g10_alg_aliases, alg_st, PGP_PKA_NOTHING));
        if (alg == PGP_PKA_NOTHING) {
            RNP_LOG("Unsupported algorithm: '%.*s'", (int) alg_bt.size(), alg_st);
            return false;
        }
        /* Parse pubkey only if it was not parsed before */
        if (!parse_pubkey(seckey, alg_s_exp, alg)) {
            RNP_LOG("failed to parse pubkey");
            goto done;
        }
    }

    if (is_protected) {
        if (!parse_protected_seckey(seckey, alg_s_exp, password)) {
            goto done;
        }
    } else {
        seckey.sec_protection.s2k.usage = PGP_S2KU_NONE;
        seckey.sec_protection.symm_alg = PGP_SA_PLAINTEXT;
        seckey.sec_protection.s2k.hash_alg = PGP_HASH_UNKNOWN;
        if (!parse_seckey(seckey, alg_s_exp)) {
            RNP_LOG("failed to parse seckey");
            goto done;
        }
    }
    ret = true;
done:
    if (!ret) {
        seckey = pgp_key_pkt_t();
    }
    return ret;
}

pgp_key_pkt_t *
g10_decrypt_seckey(const rnp::RawPacket &raw,
                   const pgp_key_pkt_t & pubkey,
                   const char *          password)
{
    if (!password) {
        return NULL;
    }
    auto seckey = std::unique_ptr<pgp_key_pkt_t>(new pgp_key_pkt_t(pubkey, false));
    if (!g23_parse_seckey(
          *seckey, raw.data().data(), raw.data().size(), password, pubkey.alg)) {
        return nullptr;
    }
    return seckey.release();
}

static bool
copy_secret_fields(pgp_key_pkt_t &dst, const pgp_key_pkt_t &src)
{
    switch (src.alg) {
    case PGP_PKA_DSA:
        if (src.material->secret()) {
            auto &dsasrc = dynamic_cast<pgp::DSAKeyMaterial &>(*src.material);
            auto &dsadst = dynamic_cast<pgp::DSAKeyMaterial &>(*dst.material);
            dsadst.set_secret(dsasrc.x());
        }
        break;
    case PGP_PKA_RSA:
    case PGP_PKA_RSA_ENCRYPT_ONLY:
    case PGP_PKA_RSA_SIGN_ONLY:
        if (src.material->secret()) {
            auto &rsasrc = dynamic_cast<pgp::RSAKeyMaterial &>(*src.material);
            auto &rsadst = dynamic_cast<pgp::RSAKeyMaterial &>(*dst.material);
            rsadst.set_secret(rsasrc.d(), rsasrc.p(), rsasrc.q(), rsasrc.u());
        }
        break;
    case PGP_PKA_ELGAMAL:
    case PGP_PKA_ELGAMAL_ENCRYPT_OR_SIGN:
        if (src.material->secret()) {
            auto &egsrc = dynamic_cast<pgp::EGKeyMaterial &>(*src.material);
            auto &egdst = dynamic_cast<pgp::EGKeyMaterial &>(*dst.material);
            egdst.set_secret(egsrc.x());
        }
        break;
    case PGP_PKA_ECDSA:
    case PGP_PKA_ECDH:
    case PGP_PKA_EDDSA:
        if (src.material->secret()) {
            auto &ecsrc = dynamic_cast<pgp::ECKeyMaterial &>(*src.material);
            auto &ecdst = dynamic_cast<pgp::ECKeyMaterial &>(*dst.material);
            ecdst.set_secret(ecsrc.x());
        }
        break;
    default:
        RNP_LOG("Unsupported public key algorithm: %d", (int) src.alg);
        return false;
    }

    dst.sec_protection = src.sec_protection;
    dst.tag = is_subkey_pkt(dst.tag) ? PGP_PKT_SECRET_SUBKEY : PGP_PKT_SECRET_KEY;
    return true;
}

namespace rnp {
bool
KeyStore::load_g10(pgp_source_t &src, const KeyProvider *key_provider)
{
    try {
        /* read src to the memory */
        MemorySource memsrc(src);
        /* parse secret key: fills material and sec_protection only */
        pgp_key_pkt_t seckey;
        if (!g23_parse_seckey(seckey, (uint8_t *) memsrc.memory(), memsrc.size(), NULL)) {
            return false;
        }
        /* copy public key fields if any */
        Key key;
        if (key_provider) {
            pgp::KeyGrip grip = seckey.material->grip();
            auto         pubkey =
              key_provider->request_key(*KeySearch::create(grip), PGP_OP_MERGE_INFO);
            if (!pubkey) {
                return false;
            }

            /* public key packet has some more info then the secret part */
            key = Key(*pubkey, true);
            if (!copy_secret_fields(key.pkt(), seckey)) {
                return false;
            }
        } else {
            key.set_pkt(std::move(seckey));
        }
        /* set rawpkt */
        key.set_rawpkt(
          RawPacket((uint8_t *) memsrc.memory(), memsrc.size(), PGP_PKT_RESERVED));
        key.format = KeyFormat::G10;
        if (!add_key(key)) {
            return false;
        }
        return true;
    } catch (const std::exception &e) {
        /* LCOV_EXCL_START */
        RNP_LOG("%s", e.what());
        return false;
        /* LCOV_EXCL_END */
    }
}
} // namespace rnp

/*
 * Write G10 S-exp to buffer
 *
 * Supported format: (1:a2:ab(3:asd1:a))
 */
bool
gnupg_sexp_t::write(pgp_dest_t &dst) const noexcept
{
    bool res = false;
    try {
        std::ostringstream   oss(std::ios_base::binary);
        sexp_output_stream_t os(&oss);
        print_canonical(&os);
        const std::string &s = oss.str();
        const char *       ss = s.c_str();
        dst_write(&dst, ss, s.size());
        res = (dst.werr == RNP_SUCCESS);

    } catch (...) {
    }

    return res;
}

void
gnupg_sexp_t::add_pubkey(const pgp_key_pkt_t &key)
{
    switch (key.alg) {
    case PGP_PKA_DSA: {
        auto &dsa = dynamic_cast<const pgp::DSAKeyMaterial &>(*key.material);
        add("dsa");
        add_mpi("p", dsa.p());
        add_mpi("q", dsa.q());
        add_mpi("g", dsa.g());
        add_mpi("y", dsa.y());
        break;
    }
    case PGP_PKA_RSA_SIGN_ONLY:
    case PGP_PKA_RSA_ENCRYPT_ONLY:
    case PGP_PKA_RSA: {
        auto &rsa = dynamic_cast<const pgp::RSAKeyMaterial &>(*key.material);
        add("rsa");
        add_mpi("n", rsa.n());
        add_mpi("e", rsa.e());
        break;
    }
    case PGP_PKA_ELGAMAL: {
        auto &eg = dynamic_cast<const pgp::EGKeyMaterial &>(*key.material);
        add("elg");
        add_mpi("p", eg.p());
        add_mpi("g", eg.g());
        add_mpi("y", eg.y());
        break;
    }
    case PGP_PKA_ECDSA:
    case PGP_PKA_ECDH:
    case PGP_PKA_EDDSA: {
        auto &ec = dynamic_cast<const pgp::ECKeyMaterial &>(*key.material);
        add("ecc");
        add_curve("curve", ec.curve());
        add_mpi("q", ec.p());
        break;
    }
    default:
        RNP_LOG("Unsupported public key algorithm: %d", (int) key.alg);
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
}

void
gnupg_sexp_t::add_seckey(const pgp_key_pkt_t &key)
{
    switch (key.alg) {
    case PGP_PKA_DSA: {
        auto &dsa = dynamic_cast<const pgp::DSAKeyMaterial &>(*key.material);
        add_mpi("x", dsa.x());
        break;
    }
    case PGP_PKA_RSA_SIGN_ONLY:
    case PGP_PKA_RSA_ENCRYPT_ONLY:
    case PGP_PKA_RSA: {
        auto &rsa = dynamic_cast<const pgp::RSAKeyMaterial &>(*key.material);
        add_mpi("d", rsa.d());
        add_mpi("p", rsa.p());
        add_mpi("q", rsa.q());
        add_mpi("u", rsa.u());
        break;
    }
    case PGP_PKA_ELGAMAL: {
        auto &eg = dynamic_cast<const pgp::EGKeyMaterial &>(*key.material);
        add_mpi("x", eg.x());
        break;
    }
    case PGP_PKA_ECDSA:
    case PGP_PKA_ECDH:
    case PGP_PKA_EDDSA: {
        auto &ec = dynamic_cast<const pgp::ECKeyMaterial &>(*key.material);
        add_mpi("d", ec.x());
        break;
    }
    default:
        RNP_LOG("Unsupported public key algorithm: %d", (int) key.alg);
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
}

rnp::secure_bytes
gnupg_sexp_t::write_padded(size_t padblock) const
{
    rnp::MemoryDest raw;
    raw.set_secure(true);

    if (!write(raw.dst())) {
        RNP_LOG("failed to serialize s_exp");
        throw rnp::rnp_exception(RNP_ERROR_BAD_STATE);
    }

    // add padding!
    size_t padding = padblock - raw.writeb() % padblock;
    for (size_t i = 0; i < padding; i++) {
        raw.write("X", 1);
    }
    if (raw.werr()) {
        RNP_LOG("failed to write padding");
        throw rnp::rnp_exception(RNP_ERROR_BAD_STATE);
    }
    const uint8_t *mem = (uint8_t *) raw.memory();
    return rnp::secure_bytes(mem, mem + raw.writeb());
}

void
gnupg_sexp_t::add_protected_seckey(pgp_key_pkt_t &       seckey,
                                   const std::string &   password,
                                   rnp::SecurityContext &ctx)
{
    pgp_key_protection_t &prot = seckey.sec_protection;
    if (prot.s2k.specifier != PGP_S2KS_ITERATED_AND_SALTED) {
        RNP_LOG("Bad s2k specifier: %d", (int) prot.s2k.specifier);
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
    const format_info *format =
      find_format(prot.symm_alg, prot.cipher_mode, prot.s2k.hash_alg);
    if (!format) {
        RNP_LOG("Unknown protection format.");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }

    // randomize IV and salt
    ctx.rng.get(prot.iv, sizeof(prot.iv));
    ctx.rng.get(prot.s2k.salt, sizeof(prot.s2k.salt));

    // write seckey
    gnupg_sexp_t raw_s_exp;
    auto         psub_s_exp = raw_s_exp.add_sub();
    psub_s_exp->add_seckey(seckey);

    // calculate hash
    char    protected_at[G10_PROTECTED_AT_SIZE + 1];
    uint8_t checksum[G10_SHA1_HASH_SIZE];
    // TODO: how critical is it if we have a skewed timestamp here due to y2k38 problem?
    struct tm tm = {};
    rnp_gmtime(ctx.time(), tm);
    strftime(protected_at, sizeof(protected_at), "%Y%m%dT%H%M%S", &tm);
    if (!g10_calculated_hash(seckey, protected_at, checksum)) {
        throw rnp::rnp_exception(RNP_ERROR_BAD_STATE);
    }

    psub_s_exp = raw_s_exp.add_sub();
    psub_s_exp->add("hash");
    psub_s_exp->add("sha1");
    psub_s_exp->add(checksum, sizeof(checksum));

    /* write raw secret key to the memory */
    rnp::secure_bytes rawkey = raw_s_exp.write_padded(format->cipher_block_size);

    /* derive encrypting key */
    unsigned keysize = pgp_key_size(prot.symm_alg);
    if (!keysize) {
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }

    rnp::secure_array<uint8_t, PGP_MAX_KEY_SIZE> derived_key;
    if (pgp_s2k_iterated(format->hash_alg,
                         derived_key.data(),
                         keysize,
                         password.c_str(),
                         prot.s2k.salt,
                         prot.s2k.iterations)) {
        RNP_LOG("s2k key derivation failed");
        throw rnp::rnp_exception(RNP_ERROR_BAD_STATE);
    }

    /* encrypt raw key */
    std::unique_ptr<Cipher> enc(
      Cipher::encryption(format->cipher, format->cipher_mode, 0, true));
    if (!enc || !enc->set_key(derived_key.data(), keysize) ||
        !enc->set_iv(prot.iv, format->iv_size)) {
        throw rnp::rnp_exception(RNP_ERROR_BAD_STATE);
    }

    size_t               output_written, input_consumed;
    std::vector<uint8_t> enckey(rawkey.size());

    if (!enc->finish(enckey.data(),
                     enckey.size(),
                     &output_written,
                     rawkey.data(),
                     rawkey.size(),
                     &input_consumed)) {
        RNP_LOG("Encryption failed");
        throw rnp::rnp_exception(RNP_ERROR_BAD_STATE);
    }

    /* build s_exp with encrypted key */
    psub_s_exp = add_sub();
    psub_s_exp->add("protected");
    psub_s_exp->add(format->g10_type);
    /* protection params: s2k, iv */
    auto psub_sub_s_exp = psub_s_exp->add_sub();
    /* s2k params: hash, salt, iterations */
    auto psub_sub_sub_s_exp = psub_sub_s_exp->add_sub();
    psub_sub_sub_s_exp->add("sha1");
    psub_sub_sub_s_exp->add(prot.s2k.salt, PGP_SALT_SIZE);
    psub_sub_sub_s_exp->add(prot.s2k.iterations);
    psub_sub_s_exp->add(prot.iv, format->iv_size);
    /* encrypted key data itself */
    psub_s_exp->add(enckey.data(), enckey.size());
    /* protected-at */
    psub_s_exp = add_sub();
    psub_s_exp->add("protected-at");
    psub_s_exp->add((uint8_t *) protected_at, G10_PROTECTED_AT_SIZE);
}

bool
g10_write_seckey(pgp_dest_t *          dst,
                 pgp_key_pkt_t *       seckey,
                 const char *          password,
                 rnp::SecurityContext &ctx)
{
    bool is_protected = true;

    switch (seckey->sec_protection.s2k.usage) {
    case PGP_S2KU_NONE:
        is_protected = false;
        break;
    case PGP_S2KU_ENCRYPTED_AND_HASHED:
        is_protected = true;
        // TODO: these are forced for now, until openpgp-native is implemented
        seckey->sec_protection.symm_alg = PGP_SA_AES_128;
        seckey->sec_protection.cipher_mode = PGP_CIPHER_MODE_CBC;
        seckey->sec_protection.s2k.hash_alg = PGP_HASH_SHA1;
        break;
    default:
        RNP_LOG("unsupported s2k usage");
        return false;
    }

    try {
        gnupg_sexp_t s_exp;
        s_exp.add(is_protected ? "protected-private-key" : "private-key");
        auto pkey = s_exp.add_sub();
        pkey->add_pubkey(*seckey);

        if (is_protected) {
            pkey->add_protected_seckey(*seckey, password, ctx);
        } else {
            pkey->add_seckey(*seckey);
        }
        return s_exp.write(*dst) && !dst->werr;
    } catch (const std::exception &e) {
        RNP_LOG("Failed to write g10 key: %s", e.what());
        return false;
    }
}

static bool
g10_calculated_hash(const pgp_key_pkt_t &key, const char *protected_at, uint8_t *checksum)
{
    try {
        /* populate s_exp */
        gnupg_sexp_t s_exp;
        s_exp.add_pubkey(key);
        s_exp.add_seckey(key);
        auto s_sub_exp = s_exp.add_sub();
        s_sub_exp->add("protected-at");
        s_sub_exp->add((uint8_t *) protected_at, G10_PROTECTED_AT_SIZE);
        /* write it to memdst */
        rnp::MemoryDest memdst;
        memdst.set_secure(true);
        if (!s_exp.write(memdst.dst())) {
            RNP_LOG("Failed to write s_exp");
            return false;
        }
        auto hash = rnp::Hash::create(PGP_HASH_SHA1);
        hash->add(memdst.memory(), memdst.writeb());
        hash->finish(checksum);
        return true;
    } catch (const std::exception &e) {
        RNP_LOG("Failed to build s_exp: %s", e.what());
        return false;
    }
}

bool
rnp_key_store_gnupg_sexp_to_dst(rnp::Key &key, pgp_dest_t &dest)
{
    if (key.format != rnp::KeyFormat::G10) {
        RNP_LOG("incorrect format: %d", static_cast<int>(key.format));
        return false;
    }
    dst_write(dest, key.rawpkt().data());
    return dest.werr == RNP_SUCCESS;
}
