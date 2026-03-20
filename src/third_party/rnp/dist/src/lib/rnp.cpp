/*-
 * Copyright (c) 2017-2023, Ribose Inc.
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
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR
 * CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "crypto/common.h"
#include "key.hpp"
#include "defaults.h"
#include <assert.h>
#include <json_object.h>
#include <json.h>
#include <librepgp/stream-ctx.h>
#include <librepgp/stream-common.h>
#include <librepgp/stream-armor.h>
#include <librepgp/stream-parse.h>
#include <librepgp/stream-write.h>
#include <librepgp/stream-sig.h>
#include <librepgp/stream-packet.h>
#include <librepgp/stream-key.h>
#include <librepgp/stream-dump.h>
#include "rekey/rnp_key_store.h"
#include <rnp/rnp.h>
#include <stdarg.h>
#include <stdlib.h>
#ifdef _MSC_VER
#include "uniwin.h"
#else
#include <unistd.h>
#endif
#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif
#include <cinttypes>
#include <string.h>
#include <sys/stat.h>
#include <stdexcept>
#include "utils.h"
#include "str-utils.h"
#include "json-utils.h"
#include "version.h"
#include "ffi-priv-types.h"
#include "file-utils.h"

#define FFI_LOG(ffi, ...)            \
    do {                             \
        FILE *fp = stderr;           \
        if (ffi && ffi->errs) {      \
            fp = ffi->errs;          \
        }                            \
        RNP_LOG_FD(fp, __VA_ARGS__); \
    } while (0)

#if defined(RNP_EXPERIMENTAL_CRYPTO_REFRESH) != defined(ENABLE_CRYPTO_REFRESH)
#error "Invalid defines combination."
#endif
#if defined(RNP_EXPERIMENTAL_PQC) != defined(ENABLE_PQC)
#error "Invalid defines combination."
#endif

static rnp::Key *get_key_require_public(rnp_key_handle_t handle);
static rnp::Key *get_key_prefer_public(rnp_key_handle_t handle);
static rnp::Key *get_key_require_secret(rnp_key_handle_t handle);

static bool rnp_password_cb_bounce(const pgp_password_ctx_t *ctx,
                                   char *                    password,
                                   size_t                    password_size,
                                   void *                    userdata_void);

static rnp_result_t rnp_dump_src_to_json(pgp_source_t &src, uint32_t flags, char **result);

static bool
call_key_callback(rnp_ffi_t ffi, const rnp::KeySearch &search, bool secret)
{
    if (!ffi->getkeycb) {
        return false;
    }
    ffi->getkeycb(
      ffi, ffi->getkeycb_ctx, search.name().c_str(), search.value().c_str(), secret);
    return true;
}

static rnp::Key *
find_key(rnp_ffi_t             ffi,
         const rnp::KeySearch &search,
         bool                  secret,
         bool                  try_key_provider,
         rnp::Key *            after = nullptr)
{
    auto      ks = secret ? ffi->secring : ffi->pubring;
    rnp::Key *key = ks->search(search, after);
    if (!key && try_key_provider && call_key_callback(ffi, search, secret)) {
        // recurse and try the store search above once more
        return find_key(ffi, search, secret, false, after);
    }
    return key;
}

static rnp::Key *
ffi_key_provider(const pgp_key_request_ctx_t *ctx, void *userdata)
{
    rnp_ffi_t ffi = (rnp_ffi_t) userdata;
    return find_key(ffi, ctx->search, ctx->secret, true);
}

static const id_str_pair sig_type_map[] = {{PGP_SIG_BINARY, "binary"},
                                           {PGP_SIG_TEXT, "text"},
                                           {PGP_SIG_STANDALONE, "standalone"},
                                           {PGP_CERT_GENERIC, "certification (generic)"},
                                           {PGP_CERT_PERSONA, "certification (persona)"},
                                           {PGP_CERT_CASUAL, "certification (casual)"},
                                           {PGP_CERT_POSITIVE, "certification (positive)"},
                                           {PGP_SIG_SUBKEY, "subkey binding"},
                                           {PGP_SIG_PRIMARY, "primary key binding"},
                                           {PGP_SIG_DIRECT, "direct"},
                                           {PGP_SIG_REV_KEY, "key revocation"},
                                           {PGP_SIG_REV_SUBKEY, "subkey revocation"},
                                           {PGP_SIG_REV_CERT, "certification revocation"},
                                           {PGP_SIG_TIMESTAMP, "timestamp"},
                                           {PGP_SIG_3RD_PARTY, "third-party"},
                                           {0, NULL}};

static const id_str_pair cert_type_map[] = {{PGP_CERT_GENERIC, RNP_CERTIFICATION_GENERIC},
                                            {PGP_CERT_PERSONA, RNP_CERTIFICATION_PERSONA},
                                            {PGP_CERT_CASUAL, RNP_CERTIFICATION_CASUAL},
                                            {PGP_CERT_POSITIVE, RNP_CERTIFICATION_POSITIVE},
                                            {0, NULL}};

static const id_str_pair pubkey_alg_map[] = {
  {PGP_PKA_RSA, RNP_ALGNAME_RSA},
  {PGP_PKA_RSA_ENCRYPT_ONLY, RNP_ALGNAME_RSA},
  {PGP_PKA_RSA_SIGN_ONLY, RNP_ALGNAME_RSA},
  {PGP_PKA_ELGAMAL, RNP_ALGNAME_ELGAMAL},
  {PGP_PKA_ELGAMAL_ENCRYPT_OR_SIGN, RNP_ALGNAME_ELGAMAL},
  {PGP_PKA_DSA, RNP_ALGNAME_DSA},
  {PGP_PKA_ECDH, RNP_ALGNAME_ECDH},
  {PGP_PKA_ECDSA, RNP_ALGNAME_ECDSA},
  {PGP_PKA_EDDSA, RNP_ALGNAME_EDDSA},
  {PGP_PKA_SM2, RNP_ALGNAME_SM2},
#if defined(ENABLE_CRYPTO_REFRESH)
  {PGP_PKA_ED25519, RNP_ALGNAME_ED25519},
  {PGP_PKA_X25519, RNP_ALGNAME_X25519},
#endif
#if defined(ENABLE_PQC)
  {PGP_PKA_KYBER768_X25519, RNP_ALGNAME_KYBER768_X25519},
  //{PGP_PKA_KYBER1024_X448, RNP_ALGNAME_KYBER1024_X448},
  {PGP_PKA_KYBER768_P256, RNP_ALGNAME_KYBER768_P256},
  {PGP_PKA_KYBER1024_P384, RNP_ALGNAME_KYBER1024_P384},
  {PGP_PKA_KYBER768_BP256, RNP_ALGNAME_KYBER768_BP256},
  {PGP_PKA_KYBER1024_BP384, RNP_ALGNAME_KYBER1024_BP384},
  {PGP_PKA_DILITHIUM3_ED25519, RNP_ALGNAME_DILITHIUM3_ED25519},
  //{PGP_PKA_DILITHIUM5_ED448, RNP_ALGNAME_DILITHIUM5_ED448},
  {PGP_PKA_DILITHIUM3_P256, RNP_ALGNAME_DILITHIUM3_P256},
  {PGP_PKA_DILITHIUM5_P384, RNP_ALGNAME_DILITHIUM5_P384},
  {PGP_PKA_DILITHIUM3_BP256, RNP_ALGNAME_DILITHIUM3_BP256},
  {PGP_PKA_DILITHIUM5_BP384, RNP_ALGNAME_DILITHIUM5_BP384},
  {PGP_PKA_SPHINCSPLUS_SHA2, RNP_ALGNAME_SPHINCSPLUS_SHA2},
  {PGP_PKA_SPHINCSPLUS_SHAKE, RNP_ALGNAME_SPHINCSPLUS_SHAKE},
#endif
  {0, NULL}};

static const id_str_pair symm_alg_map[] = {{PGP_SA_IDEA, RNP_ALGNAME_IDEA},
                                           {PGP_SA_TRIPLEDES, RNP_ALGNAME_TRIPLEDES},
                                           {PGP_SA_CAST5, RNP_ALGNAME_CAST5},
                                           {PGP_SA_BLOWFISH, RNP_ALGNAME_BLOWFISH},
                                           {PGP_SA_AES_128, RNP_ALGNAME_AES_128},
                                           {PGP_SA_AES_192, RNP_ALGNAME_AES_192},
                                           {PGP_SA_AES_256, RNP_ALGNAME_AES_256},
                                           {PGP_SA_TWOFISH, RNP_ALGNAME_TWOFISH},
                                           {PGP_SA_CAMELLIA_128, RNP_ALGNAME_CAMELLIA_128},
                                           {PGP_SA_CAMELLIA_192, RNP_ALGNAME_CAMELLIA_192},
                                           {PGP_SA_CAMELLIA_256, RNP_ALGNAME_CAMELLIA_256},
                                           {PGP_SA_SM4, RNP_ALGNAME_SM4},
                                           {0, NULL}};

static const id_str_pair aead_alg_map[] = {
  {PGP_AEAD_NONE, "None"}, {PGP_AEAD_EAX, "EAX"}, {PGP_AEAD_OCB, "OCB"}, {0, NULL}};

static const id_str_pair cipher_mode_map[] = {{PGP_CIPHER_MODE_CFB, "CFB"},
                                              {PGP_CIPHER_MODE_CBC, "CBC"},
                                              {PGP_CIPHER_MODE_OCB, "OCB"},
                                              {0, NULL}};

static const id_str_pair compress_alg_map[] = {{PGP_C_NONE, "Uncompressed"},
                                               {PGP_C_ZIP, "ZIP"},
                                               {PGP_C_ZLIB, "ZLIB"},
                                               {PGP_C_BZIP2, "BZip2"},
                                               {0, NULL}};

static const id_str_pair hash_alg_map[] = {{PGP_HASH_MD5, RNP_ALGNAME_MD5},
                                           {PGP_HASH_SHA1, RNP_ALGNAME_SHA1},
                                           {PGP_HASH_RIPEMD, RNP_ALGNAME_RIPEMD160},
                                           {PGP_HASH_SHA256, RNP_ALGNAME_SHA256},
                                           {PGP_HASH_SHA384, RNP_ALGNAME_SHA384},
                                           {PGP_HASH_SHA512, RNP_ALGNAME_SHA512},
                                           {PGP_HASH_SHA224, RNP_ALGNAME_SHA224},
                                           {PGP_HASH_SHA3_256, RNP_ALGNAME_SHA3_256},
                                           {PGP_HASH_SHA3_512, RNP_ALGNAME_SHA3_512},
                                           {PGP_HASH_SM3, RNP_ALGNAME_SM3},
                                           {0, NULL}};

#if defined(ENABLE_PQC)
static const id_str_pair sphincsplus_params_map[] = {{sphincsplus_simple_128s, "128s"},
                                                     {sphincsplus_simple_128f, "128f"},
                                                     {sphincsplus_simple_192s, "192s"},
                                                     {sphincsplus_simple_192f, "192f"},
                                                     {sphincsplus_simple_256s, "256s"},
                                                     {sphincsplus_simple_256f, "256f"},
                                                     {0, NULL}};
#endif

static const id_str_pair s2k_type_map[] = {
  {PGP_S2KS_SIMPLE, "Simple"},
  {PGP_S2KS_SALTED, "Salted"},
  {PGP_S2KS_ITERATED_AND_SALTED, "Iterated and salted"},
  {0, NULL}};

static const id_str_pair key_usage_map[] = {
  {PGP_KF_SIGN, "sign"},
  {PGP_KF_CERTIFY, "certify"},
  {PGP_KF_ENCRYPT, "encrypt"},
  {PGP_KF_AUTH, "authenticate"},
  {0, NULL},
};

static const id_str_pair key_flags_map[] = {
  {PGP_KF_SPLIT, "split"},
  {PGP_KF_SHARED, "shared"},
  {0, NULL},
};

static const id_str_pair key_server_prefs_map[] = {{PGP_KEY_SERVER_NO_MODIFY, "no-modify"},
                                                   {0, NULL}};

static const id_str_pair armor_type_map[] = {{PGP_ARMORED_MESSAGE, "message"},
                                             {PGP_ARMORED_PUBLIC_KEY, "public key"},
                                             {PGP_ARMORED_SECRET_KEY, "secret key"},
                                             {PGP_ARMORED_SIGNATURE, "signature"},
                                             {PGP_ARMORED_CLEARTEXT, "cleartext"},
                                             {0, NULL}};

static const id_str_pair key_import_status_map[] = {
  {PGP_KEY_IMPORT_STATUS_UNKNOWN, "unknown"},
  {PGP_KEY_IMPORT_STATUS_UNCHANGED, "unchanged"},
  {PGP_KEY_IMPORT_STATUS_UPDATED, "updated"},
  {PGP_KEY_IMPORT_STATUS_NEW, "new"},
  {0, NULL}};

static const id_str_pair sig_import_status_map[] = {
  {PGP_SIG_IMPORT_STATUS_UNKNOWN, "unknown"},
  {PGP_SIG_IMPORT_STATUS_UNKNOWN_KEY, "unknown key"},
  {PGP_SIG_IMPORT_STATUS_UNCHANGED, "unchanged"},
  {PGP_SIG_IMPORT_STATUS_NEW, "new"},
  {0, NULL}};

static const id_str_pair revocation_code_map[] = {
  {PGP_REVOCATION_NO_REASON, "no"},
  {PGP_REVOCATION_SUPERSEDED, "superseded"},
  {PGP_REVOCATION_COMPROMISED, "compromised"},
  {PGP_REVOCATION_RETIRED, "retired"},
  {PGP_REVOCATION_NO_LONGER_VALID, "no longer valid"},
  {0, NULL}};

static bool
symm_alg_supported(int alg)
{
    return pgp_is_sa_supported(alg, true);
}

static bool
hash_alg_supported(int alg)
{
    switch (alg) {
    case PGP_HASH_MD5:
    case PGP_HASH_SHA1:
#if defined(ENABLE_RIPEMD160)
    case PGP_HASH_RIPEMD:
#endif
    case PGP_HASH_SHA256:
    case PGP_HASH_SHA384:
    case PGP_HASH_SHA512:
    case PGP_HASH_SHA224:
    case PGP_HASH_SHA3_256:
    case PGP_HASH_SHA3_512:
#if defined(ENABLE_SM2)
    case PGP_HASH_SM3:
#endif
        return true;
    default:
        return false;
    }
}

static bool
aead_alg_supported(int alg)
{
    switch (alg) {
    case PGP_AEAD_NONE:
#if defined(ENABLE_AEAD)
#if !defined(CRYPTO_BACKEND_OPENSSL)
    case PGP_AEAD_EAX:
#endif
    case PGP_AEAD_OCB:
#endif
        return true;
    default:
        return false;
    }
}

static bool
pub_alg_supported(int alg)
{
    switch (alg) {
    case PGP_PKA_RSA:
    case PGP_PKA_ELGAMAL:
    case PGP_PKA_DSA:
    case PGP_PKA_ECDH:
    case PGP_PKA_ECDSA:
    case PGP_PKA_EDDSA:
#if defined(ENABLE_SM2)
    case PGP_PKA_SM2:
#endif
#if defined(ENABLE_CRYPTO_REFRESH)
    case PGP_PKA_X25519:
    case PGP_PKA_ED25519:
#endif
#if defined(ENABLE_PQC)
    case PGP_PKA_KYBER768_X25519:
    // case PGP_PKA_KYBER1024_X448:
    case PGP_PKA_KYBER768_P256:
    case PGP_PKA_KYBER1024_P384:
    case PGP_PKA_KYBER768_BP256:
    case PGP_PKA_KYBER1024_BP384:
    case PGP_PKA_DILITHIUM3_ED25519:
    // case PGP_PKA_DILITHIUM5_ED448:
    case PGP_PKA_DILITHIUM3_P256:
    case PGP_PKA_DILITHIUM5_P384:
    case PGP_PKA_DILITHIUM3_BP256:
    case PGP_PKA_DILITHIUM5_BP384:
    case PGP_PKA_SPHINCSPLUS_SHA2:
    case PGP_PKA_SPHINCSPLUS_SHAKE:
#endif
        return true;
    default:
        return false;
    }
}

static bool
z_alg_supported(int alg)
{
    switch (alg) {
    case PGP_C_NONE:
    case PGP_C_ZIP:
    case PGP_C_ZLIB:
    case PGP_C_BZIP2:
        return true;
    default:
        return false;
    }
}

static bool
curve_str_to_type(const char *str, pgp_curve_t *value)
{
    *value = pgp::ec::Curve::by_name(str);
    return pgp::ec::Curve::is_supported(*value);
}

static bool
curve_type_to_str(pgp_curve_t type, const char **str)
{
    auto desc = pgp::ec::Curve::get(type);
    if (!desc) {
        return false;
    }
    *str = desc->pgp_name;
    return true;
}

static bool
str_to_cipher(const char *str, pgp_symm_alg_t *cipher)
{
    auto alg = id_str_pair::lookup(symm_alg_map, str, PGP_SA_UNKNOWN);
    if (!symm_alg_supported(alg)) {
        return false;
    }
    *cipher = static_cast<pgp_symm_alg_t>(alg);
    return true;
}

static bool
str_to_hash_alg(const char *str, pgp_hash_alg_t *hash_alg)
{
    auto alg = id_str_pair::lookup(hash_alg_map, str, PGP_HASH_UNKNOWN);
    if (!hash_alg_supported(alg)) {
        return false;
    }
    *hash_alg = static_cast<pgp_hash_alg_t>(alg);
    return true;
}

static bool
str_to_aead_alg(const char *str, pgp_aead_alg_t *aead_alg)
{
    auto alg = id_str_pair::lookup(aead_alg_map, str, PGP_AEAD_UNKNOWN);
    if (!aead_alg_supported(alg)) {
        return false;
    }
    *aead_alg = static_cast<pgp_aead_alg_t>(alg);
    return true;
}

static bool
str_to_compression_alg(const char *str, pgp_compression_type_t *zalg)
{
    auto alg = id_str_pair::lookup(compress_alg_map, str, PGP_C_UNKNOWN);
    if (!z_alg_supported(alg)) {
        return false;
    }
    *zalg = static_cast<pgp_compression_type_t>(alg);
    return true;
}

static bool
str_to_revocation_type(const char *str, pgp_revocation_type_t *code)
{
    pgp_revocation_type_t rev = static_cast<pgp_revocation_type_t>(
      id_str_pair::lookup(revocation_code_map, str, PGP_REVOCATION_NO_REASON));
    if ((rev == PGP_REVOCATION_NO_REASON) && !rnp::str_case_eq(str, "no")) {
        return false;
    }
    *code = rev;
    return true;
}

static bool
str_to_cipher_mode(const char *str, pgp_cipher_mode_t *mode)
{
    pgp_cipher_mode_t c_mode = static_cast<pgp_cipher_mode_t>(
      id_str_pair::lookup(cipher_mode_map, str, PGP_CIPHER_MODE_NONE));
    if (c_mode == PGP_CIPHER_MODE_NONE) {
        return false;
    }

    *mode = c_mode;
    return true;
}

static bool
str_to_pubkey_alg(const char *str, pgp_pubkey_alg_t *pub_alg)
{
    auto alg = id_str_pair::lookup(pubkey_alg_map, str, PGP_PKA_NOTHING);
    if (!pub_alg_supported(alg)) {
        return false;
    }
    *pub_alg = static_cast<pgp_pubkey_alg_t>(alg);
    return true;
}

static bool
str_to_key_flag(const char *str, uint8_t *flag)
{
    uint8_t _flag = id_str_pair::lookup(key_usage_map, str);
    if (!_flag) {
        return false;
    }
    *flag = _flag;
    return true;
}

static bool
parse_ks_format(rnp::KeyFormat *key_store_format, const char *format)
{
    if (!strcmp(format, RNP_KEYSTORE_GPG)) {
        *key_store_format = rnp::KeyFormat::GPG;
    } else if (!strcmp(format, RNP_KEYSTORE_KBX)) {
        *key_store_format = rnp::KeyFormat::KBX;
    } else if (!strcmp(format, RNP_KEYSTORE_G10)) {
        *key_store_format = rnp::KeyFormat::G10;
    } else {
        return false;
    }
    return true;
}

static rnp_result_t
hex_encode_value(const uint8_t *value, size_t len, char **res)
{
    size_t hex_len = len * 2 + 1;
    *res = (char *) malloc(hex_len);
    if (!*res) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    if (!rnp::hex_encode(value, len, *res, hex_len, rnp::HexFormat::Uppercase)) {
        /* LCOV_EXCL_START */
        free(*res);
        *res = NULL;
        return RNP_ERROR_GENERIC;
        /* LCOV_EXCL_END */
    }
    return RNP_SUCCESS;
}

static rnp_result_t
ret_str_value(const char *str, char **res)
{
    if (!str) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    char *strcp = strdup(str);
    if (!strcp) {
        *res = NULL;                    // LCOV_EXCL_LINE
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    *res = strcp;
    return RNP_SUCCESS;
}

static rnp_result_t
ret_vec_value(const std::vector<uint8_t> &vec, uint8_t **buf, size_t *buf_len)
{
    *buf = (uint8_t *) calloc(1, vec.size());
    if (!*buf) {
        return RNP_ERROR_OUT_OF_MEMORY;
    }
    memcpy(*buf, vec.data(), vec.size());
    *buf_len = vec.size();
    return RNP_SUCCESS;
}

static rnp_result_t
get_map_value(const id_str_pair *map, int val, char **res)
{
    return ret_str_value(id_str_pair::lookup(map, val, NULL), res);
}

static rnp_result_t
ret_fingerprint(const pgp::Fingerprint &fp, char **res)
{
    return hex_encode_value(fp.data(), fp.size(), res);
}

static rnp_result_t
ret_keyid(const pgp::KeyID &keyid, char **res)
{
    return hex_encode_value(keyid.data(), keyid.size(), res);
}

static rnp_result_t
ret_grip(const pgp::KeyGrip &grip, char **res)
{
    return hex_encode_value(grip.data(), grip.size(), res);
}

static uint32_t
ffi_exception(FILE *fp, const char *func, const char *msg, uint32_t ret = RNP_ERROR_GENERIC)
{
    if (rnp_log_switch()) {
        fprintf(
          fp, "[%s()] Error 0x%08X (%s): %s\n", func, ret, rnp_result_to_string(ret), msg);
    }
    return ret;
}

#define FFI_GUARD_FP(fp)                                                            \
    catch (rnp::rnp_exception & e)                                                  \
    {                                                                               \
        return ffi_exception((fp), __func__, e.what(), e.code());                   \
    }                                                                               \
    catch (std::bad_alloc &)                                                        \
    {                                                                               \
        return ffi_exception((fp), __func__, "bad_alloc", RNP_ERROR_OUT_OF_MEMORY); \
    }                                                                               \
    catch (std::exception & e)                                                      \
    {                                                                               \
        return ffi_exception((fp), __func__, e.what());                             \
    }                                                                               \
    catch (...)                                                                     \
    {                                                                               \
        return ffi_exception((fp), __func__, "unknown exception");                  \
    }

#define FFI_GUARD FFI_GUARD_FP((stderr))

rnp_ffi_st::rnp_ffi_st(rnp::KeyFormat pub_fmt, rnp::KeyFormat sec_fmt)
{
    errs = stderr;
    pubring = new rnp::KeyStore("", context, pub_fmt);
    secring = new rnp::KeyStore("", context, sec_fmt);
    getkeycb = NULL;
    getkeycb_ctx = NULL;
    getpasscb = NULL;
    getpasscb_ctx = NULL;
    key_provider.callback = ffi_key_provider;
    key_provider.userdata = this;
    pass_provider.callback = rnp_password_cb_bounce;
    pass_provider.userdata = this;
}

rnp::RNG &
rnp_ffi_st::rng() noexcept
{
    return context.rng;
}

rnp::SecurityProfile &
rnp_ffi_st::profile() noexcept
{
    return context.profile;
}

rnp_result_t
rnp_ffi_create(rnp_ffi_t *ffi, const char *pub_format, const char *sec_format)
try {
    // checks
    if (!ffi || !pub_format || !sec_format) {
        return RNP_ERROR_NULL_POINTER;
    }

    auto pub_ks_format = rnp::KeyFormat::Unknown;
    auto sec_ks_format = rnp::KeyFormat::Unknown;
    if (!parse_ks_format(&pub_ks_format, pub_format) ||
        !parse_ks_format(&sec_ks_format, sec_format)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    struct rnp_ffi_st *ob = new rnp_ffi_st(pub_ks_format, sec_ks_format);
    *ffi = ob;
    return RNP_SUCCESS;
}
FFI_GUARD

static bool
is_std_file(FILE *fp)
{
    return fp == stdout || fp == stderr;
}

static void
close_io_file(FILE **fp)
{
    if (*fp && !is_std_file(*fp)) {
        fclose(*fp);
    }
    *fp = NULL;
}

rnp_ffi_st::~rnp_ffi_st()
{
    close_io_file(&errs);
    delete pubring;
    delete secring;
}

rnp_result_t
rnp_ffi_destroy(rnp_ffi_t ffi)
try {
    if (ffi) {
        delete ffi;
    }
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_ffi_set_log_fd(rnp_ffi_t ffi, int fd)
try {
    // checks
    if (!ffi) {
        return RNP_ERROR_NULL_POINTER;
    }

    // open
    FILE *errs = rnp_fdopen(fd, "a");
    if (!errs) {
        return RNP_ERROR_ACCESS;
    }
    // close previous streams and replace them
    close_io_file(&ffi->errs);
    ffi->errs = errs;
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_ffi_set_key_provider(rnp_ffi_t ffi, rnp_get_key_cb getkeycb, void *getkeycb_ctx)
try {
    if (!ffi) {
        return RNP_ERROR_NULL_POINTER;
    }
    ffi->getkeycb = getkeycb;
    ffi->getkeycb_ctx = getkeycb_ctx;
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_ffi_set_pass_provider(rnp_ffi_t ffi, rnp_password_cb getpasscb, void *getpasscb_ctx)
try {
    if (!ffi) {
        return RNP_ERROR_NULL_POINTER;
    }
    ffi->getpasscb = getpasscb;
    ffi->getpasscb_ctx = getpasscb_ctx;
    return RNP_SUCCESS;
}
FFI_GUARD

static const char *
operation_description(uint8_t op)
{
    switch (op) {
    case PGP_OP_ADD_SUBKEY:
        return "add subkey";
    case PGP_OP_ADD_USERID:
        return "add userid";
    case PGP_OP_SIGN:
        return "sign";
    case PGP_OP_DECRYPT:
        return "decrypt";
    case PGP_OP_UNLOCK:
        return "unlock";
    case PGP_OP_PROTECT:
        return "protect";
    case PGP_OP_UNPROTECT:
        return "unprotect";
    case PGP_OP_DECRYPT_SYM:
        return "decrypt (symmetric)";
    case PGP_OP_ENCRYPT_SYM:
        return "encrypt (symmetric)";
    default:
        return "unknown";
    }
}

static bool
rnp_password_cb_bounce(const pgp_password_ctx_t *ctx,
                       char *                    password,
                       size_t                    password_size,
                       void *                    userdata_void)
{
    rnp_ffi_t ffi = (rnp_ffi_t) userdata_void;

    if (!ffi || !ffi->getpasscb) {
        return false;
    }

    rnp_key_handle_st key(ffi, nullptr, (rnp::Key *) ctx->key);
    return ffi->getpasscb(ffi,
                          ffi->getpasscb_ctx,
                          ctx->key ? &key : NULL,
                          operation_description(ctx->op),
                          password,
                          password_size);
}

const char *
rnp_result_to_string(rnp_result_t result)
{
    switch (result) {
    case RNP_SUCCESS:
        return "Success";

    case RNP_ERROR_GENERIC:
        return "Unknown error";
    case RNP_ERROR_BAD_FORMAT:
        return "Bad format";
    case RNP_ERROR_BAD_PARAMETERS:
        return "Bad parameters";
    case RNP_ERROR_NOT_IMPLEMENTED:
        return "Not implemented";
    case RNP_ERROR_NOT_SUPPORTED:
        return "Not supported";
    case RNP_ERROR_OUT_OF_MEMORY:
        return "Out of memory";
    case RNP_ERROR_SHORT_BUFFER:
        return "Buffer too short";
    case RNP_ERROR_NULL_POINTER:
        return "Null pointer";

    case RNP_ERROR_ACCESS:
        return "Error accessing file";
    case RNP_ERROR_READ:
        return "Error reading file";
    case RNP_ERROR_WRITE:
        return "Error writing file";

    case RNP_ERROR_BAD_STATE:
        return "Bad state";
    case RNP_ERROR_MAC_INVALID:
        return "Invalid MAC";
    case RNP_ERROR_SIGNATURE_INVALID:
        return "Invalid signature";
    case RNP_ERROR_KEY_GENERATION:
        return "Error during key generation";
    case RNP_ERROR_BAD_PASSWORD:
        return "Bad password";
    case RNP_ERROR_KEY_NOT_FOUND:
        return "Key not found";
    case RNP_ERROR_NO_SUITABLE_KEY:
        return "No suitable key";
    case RNP_ERROR_DECRYPT_FAILED:
        return "Decryption failed";
    case RNP_ERROR_ENCRYPT_FAILED:
        return "Encryption failed";
    case RNP_ERROR_RNG:
        return "Failure of random number generator";
    case RNP_ERROR_SIGNING_FAILED:
        return "Signing failed";
    case RNP_ERROR_NO_SIGNATURES_FOUND:
        return "No signatures found cannot verify";

    case RNP_ERROR_SIGNATURE_EXPIRED:
        return "Expired signature";
    case RNP_ERROR_VERIFICATION_FAILED:
        return "Signature verification failed cannot verify";
    case RNP_ERROR_SIGNATURE_UNKNOWN:
        return "Unknown signature";

    case RNP_ERROR_NOT_ENOUGH_DATA:
        return "Not enough data";
    case RNP_ERROR_UNKNOWN_TAG:
        return "Unknown tag";
    case RNP_ERROR_PACKET_NOT_CONSUMED:
        return "Packet not consumed";
    case RNP_ERROR_NO_USERID:
        return "No userid";
    case RNP_ERROR_EOF:
        return "EOF detected";
    }

    return "Unsupported error code";
}

const char *
rnp_version_string()
{
    return RNP_VERSION_STRING;
}

const char *
rnp_version_string_full()
{
    return RNP_VERSION_STRING_FULL;
}

uint32_t
rnp_version()
{
    return RNP_VERSION_CODE;
}

uint32_t
rnp_version_for(uint32_t major, uint32_t minor, uint32_t patch)
{
    if (major > RNP_VERSION_COMPONENT_MASK || minor > RNP_VERSION_COMPONENT_MASK ||
        patch > RNP_VERSION_COMPONENT_MASK) {
        RNP_LOG("invalid version, out of range: %d.%d.%d", major, minor, patch);
        return 0;
    }
    return RNP_VERSION_CODE_FOR(major, minor, patch);
}

uint32_t
rnp_version_major(uint32_t version)
{
    return (version >> RNP_VERSION_MAJOR_SHIFT) & RNP_VERSION_COMPONENT_MASK;
}

uint32_t
rnp_version_minor(uint32_t version)
{
    return (version >> RNP_VERSION_MINOR_SHIFT) & RNP_VERSION_COMPONENT_MASK;
}

uint32_t
rnp_version_patch(uint32_t version)
{
    return (version >> RNP_VERSION_PATCH_SHIFT) & RNP_VERSION_COMPONENT_MASK;
}

uint64_t
rnp_version_commit_timestamp()
{
    return RNP_VERSION_COMMIT_TIMESTAMP;
}

#ifndef RNP_NO_DEPRECATED
/* LCOV_EXCL_START */
rnp_result_t
rnp_enable_debug(const char *file)
try {
    return RNP_SUCCESS;
}
FFI_GUARD
/* LCOV_EXCL_END */
#endif

#ifndef RNP_NO_DEPRECATED
/* LCOV_EXCL_START */
rnp_result_t
rnp_disable_debug()
try {
    return RNP_SUCCESS;
}
FFI_GUARD
/* LCOV_EXCL_END */
#endif

rnp_result_t
rnp_get_default_homedir(char **homedir)
try {
    // checks
    if (!homedir) {
        return RNP_ERROR_NULL_POINTER;
    }

    // get the users home dir
    auto home = rnp::path::HOME(".rnp");
    if (home.empty()) {
        return RNP_ERROR_NOT_SUPPORTED;
    }
    return ret_str_value(home.c_str(), homedir);
}
FFI_GUARD

rnp_result_t
rnp_detect_homedir_info(
  const char *homedir, char **pub_format, char **pub_path, char **sec_format, char **sec_path)
try {
    // checks
    if (!homedir || !pub_format || !pub_path || !sec_format || !sec_path) {
        return RNP_ERROR_NULL_POINTER;
    }

    // we only support the common cases of GPG+GPG or GPG+G10, we don't
    // support unused combinations like KBX+KBX

    *pub_format = NULL;
    *pub_path = NULL;
    *sec_format = NULL;
    *sec_path = NULL;

    // check for pubring.kbx file and for private-keys-v1.d dir
    std::string pub = rnp::path::append(homedir, "pubring.kbx");
    std::string sec = rnp::path::append(homedir, "private-keys-v1.d");
    if (rnp::path::exists(pub) && rnp::path::exists(sec, true)) {
        *pub_format = strdup("KBX");
        *sec_format = strdup("G10");
    } else {
        // check for pubring.gpg and secring.gpg
        pub = rnp::path::append(homedir, "pubring.gpg");
        sec = rnp::path::append(homedir, "secring.gpg");
        if (rnp::path::exists(pub) && rnp::path::exists(sec)) {
            *pub_format = strdup("GPG");
            *sec_format = strdup("GPG");
        } else {
            // we leave the *formats as NULL if we were not able to determine the format
            // (but no error occurred)
            return RNP_SUCCESS;
        }
    }

    // set paths
    *pub_path = strdup(pub.c_str());
    *sec_path = strdup(sec.c_str());

    // check for allocation failures
    if (*pub_format && *pub_path && *sec_format && *sec_path) {
        return RNP_SUCCESS;
    }

    /* LCOV_EXCL_START */
    free(*pub_format);
    *pub_format = NULL;
    free(*pub_path);
    *pub_path = NULL;
    free(*sec_format);
    *sec_format = NULL;
    free(*sec_path);
    *sec_path = NULL;
    return RNP_ERROR_OUT_OF_MEMORY;
    /* LCOV_EXCL_END */
}
FFI_GUARD

rnp_result_t
rnp_detect_key_format(const uint8_t buf[], size_t buf_len, char **format)
try {
    // checks
    if (!buf || !format) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!buf_len) {
        return RNP_ERROR_SHORT_BUFFER;
    }

    // ordered from most reliable detection to least
    if (buf_len >= 12 && memcmp(buf + 8, "KBXf", 4) == 0) {
        // KBX has a magic KBXf marker
        return ret_str_value("KBX", format);
    } else if (buf_len >= 5 && memcmp(buf, "-----", 5) == 0) {
        // likely armored GPG
        return ret_str_value("GPG", format);
    } else if (buf[0] == '(') {
        // G10 is s-exprs and should start end end with parentheses
        return ret_str_value("G10", format);
    } else if (buf[0] & PGP_PTAG_ALWAYS_SET) {
        // this is harder to reliably determine, but could likely be improved
        return ret_str_value("GPG", format);
    }
    *format = NULL;
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_calculate_iterations(const char *hash, size_t msec, size_t *iterations)
try {
    if (!hash || !iterations) {
        return RNP_ERROR_NULL_POINTER;
    }
    pgp_hash_alg_t halg = PGP_HASH_UNKNOWN;
    if (!str_to_hash_alg(hash, &halg)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    *iterations = pgp_s2k_compute_iters(halg, msec, 0);
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_supports_feature(const char *type, const char *name, bool *supported)
try {
    if (!type || !name || !supported) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (rnp::str_case_eq(type, RNP_FEATURE_SYMM_ALG)) {
        pgp_symm_alg_t alg = PGP_SA_UNKNOWN;
        *supported = str_to_cipher(name, &alg);
    } else if (rnp::str_case_eq(type, RNP_FEATURE_AEAD_ALG)) {
        pgp_aead_alg_t alg = PGP_AEAD_UNKNOWN;
        *supported = str_to_aead_alg(name, &alg);
    } else if (rnp::str_case_eq(type, RNP_FEATURE_PROT_MODE)) {
        // for now we support only CFB for key encryption
        *supported = rnp::str_case_eq(name, "CFB");
    } else if (rnp::str_case_eq(type, RNP_FEATURE_PK_ALG)) {
        pgp_pubkey_alg_t alg = PGP_PKA_NOTHING;
        *supported = str_to_pubkey_alg(name, &alg);
    } else if (rnp::str_case_eq(type, RNP_FEATURE_HASH_ALG)) {
        pgp_hash_alg_t alg = PGP_HASH_UNKNOWN;
        *supported = str_to_hash_alg(name, &alg);
    } else if (rnp::str_case_eq(type, RNP_FEATURE_COMP_ALG)) {
        pgp_compression_type_t alg = PGP_C_UNKNOWN;
        *supported = str_to_compression_alg(name, &alg);
    } else if (rnp::str_case_eq(type, RNP_FEATURE_CURVE)) {
        pgp_curve_t curve = PGP_CURVE_UNKNOWN;
        *supported = curve_str_to_type(name, &curve);
    } else {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return RNP_SUCCESS;
}
FFI_GUARD

static rnp_result_t
json_array_add_id_str(json_object *arr, const id_str_pair *map, bool (*check)(int))
{
    while (map->str) {
        if (check(map->id) && !json_array_add(arr, map->str)) {
            return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
        }
        map++;
    }
    return RNP_SUCCESS;
}

rnp_result_t
rnp_supported_features(const char *type, char **result)
try {
    if (!type || !result) {
        return RNP_ERROR_NULL_POINTER;
    }

    json_object *features = json_object_new_array();
    if (!features) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    rnp::JSONObject featwrap(features);
    rnp_result_t    ret = RNP_ERROR_BAD_PARAMETERS;

    if (rnp::str_case_eq(type, RNP_FEATURE_SYMM_ALG)) {
        ret = json_array_add_id_str(features, symm_alg_map, symm_alg_supported);
    } else if (rnp::str_case_eq(type, RNP_FEATURE_AEAD_ALG)) {
        ret = json_array_add_id_str(features, aead_alg_map, aead_alg_supported);
    } else if (rnp::str_case_eq(type, RNP_FEATURE_PROT_MODE)) {
        ret = json_array_add_id_str(
          features, cipher_mode_map, [](int alg) { return alg == PGP_CIPHER_MODE_CFB; });
    } else if (rnp::str_case_eq(type, RNP_FEATURE_PK_ALG)) {
        ret = json_array_add_id_str(features, pubkey_alg_map, pub_alg_supported);
    } else if (rnp::str_case_eq(type, RNP_FEATURE_HASH_ALG)) {
        ret = json_array_add_id_str(features, hash_alg_map, hash_alg_supported);
    } else if (rnp::str_case_eq(type, RNP_FEATURE_COMP_ALG)) {
        ret = json_array_add_id_str(features, compress_alg_map, z_alg_supported);
    } else if (rnp::str_case_eq(type, RNP_FEATURE_CURVE)) {
        for (pgp_curve_t curve = PGP_CURVE_NIST_P_256; curve < PGP_CURVE_MAX;
             curve = (pgp_curve_t)(curve + 1)) {
            auto desc = pgp::ec::Curve::get(curve);
            if (!desc) {
                return RNP_ERROR_BAD_STATE; // LCOV_EXCL_LINE
            }
            if (!desc->supported) {
                continue;
            }
            if (!json_array_add(features, desc->pgp_name)) {
                return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
            }
        }
        ret = RNP_SUCCESS;
    }

    if (ret) {
        return ret;
    }
    return ret_str_value(json_object_to_json_string_ext(features, JSON_C_TO_STRING_PRETTY),
                         result);
}
FFI_GUARD

static bool
get_feature_sec_value(
  rnp_ffi_t ffi, const char *stype, const char *sname, rnp::FeatureType &type, int &value)
{
    /* check type */
    if (rnp::str_case_eq(stype, RNP_FEATURE_HASH_ALG)) {
        type = rnp::FeatureType::Hash;
        /* check feature name */
        pgp_hash_alg_t alg = PGP_HASH_UNKNOWN;
        if (sname && !str_to_hash_alg(sname, &alg)) {
            FFI_LOG(ffi, "Unknown hash algorithm: %s", sname);
            return false;
        }
        value = alg;
        return true;
    }

    if (rnp::str_case_eq(stype, RNP_FEATURE_SYMM_ALG)) {
        type = rnp::FeatureType::Cipher;
        /* check feature name */
        pgp_symm_alg_t alg = PGP_SA_UNKNOWN;
        if (sname && !str_to_cipher(sname, &alg)) {
            FFI_LOG(ffi, "Unknown cipher: %s", sname);
            return false;
        }
        value = alg;
        return true;
    }

    FFI_LOG(ffi, "Unsupported feature type: %s", stype);
    return false;
}

static bool
get_feature_sec_level(rnp_ffi_t ffi, uint32_t flevel, rnp::SecurityLevel &level)
{
    switch (flevel) {
    case RNP_SECURITY_PROHIBITED:
        level = rnp::SecurityLevel::Disabled;
        break;
    case RNP_SECURITY_INSECURE:
        level = rnp::SecurityLevel::Insecure;
        break;
    case RNP_SECURITY_DEFAULT:
        level = rnp::SecurityLevel::Default;
        break;
    default:
        FFI_LOG(ffi, "Invalid security level : %" PRIu32, flevel);
        return false;
    }
    return true;
}

static bool
extract_flag(uint32_t &flags, uint32_t flag)
{
    bool res = flags & flag;
    flags &= ~flag;
    return res;
}

rnp_result_t
rnp_add_security_rule(rnp_ffi_t   ffi,
                      const char *type,
                      const char *name,
                      uint32_t    flags,
                      uint64_t    from,
                      uint32_t    level)
try {
    if (!ffi || !type || !name) {
        return RNP_ERROR_NULL_POINTER;
    }
    /* convert values */
    rnp::FeatureType   ftype;
    int                fvalue;
    rnp::SecurityLevel sec_level;
    if (!get_feature_sec_value(ffi, type, name, ftype, fvalue) ||
        !get_feature_sec_level(ffi, level, sec_level)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    /* check flags */
    bool rule_override = extract_flag(flags, RNP_SECURITY_OVERRIDE);
    bool verify_key = extract_flag(flags, RNP_SECURITY_VERIFY_KEY);
    bool verify_data = extract_flag(flags, RNP_SECURITY_VERIFY_DATA);
    if (flags) {
        FFI_LOG(ffi, "Unknown flags: %" PRIu32, flags);
        return RNP_ERROR_BAD_PARAMETERS;
    }
    /* add rule */
    rnp::SecurityRule newrule(ftype, fvalue, sec_level, from);
    newrule.override = rule_override;
    /* Add rule for any action */
    if (!verify_key && !verify_data) {
        ffi->profile().add_rule(newrule);
        return RNP_SUCCESS;
    }
    /* Add rule for each specified key usage */
    if (verify_key) {
        newrule.action = rnp::SecurityAction::VerifyKey;
        ffi->profile().add_rule(newrule);
    }
    if (verify_data) {
        newrule.action = rnp::SecurityAction::VerifyData;
        ffi->profile().add_rule(newrule);
    }
    return RNP_SUCCESS;
}
FFI_GUARD

static rnp::SecurityAction
get_security_action(uint32_t flags)
{
    if (flags & RNP_SECURITY_VERIFY_KEY) {
        return rnp::SecurityAction::VerifyKey;
    }
    if (flags & RNP_SECURITY_VERIFY_DATA) {
        return rnp::SecurityAction::VerifyData;
    }
    return rnp::SecurityAction::Any;
}

rnp_result_t
rnp_get_security_rule(rnp_ffi_t   ffi,
                      const char *type,
                      const char *name,
                      uint64_t    time,
                      uint32_t *  flags,
                      uint64_t *  from,
                      uint32_t *  level)
try {
    if (!ffi || !type || !name || !level) {
        return RNP_ERROR_NULL_POINTER;
    }
    /* convert values */
    rnp::FeatureType ftype;
    int              fvalue;
    if (!get_feature_sec_value(ffi, type, name, ftype, fvalue)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    /* init default rule */
    rnp::SecurityRule rule(ftype, fvalue, ffi->profile().def_level());
    /* Check whether limited usage is requested */
    auto action = get_security_action(flags ? *flags : 0);
    /* check whether rule exists */
    if (ffi->profile().has_rule(ftype, fvalue, time, action)) {
        rule = ffi->profile().get_rule(ftype, fvalue, time, action);
    }
    /* fill the results */
    if (flags) {
        *flags = rule.override ? RNP_SECURITY_OVERRIDE : 0;
        switch (rule.action) {
        case rnp::SecurityAction::VerifyKey:
            *flags |= RNP_SECURITY_VERIFY_KEY;
            break;
        case rnp::SecurityAction::VerifyData:
            *flags |= RNP_SECURITY_VERIFY_DATA;
            break;
        default:
            break;
        }
    }
    if (from) {
        *from = rule.from;
    }
    switch (rule.level) {
    case rnp::SecurityLevel::Disabled:
        *level = RNP_SECURITY_PROHIBITED;
        break;
    case rnp::SecurityLevel::Insecure:
        *level = RNP_SECURITY_INSECURE;
        break;
    case rnp::SecurityLevel::Default:
        *level = RNP_SECURITY_DEFAULT;
        break;
    default:
        /* LCOV_EXCL_START */
        FFI_LOG(ffi, "Invalid security level.");
        return RNP_ERROR_BAD_STATE;
        /* LCOV_EXCL_END */
    }
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_remove_security_rule(rnp_ffi_t   ffi,
                         const char *type,
                         const char *name,
                         uint32_t    level,
                         uint32_t    flags,
                         uint64_t    from,
                         size_t *    removed)
try {
    if (!ffi) {
        return RNP_ERROR_NULL_POINTER;
    }
    /* check flags */
    bool                remove_all = extract_flag(flags, RNP_SECURITY_REMOVE_ALL);
    bool                rule_override = extract_flag(flags, RNP_SECURITY_OVERRIDE);
    rnp::SecurityAction action = get_security_action(flags);
    extract_flag(flags, RNP_SECURITY_VERIFY_DATA | RNP_SECURITY_VERIFY_KEY);
    if (flags) {
        FFI_LOG(ffi, "Unknown flags: %" PRIu32, flags);
        return RNP_ERROR_BAD_PARAMETERS;
    }
    /* remove all rules */
    size_t rules = ffi->profile().size();
    if (!type) {
        ffi->profile().clear_rules();
        goto success;
    }
    rnp::FeatureType   ftype;
    int                fvalue;
    rnp::SecurityLevel flevel;
    if (!get_feature_sec_value(ffi, type, name, ftype, fvalue) ||
        !get_feature_sec_level(ffi, level, flevel)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    /* remove all rules for the specified type */
    if (!name) {
        ffi->profile().clear_rules(ftype);
        goto success;
    }
    if (remove_all) {
        /* remove all rules for the specified type and name */
        ffi->profile().clear_rules(ftype, fvalue);
    } else {
        /* remove specific rule */
        rnp::SecurityRule rule(ftype, fvalue, flevel, from, action);
        rule.override = rule_override;
        ffi->profile().del_rule(rule);
    }
success:
    if (removed) {
        *removed = rules - ffi->profile().size();
    }
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_request_password(rnp_ffi_t ffi, rnp_key_handle_t key, const char *context, char **password)
try {
    if (!ffi || !password || !ffi->getpasscb) {
        return RNP_ERROR_NULL_POINTER;
    }

    rnp::secure_vector<char> pass(MAX_PASSWORD_LENGTH, '\0');
    bool                     req_res =
      ffi->getpasscb(ffi, ffi->getpasscb_ctx, key, context, pass.data(), pass.size());
    if (!req_res) {
        return RNP_ERROR_GENERIC;
    }
    size_t pass_len = strlen(pass.data()) + 1;
    *password = (char *) malloc(pass_len);
    if (!*password) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    memcpy(*password, pass.data(), pass_len);
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_set_timestamp(rnp_ffi_t ffi, uint64_t time)
try {
    if (!ffi) {
        return RNP_ERROR_NULL_POINTER;
    }
    ffi->context.set_time(time);
    return RNP_SUCCESS;
}
FFI_GUARD

static rnp_result_t
load_keys_from_input(rnp_ffi_t ffi, rnp_input_t input, rnp::KeyStore *store)
{
    rnp::KeyProvider        chained(rnp_key_provider_store, store);
    const rnp::KeyProvider *key_providers[] = {&chained, &ffi->key_provider, NULL};
    const rnp::KeyProvider  key_provider(rnp_key_provider_chained, key_providers);

    if (!input->src_directory.empty()) {
        // load the keys
        store->path = input->src_directory;
        if (!store->load(&key_provider)) {
            return RNP_ERROR_BAD_FORMAT;
        }
        return RNP_SUCCESS;
    }

    // load the keys
    if (!store->load(input->src, &key_provider)) {
        return RNP_ERROR_BAD_FORMAT;
    }
    return RNP_SUCCESS;
}

static bool
key_needs_conversion(const rnp::Key *key, const rnp::KeyStore *store)
{
    auto key_format = key->format;
    auto store_format = store->format;
    /* rnp::Key->format is only ever GPG or G10.
     *
     * The key store, however, could have a format of KBX, GPG, or G10.
     * A KBX (and GPG) key store can only handle a rnp::Key with a format of GPG.
     * A G10 key store can only handle a rnp::Key with a format of G10.
     */
    // should never be the case
    assert(key_format != rnp::KeyFormat::KBX);
    // normalize the store format
    if (store_format == rnp::KeyFormat::KBX) {
        store_format = rnp::KeyFormat::GPG;
    }
    // from here, both the key and store formats can only be GPG or G10
    return key_format != store_format;
}

static rnp_result_t
do_load_keys(rnp_ffi_t ffi, rnp_input_t input, rnp::KeyFormat format, key_type_t key_type)
{
    // create a temporary key store to hold the keys
    std::unique_ptr<rnp::KeyStore> tmp_store;
    try {
        tmp_store =
          std::unique_ptr<rnp::KeyStore>(new rnp::KeyStore("", ffi->context, format));
    } catch (const std::invalid_argument &e) {
        FFI_LOG(ffi, "Failed to create key store of format: %d", (int) format);
        return RNP_ERROR_BAD_PARAMETERS;
    }

    // load keys into our temporary store
    rnp_result_t tmpret = load_keys_from_input(ffi, input, tmp_store.get());
    if (tmpret) {
        return tmpret;
    }
    // go through all the loaded keys
    for (auto &key : tmp_store->keys) {
        // check that the key is the correct type and has not already been loaded
        // add secret key part if it is and we need it
        if (key.is_secret() && ((key_type == KEY_TYPE_SECRET) || (key_type == KEY_TYPE_ANY))) {
            if (key_needs_conversion(&key, ffi->secring)) {
                FFI_LOG(ffi, "This key format conversion is not yet supported");
                return RNP_ERROR_NOT_IMPLEMENTED;
            }

            if (!ffi->secring->add_key(key)) {
                FFI_LOG(ffi, "Failed to add secret key");
                return RNP_ERROR_GENERIC;
            }
        }

        // add public key part if needed
        if ((key.format == rnp::KeyFormat::G10) ||
            ((key_type != KEY_TYPE_ANY) && (key_type != KEY_TYPE_PUBLIC))) {
            continue;
        }

        rnp::Key keycp;
        try {
            keycp = rnp::Key(key, true);
        } catch (const std::exception &e) {
            RNP_LOG("Failed to copy public key part: %s", e.what());
            return RNP_ERROR_GENERIC;
        }

        /* TODO: We could do this a few different ways. There isn't an obvious reason
         * to restrict what formats we load, so we don't necessarily need to require a
         * conversion just to load and use a G10 key when using GPG keyrings, for
         * example. We could just convert when saving.
         */

        if (key_needs_conversion(&key, ffi->pubring)) {
            FFI_LOG(ffi, "This key format conversion is not yet supported");
            return RNP_ERROR_NOT_IMPLEMENTED;
        }

        if (!ffi->pubring->add_key(keycp)) {
            FFI_LOG(ffi, "Failed to add public key");
            return RNP_ERROR_GENERIC;
        }
    }
    // success, even if we didn't actually load any
    return RNP_SUCCESS;
}

static key_type_t
flags_to_key_type(uint32_t *flags)
{
    key_type_t type = KEY_TYPE_NONE;
    // figure out what type of keys to operate on, based on flags
    if ((*flags & RNP_LOAD_SAVE_PUBLIC_KEYS) && (*flags & RNP_LOAD_SAVE_SECRET_KEYS)) {
        type = KEY_TYPE_ANY;
        extract_flag(*flags, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS);
    } else if (*flags & RNP_LOAD_SAVE_PUBLIC_KEYS) {
        type = KEY_TYPE_PUBLIC;
        extract_flag(*flags, RNP_LOAD_SAVE_PUBLIC_KEYS);
    } else if (*flags & RNP_LOAD_SAVE_SECRET_KEYS) {
        type = KEY_TYPE_SECRET;
        extract_flag(*flags, RNP_LOAD_SAVE_SECRET_KEYS);
    }
    return type;
}

rnp_result_t
rnp_load_keys(rnp_ffi_t ffi, const char *format, rnp_input_t input, uint32_t flags)
try {
    // checks
    if (!ffi || !format || !input) {
        return RNP_ERROR_NULL_POINTER;
    }
    key_type_t type = flags_to_key_type(&flags);
    if (!type) {
        FFI_LOG(ffi, "invalid flags - must have public and/or secret keys");
        return RNP_ERROR_BAD_PARAMETERS;
    }
    auto ks_format = rnp::KeyFormat::Unknown;
    if (!parse_ks_format(&ks_format, format)) {
        FFI_LOG(ffi, "invalid key store format: %s", format);
        return RNP_ERROR_BAD_PARAMETERS;
    }

    // check for any unrecognized flags (not forward-compat, but maybe still a good idea)
    if (flags) {
        FFI_LOG(ffi, "unexpected flags remaining: 0x%X", flags);
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return do_load_keys(ffi, input, ks_format, type);
}
FFI_GUARD

rnp_result_t
rnp_unload_keys(rnp_ffi_t ffi, uint32_t flags)
try {
    if (!ffi) {
        return RNP_ERROR_NULL_POINTER;
    }

    if (flags & ~(RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    if (flags & RNP_KEY_UNLOAD_PUBLIC) {
        ffi->pubring->clear();
    }
    if (flags & RNP_KEY_UNLOAD_SECRET) {
        ffi->secring->clear();
    }

    return RNP_SUCCESS;
}
FFI_GUARD

static rnp_result_t
rnp_input_dearmor_if_needed(rnp_input_t input, bool noheaders = false)
{
    if (!input) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!input->src_directory.empty()) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    bool require_armor = false;
    /* check whether we already have armored stream */
    if (input->src.type == PGP_STREAM_ARMORED) {
        if (!input->src.eof()) {
            /* be ready for the case of damaged armoring */
            return input->src.error() ? RNP_ERROR_READ : RNP_SUCCESS;
        }
        /* eof - probably next we have another armored message */
        input->src.close();
        rnp_input_st *base = (rnp_input_st *) input->app_ctx;
        *input = std::move(*base);
        delete base;
        /* we should not mix armored data with binary */
        require_armor = true;
        /* skip spaces before the next armored block or EOF */
        input->src.skip_chars("\r\n \t");
    }
    if (input->src.eof()) {
        return RNP_ERROR_EOF;
    }
    /* check whether input is armored only if base64 is not forced */
    if (!noheaders && !input->src.is_armored()) {
        return require_armor ? RNP_ERROR_BAD_FORMAT : RNP_SUCCESS;
    }

    /* Store original input in app_ctx and replace src/app_ctx with armored data */
    rnp_input_t app_ctx = new rnp_input_st();
    *app_ctx = std::move(*input);

    rnp_result_t ret = init_armored_src(&input->src, &app_ctx->src, noheaders);
    if (ret) {
        /* original src may be changed during init_armored_src call, so copy it back */
        *input = std::move(*app_ctx);
        delete app_ctx;
        return ret;
    }
    input->app_ctx = app_ctx;
    return RNP_SUCCESS;
}

static const char *
key_status_to_str(pgp_key_import_status_t status)
{
    if (status == PGP_KEY_IMPORT_STATUS_UNKNOWN) {
        return "none";
    }
    return id_str_pair::lookup(key_import_status_map, status, "none");
}

static rnp_result_t
add_key_status(json_object *           keys,
               const rnp::Key *        key,
               pgp_key_import_status_t pub,
               pgp_key_import_status_t sec)
{
    json_object *jsokey = json_object_new_object();
    if (!jsokey) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }

    if (!json_add(jsokey, "public", key_status_to_str(pub)) ||
        !json_add(jsokey, "secret", key_status_to_str(sec)) ||
        !json_add(jsokey, "fingerprint", key->fp()) || !json_array_add(keys, jsokey)) {
        /* LCOV_EXCL_START */
        json_object_put(jsokey);
        return RNP_ERROR_OUT_OF_MEMORY;
        /* LCOV_EXCL_END */
    }

    return RNP_SUCCESS;
}

rnp_result_t
rnp_import_keys(rnp_ffi_t ffi, rnp_input_t input, uint32_t flags, char **results)
try {
    if (!ffi || !input) {
        return RNP_ERROR_NULL_POINTER;
    }
    bool sec = extract_flag(flags, RNP_LOAD_SAVE_SECRET_KEYS);
    bool pub = extract_flag(flags, RNP_LOAD_SAVE_PUBLIC_KEYS);
    if (!pub && !sec) {
        FFI_LOG(ffi, "bad flags: need to specify public and/or secret keys");
        return RNP_ERROR_BAD_PARAMETERS;
    }
    bool skipbad = extract_flag(flags, RNP_LOAD_SAVE_PERMISSIVE);
    bool single = extract_flag(flags, RNP_LOAD_SAVE_SINGLE);
    bool base64 = extract_flag(flags, RNP_LOAD_SAVE_BASE64);
    if (flags) {
        FFI_LOG(ffi, "unexpected flags remaining: 0x%X", flags);
        return RNP_ERROR_BAD_PARAMETERS;
    }

    rnp_result_t  ret = RNP_ERROR_GENERIC;
    rnp::KeyStore tmp_store("", ffi->context);

    /* check whether input is base64 */
    if (base64 && input->src.is_base64()) {
        ret = rnp_input_dearmor_if_needed(input, true);
        if (ret) {
            return ret;
        }
    }

    // load keys to temporary keystore.
    if (single) {
        /* we need to init and handle dearmor on this layer since it may be used for the next
         * keys import */
        ret = rnp_input_dearmor_if_needed(input);
        if (ret == RNP_ERROR_EOF) {
            return ret;
        }
        if (ret) {
            FFI_LOG(ffi, "Failed to init/check dearmor.");
            return ret;
        }
        ret = tmp_store.load_pgp_key(input->src, skipbad);
        if (ret) {
            return ret;
        }
    } else {
        ret = tmp_store.load_pgp(input->src, skipbad);
        if (ret) {
            return ret;
        }
    }

    json_object *jsores = json_object_new_object();
    if (!jsores) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    rnp::JSONObject jsowrap(jsores);
    json_object *   jsokeys = json_object_new_array();
    if (!json_add(jsores, "keys", jsokeys)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }

    // import keys to the main keystore.
    for (auto &key : tmp_store.keys) {
        pgp_key_import_status_t pub_status = PGP_KEY_IMPORT_STATUS_UNKNOWN;
        pgp_key_import_status_t sec_status = PGP_KEY_IMPORT_STATUS_UNKNOWN;
        if (!pub && key.is_public()) {
            continue;
        }
        // if we got here then we add public key itself or public part of the secret key
        if (!ffi->pubring->import_key(key, true, &pub_status)) {
            return RNP_ERROR_BAD_PARAMETERS;
        }
        // import secret key part if available and requested
        if (sec && key.is_secret()) {
            if (!ffi->secring->import_key(key, false, &sec_status)) {
                return RNP_ERROR_BAD_PARAMETERS;
            }
            // add uids, certifications and other stuff from the public key if any
            auto *expub = ffi->pubring->get_key(key.fp());
            if (expub && !ffi->secring->import_key(*expub, true)) {
                return RNP_ERROR_BAD_PARAMETERS;
            }
        }
        // now add key fingerprint to json based on statuses
        rnp_result_t tmpret = add_key_status(jsokeys, &key, pub_status, sec_status);
        if (tmpret) {
            return tmpret;
        }
    }
    if (!results) {
        return RNP_SUCCESS;
    }
    return ret_str_value(json_object_to_json_string_ext(jsores, JSON_C_TO_STRING_PRETTY),
                         results);
}
FFI_GUARD

static const char *
sig_status_to_str(pgp_sig_import_status_t status)
{
    if (status == PGP_SIG_IMPORT_STATUS_UNKNOWN) {
        return "none";
    }
    return id_str_pair::lookup(sig_import_status_map, status, "none");
}

static rnp_result_t
add_sig_status(json_object *           sigs,
               const rnp::Key *        signer,
               pgp_sig_import_status_t pub,
               pgp_sig_import_status_t sec)
{
    json_object *jsosig = json_object_new_object();
    if (!jsosig) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }

    if (!json_add(jsosig, "public", sig_status_to_str(pub)) ||
        !json_add(jsosig, "secret", sig_status_to_str(sec))) {
        /* LCOV_EXCL_START */
        json_object_put(jsosig);
        return RNP_ERROR_OUT_OF_MEMORY;
        /* LCOV_EXCL_END */
    }

    if (signer && !json_add(jsosig, "signer fingerprint", signer->fp())) {
        /* LCOV_EXCL_START */
        json_object_put(jsosig);
        return RNP_ERROR_OUT_OF_MEMORY;
        /* LCOV_EXCL_END */
    }

    if (!json_array_add(sigs, jsosig)) {
        /* LCOV_EXCL_START */
        json_object_put(jsosig);
        return RNP_ERROR_OUT_OF_MEMORY;
        /* LCOV_EXCL_END */
    }

    return RNP_SUCCESS;
}

rnp_result_t
rnp_import_signatures(rnp_ffi_t ffi, rnp_input_t input, uint32_t flags, char **results)
try {
    if (!ffi || !input) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (flags) {
        FFI_LOG(ffi, "wrong flags: %d", (int) flags);
        return RNP_ERROR_BAD_PARAMETERS;
    }

    pgp::pkt::Signatures sigs;
    rnp_result_t         sigret = process_pgp_signatures(input->src, sigs);
    if (sigret) {
        FFI_LOG(ffi, "failed to parse signature(s)");
        return sigret;
    }

    json_object *jsores = json_object_new_object();
    if (!jsores) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    rnp::JSONObject jsowrap(jsores);
    json_object *   jsosigs = json_object_new_array();
    if (!json_add(jsores, "sigs", jsosigs)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }

    for (auto &sig : sigs) {
        pgp_sig_import_status_t pub_status = PGP_SIG_IMPORT_STATUS_UNKNOWN;
        pgp_sig_import_status_t sec_status = PGP_SIG_IMPORT_STATUS_UNKNOWN;
        auto *                  pkey = ffi->pubring->import_signature(sig, &pub_status);
        auto *                  skey = ffi->secring->import_signature(sig, &sec_status);
        sigret = add_sig_status(jsosigs, pkey ? pkey : skey, pub_status, sec_status);
        if (sigret) {
            return sigret; // LCOV_EXCL_LINE
        }
    }
    if (!results) {
        return RNP_SUCCESS;
    }
    return ret_str_value(json_object_to_json_string_ext(jsores, JSON_C_TO_STRING_PRETTY),
                         results);
}
FFI_GUARD

static bool
copy_store_keys(rnp_ffi_t ffi, rnp::KeyStore *dest, rnp::KeyStore *src)
{
    for (auto &key : src->keys) {
        if (!dest->add_key(key)) {
            FFI_LOG(ffi, "failed to add key to the store");
            return false;
        }
    }
    return true;
}

static rnp_result_t
do_save_keys(rnp_ffi_t ffi, rnp_output_t output, rnp::KeyFormat format, key_type_t key_type)
{
    // create a temporary key store to hold the keys
    rnp::KeyStore *tmp_store = nullptr;
    try {
        tmp_store = new rnp::KeyStore("", ffi->context, format);
    } catch (const std::invalid_argument &e) {
        /* LCOV_EXCL_START */
        FFI_LOG(ffi, "Failed to create key store of format: %d", (int) format);
        return RNP_ERROR_BAD_PARAMETERS;
        /* LCOV_EXCL_END */
    } catch (const std::exception &e) {
        /* LCOV_EXCL_START */
        FFI_LOG(ffi, "%s", e.what());
        return RNP_ERROR_OUT_OF_MEMORY;
        /* LCOV_EXCL_END */
    }
    std::unique_ptr<rnp::KeyStore> tmp_store_ptr(tmp_store);
    // include the public keys, if desired
    if (key_type == KEY_TYPE_PUBLIC || key_type == KEY_TYPE_ANY) {
        if (!copy_store_keys(ffi, tmp_store, ffi->pubring)) {
            return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
        }
    }
    // include the secret keys, if desired
    if (key_type == KEY_TYPE_SECRET || key_type == KEY_TYPE_ANY) {
        if (!copy_store_keys(ffi, tmp_store, ffi->secring)) {
            return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
        }
    }
    // preliminary check on the format
    for (auto &key : tmp_store->keys) {
        if (key_needs_conversion(&key, tmp_store)) {
            FFI_LOG(ffi, "This key format conversion is not yet supported");
            return RNP_ERROR_NOT_IMPLEMENTED;
        }
    }
    // write
    if (output->dst_directory) {
        tmp_store->path = output->dst_directory;
        if (!tmp_store->write()) {
            return RNP_ERROR_WRITE;
        }
        return RNP_SUCCESS;
    } else {
        if (!tmp_store->write(output->dst)) {
            return RNP_ERROR_WRITE;
        }
        dst_flush(&output->dst);
        output->keep = (output->dst.werr == RNP_SUCCESS);
        return output->dst.werr;
    }
}

rnp_result_t
rnp_save_keys(rnp_ffi_t ffi, const char *format, rnp_output_t output, uint32_t flags)
try {
    // checks
    if (!ffi || !format || !output) {
        return RNP_ERROR_NULL_POINTER;
    }
    key_type_t type = flags_to_key_type(&flags);
    if (!type) {
        FFI_LOG(ffi, "invalid flags - must have public and/or secret keys");
        return RNP_ERROR_BAD_PARAMETERS;
    }
    // check for any unrecognized flags (not forward-compat, but maybe still a good idea)
    if (flags) {
        FFI_LOG(ffi, "unexpected flags remaining: 0x%X", flags);
        return RNP_ERROR_BAD_PARAMETERS;
    }
    auto ks_format = rnp::KeyFormat::Unknown;
    if (!parse_ks_format(&ks_format, format)) {
        FFI_LOG(ffi, "unknown key store format: %s", format);
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return do_save_keys(ffi, output, ks_format, type);
}
FFI_GUARD

rnp_result_t
rnp_get_public_key_count(rnp_ffi_t ffi, size_t *count)
try {
    if (!ffi || !count) {
        return RNP_ERROR_NULL_POINTER;
    }
    *count = ffi->pubring->key_count();
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_get_secret_key_count(rnp_ffi_t ffi, size_t *count)
try {
    if (!ffi || !count) {
        return RNP_ERROR_NULL_POINTER;
    }
    *count = ffi->secring->key_count();
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_input_st::rnp_input_st() : reader(NULL), closer(NULL), app_ctx(NULL)
{
    memset(&src, 0, sizeof(src));
}

rnp_input_st &
rnp_input_st::operator=(rnp_input_st &&input)
{
    src.close();
    src = std::move(input.src);
    memset(&input.src, 0, sizeof(input.src));
    reader = input.reader;
    input.reader = NULL;
    closer = input.closer;
    input.closer = NULL;
    app_ctx = input.app_ctx;
    input.app_ctx = NULL;
    src_directory = std::move(input.src_directory);
    return *this;
}

rnp_input_st::~rnp_input_st()
{
    bool armored = src.type == PGP_STREAM_ARMORED;
    src.close();
    if (armored) {
        rnp_input_t armored = (rnp_input_t) app_ctx;
        delete armored;
        app_ctx = NULL;
    }
}

rnp_result_t
rnp_input_from_path(rnp_input_t *input, const char *path)
try {
    if (!input || !path) {
        return RNP_ERROR_NULL_POINTER;
    }
    rnp_input_st *ob = new rnp_input_st();
    struct stat   st = {0};
    if (rnp_stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        // a bit hacky, just save the directory path
        ob->src_directory = path;
        // return error on attempt to read from this source
        (void) init_null_src(&ob->src);
    } else {
        // simple input from a file
        rnp_result_t ret = init_file_src(&ob->src, path);
        if (ret) {
            delete ob;
            return ret;
        }
    }
    *input = ob;
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_input_from_stdin(rnp_input_t *input)
try {
    if (!input) {
        return RNP_ERROR_NULL_POINTER;
    }
    *input = new rnp_input_st();
    rnp_result_t ret = init_stdin_src(&(*input)->src);
    if (ret) {
        /* LCOV_EXCL_START */
        delete *input;
        *input = NULL;
        return ret;
        /* LCOV_EXCL_END */
    }
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_input_from_memory(rnp_input_t *input, const uint8_t buf[], size_t buf_len, bool do_copy)
try {
    if (!input) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!buf && buf_len) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!buf_len) {
        // prevent malloc(0)
        do_copy = false;
    }
    *input = new rnp_input_st();
    uint8_t *data = (uint8_t *) buf;
    if (do_copy) {
        data = (uint8_t *) malloc(buf_len);
        if (!data) {
            /* LCOV_EXCL_START */
            delete *input;
            *input = NULL;
            return RNP_ERROR_OUT_OF_MEMORY;
            /* LCOV_EXCL_END */
        }
        memcpy(data, buf, buf_len);
    }
    rnp_result_t ret = init_mem_src(&(*input)->src, data, buf_len, do_copy);
    if (ret) {
        /* LCOV_EXCL_START */
        if (do_copy) {
            free(data);
        }
        delete *input;
        *input = NULL;
        /* LCOV_EXCL_END */
    }
    return ret;
}
FFI_GUARD

static bool
input_reader_bounce(pgp_source_t *src, void *buf, size_t len, size_t *read)
{
    rnp_input_t input = (rnp_input_t) src->param;
    if (!input->reader) {
        return false;
    }
    return input->reader(input->app_ctx, buf, len, read);
}

static void
input_closer_bounce(pgp_source_t *src)
{
    rnp_input_t input = (rnp_input_t) src->param;
    if (input->closer) {
        input->closer(input->app_ctx);
    }
}

rnp_result_t
rnp_input_from_callback(rnp_input_t *       input,
                        rnp_input_reader_t *reader,
                        rnp_input_closer_t *closer,
                        void *              app_ctx)
try {
    // checks
    if (!input || !reader) {
        return RNP_ERROR_NULL_POINTER;
    }
    rnp_input_st *obj = new rnp_input_st();
    pgp_source_t *src = &obj->src;
    obj->reader = reader;
    obj->closer = closer;
    obj->app_ctx = app_ctx;
    if (!init_src_common(src, 0)) {
        /* LCOV_EXCL_START */
        delete obj;
        return RNP_ERROR_OUT_OF_MEMORY;
        /* LCOV_EXCL_END */
    }
    src->param = obj;
    src->raw_read = input_reader_bounce;
    src->raw_close = input_closer_bounce;
    src->type = PGP_STREAM_MEMORY;
    *input = obj;
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_input_destroy(rnp_input_t input)
try {
    delete input;
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_output_to_path(rnp_output_t *output, const char *path)
try {
    struct rnp_output_st *ob = NULL;
    struct stat           st = {0};

    if (!output || !path) {
        return RNP_ERROR_NULL_POINTER;
    }
    ob = (rnp_output_st *) calloc(1, sizeof(*ob));
    if (!ob) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    if (rnp_stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        // a bit hacky, just save the directory path
        ob->dst_directory = strdup(path);
        if (!ob->dst_directory) {
            /* LCOV_EXCL_START */
            free(ob);
            return RNP_ERROR_OUT_OF_MEMORY;
            /* LCOV_EXCL_END */
        }
    } else {
        // simple output to a file
        rnp_result_t ret = init_file_dest(&ob->dst, path, true);
        if (ret) {
            free(ob);
            return ret;
        }
    }
    *output = ob;
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_output_to_file(rnp_output_t *output, const char *path, uint32_t flags)
try {
    if (!output || !path) {
        return RNP_ERROR_NULL_POINTER;
    }
    bool overwrite = extract_flag(flags, RNP_OUTPUT_FILE_OVERWRITE);
    bool random = extract_flag(flags, RNP_OUTPUT_FILE_RANDOM);
    if (flags) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    rnp_output_t res = (rnp_output_t) calloc(1, sizeof(*res));
    if (!res) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    rnp_result_t ret = RNP_ERROR_GENERIC;
    if (random) {
        ret = init_tmpfile_dest(&res->dst, path, overwrite);
    } else {
        ret = init_file_dest(&res->dst, path, overwrite);
    }
    if (ret) {
        free(res);
        return ret;
    }
    *output = res;
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_output_to_stdout(rnp_output_t *output)
try {
    if (!output) {
        return RNP_ERROR_NULL_POINTER;
    }
    rnp_output_t res = (rnp_output_t) calloc(1, sizeof(*res));
    if (!res) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    rnp_result_t ret = init_stdout_dest(&res->dst);
    if (ret) {
        free(res);
        return ret;
    }
    *output = res;
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_output_to_memory(rnp_output_t *output, size_t max_alloc)
try {
    // checks
    if (!output) {
        return RNP_ERROR_NULL_POINTER;
    }

    *output = (rnp_output_t) calloc(1, sizeof(**output));
    if (!*output) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    rnp_result_t ret = init_mem_dest(&(*output)->dst, NULL, max_alloc);
    if (ret) {
        /* LCOV_EXCL_START */
        free(*output);
        *output = NULL;
        /* LCOV_EXCL_END */
    }
    return ret;
}
FFI_GUARD

rnp_result_t
rnp_output_to_armor(rnp_output_t base, rnp_output_t *output, const char *type)
try {
    if (!base || !output) {
        return RNP_ERROR_NULL_POINTER;
    }
    pgp_armored_msg_t msgtype = PGP_ARMORED_MESSAGE;
    if (type) {
        msgtype = static_cast<pgp_armored_msg_t>(
          id_str_pair::lookup(armor_type_map, type, PGP_ARMORED_UNKNOWN));
        if (msgtype == PGP_ARMORED_UNKNOWN) {
            RNP_LOG("Unsupported armor type: %s", type);
            return RNP_ERROR_BAD_PARAMETERS;
        }
    }
    *output = (rnp_output_t) calloc(1, sizeof(**output));
    if (!*output) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    rnp_result_t ret = init_armored_dst(&(*output)->dst, &base->dst, msgtype);
    if (ret) {
        /* LCOV_EXCL_START */
        free(*output);
        *output = NULL;
        return ret;
        /* LCOV_EXCL_END */
    }
    (*output)->app_ctx = base;
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_output_memory_get_buf(rnp_output_t output, uint8_t **buf, size_t *len, bool do_copy)
try {
    if (!output || !buf || !len) {
        return RNP_ERROR_NULL_POINTER;
    }

    *len = output->dst.writeb;
    *buf = (uint8_t *) mem_dest_get_memory(&output->dst);
    if (!*buf) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    if (do_copy) {
        uint8_t *tmp_buf = *buf;
        *buf = (uint8_t *) malloc(*len);
        if (!*buf) {
            return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
        }
        memcpy(*buf, tmp_buf, *len);
    }
    return RNP_SUCCESS;
}
FFI_GUARD

static rnp_result_t
output_writer_bounce(pgp_dest_t *dst, const void *buf, size_t len)
{
    rnp_output_t output = (rnp_output_t) dst->param;
    if (!output->writer) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!output->writer(output->app_ctx, buf, len)) {
        return RNP_ERROR_WRITE;
    }
    return RNP_SUCCESS;
}

static void
output_closer_bounce(pgp_dest_t *dst, bool discard)
{
    rnp_output_t output = (rnp_output_t) dst->param;
    if (output->closer) {
        output->closer(output->app_ctx, discard);
    }
}

rnp_result_t
rnp_output_to_null(rnp_output_t *output)
try {
    // checks
    if (!output) {
        return RNP_ERROR_NULL_POINTER;
    }

    *output = (rnp_output_t) calloc(1, sizeof(**output));
    if (!*output) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    rnp_result_t ret = init_null_dest(&(*output)->dst);
    if (ret) {
        /* LCOV_EXCL_START */
        free(*output);
        *output = NULL;
        /* LCOV_EXCL_END */
    }
    return ret;
}
FFI_GUARD

rnp_result_t
rnp_output_write(rnp_output_t output, const void *data, size_t size, size_t *written)
try {
    if (!output || (!data && size)) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!data && !size) {
        if (written) {
            *written = 0;
        }
        return RNP_SUCCESS;
    }
    size_t old = output->dst.writeb + output->dst.clen;
    dst_write(&output->dst, data, size);
    if (!output->dst.werr && written) {
        *written = output->dst.writeb + output->dst.clen - old;
    }
    output->keep = !output->dst.werr;
    return output->dst.werr;
}
FFI_GUARD

rnp_result_t
rnp_output_to_callback(rnp_output_t *       output,
                       rnp_output_writer_t *writer,
                       rnp_output_closer_t *closer,
                       void *               app_ctx)
try {
    // checks
    if (!output || !writer) {
        return RNP_ERROR_NULL_POINTER;
    }

    *output = (rnp_output_t) calloc(1, sizeof(**output));
    if (!*output) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    (*output)->writer = writer;
    (*output)->closer = closer;
    (*output)->app_ctx = app_ctx;

    pgp_dest_t *dst = &(*output)->dst;
    dst->write = output_writer_bounce;
    dst->close = output_closer_bounce;
    dst->param = *output;
    dst->type = PGP_STREAM_MEMORY;
    dst->writeb = 0;
    dst->werr = RNP_SUCCESS;
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_output_finish(rnp_output_t output)
try {
    if (!output) {
        return RNP_ERROR_NULL_POINTER;
    }
    return dst_finish(&output->dst);
}
FFI_GUARD

rnp_result_t
rnp_output_destroy(rnp_output_t output)
try {
    if (output) {
        if (output->dst.type == PGP_STREAM_ARMORED) {
            ((rnp_output_t) output->app_ctx)->keep = output->keep;
        }
        dst_close(&output->dst, !output->keep);
        free(output->dst_directory);
        free(output);
    }
    return RNP_SUCCESS;
}
FFI_GUARD

static rnp_result_t
rnp_op_add_signature(rnp_ffi_t                 ffi,
                     rnp_op_sign_signatures_t &signatures,
                     rnp_key_handle_t          key,
                     rnp_ctx_t &               ctx,
                     rnp_op_sign_signature_t * sig)
{
    if (!key) {
        return RNP_ERROR_NULL_POINTER;
    }

    auto *signkey =
      find_suitable_key(PGP_OP_SIGN, get_key_require_secret(key), &key->ffi->key_provider);
    if (!signkey) {
        return RNP_ERROR_NO_SUITABLE_KEY;
    }

    try {
        signatures.emplace_back();
    } catch (const std::exception &e) {
        /* LCOV_EXCL_START */
        FFI_LOG(ffi, "%s", e.what());
        return RNP_ERROR_BAD_PARAMETERS;
        /* LCOV_EXCL_END */
    }
    rnp_op_sign_signature_t newsig = &signatures.back();
    newsig->signer.key = signkey;
    /* set default create/expire times */
    newsig->signer.sigcreate = ctx.sigcreate;
    newsig->signer.sigexpire = ctx.sigexpire;
    newsig->ffi = ffi;

    if (sig) {
        *sig = newsig;
    }
    return RNP_SUCCESS;
}

static rnp_result_t
rnp_op_set_armor(rnp_ctx_t &ctx, bool armored)
{
    ctx.armor = armored;
    return RNP_SUCCESS;
}

static rnp_result_t
rnp_op_set_compression(rnp_ffi_t ffi, rnp_ctx_t &ctx, const char *compression, int level)
{
    if (!compression) {
        return RNP_ERROR_NULL_POINTER;
    }

    pgp_compression_type_t zalg = PGP_C_UNKNOWN;
    if (!str_to_compression_alg(compression, &zalg)) {
        FFI_LOG(ffi, "Invalid compression: %s", compression);
        return RNP_ERROR_BAD_PARAMETERS;
    }
    ctx.zalg = (int) zalg;
    ctx.zlevel = level;
    return RNP_SUCCESS;
}

static rnp_result_t
rnp_op_set_hash(rnp_ffi_t ffi, rnp_ctx_t &ctx, const char *hash)
{
    if (!hash) {
        return RNP_ERROR_NULL_POINTER;
    }

    if (!str_to_hash_alg(hash, &ctx.halg)) {
        FFI_LOG(ffi, "Invalid hash: %s", hash);
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return RNP_SUCCESS;
}

static rnp_result_t
rnp_op_set_creation_time(rnp_ctx_t &ctx, uint32_t create)
{
    ctx.sigcreate = create;
    return RNP_SUCCESS;
}

static rnp_result_t
rnp_op_set_expiration_time(rnp_ctx_t &ctx, uint32_t expire)
{
    ctx.sigexpire = expire;
    return RNP_SUCCESS;
}

static rnp_result_t
rnp_op_set_flags(rnp_ffi_t ffi, rnp_ctx_t &ctx, uint32_t flags)
{
    ctx.no_wrap = extract_flag(flags, RNP_ENCRYPT_NOWRAP);
    if (flags) {
        FFI_LOG(ffi, "Unknown operation flags: %x", flags);
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return RNP_SUCCESS;
}

static rnp_result_t
rnp_op_set_file_name(rnp_ctx_t &ctx, const char *filename)
{
    ctx.filename = filename ? filename : "";
    return RNP_SUCCESS;
}

static rnp_result_t
rnp_op_set_file_mtime(rnp_ctx_t &ctx, uint32_t mtime)
{
    ctx.filemtime = mtime;
    return RNP_SUCCESS;
}

rnp_result_t
rnp_op_encrypt_create(rnp_op_encrypt_t *op,
                      rnp_ffi_t         ffi,
                      rnp_input_t       input,
                      rnp_output_t      output)
try {
    // checks
    if (!op || !ffi || !input || !output) {
        return RNP_ERROR_NULL_POINTER;
    }

    *op = new rnp_op_encrypt_st(ffi, input, output);
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_encrypt_add_recipient(rnp_op_encrypt_t op, rnp_key_handle_t handle)
try {
    // checks
    if (!op || !handle) {
        return RNP_ERROR_NULL_POINTER;
    }

#if defined(ENABLE_PQC)
    bool prefer_pqc = op->rnpctx.pref_pqc_enc_subkey;
#else
    bool prefer_pqc = false;
#endif
    auto *key = find_suitable_key(PGP_OP_ENCRYPT,
                                  get_key_prefer_public(handle),
                                  &handle->ffi->key_provider,
                                  false,
                                  prefer_pqc);
    if (!key) {
        return RNP_ERROR_NO_SUITABLE_KEY;
    }
    op->rnpctx.recipients.push_back(key);
    return RNP_SUCCESS;
}
FFI_GUARD

#if defined(RNP_EXPERIMENTAL_CRYPTO_REFRESH)
rnp_result_t
rnp_op_encrypt_enable_pkesk_v6(rnp_op_encrypt_t op)
try {
    if (!op) {
        return RNP_ERROR_NULL_POINTER;
    }

    op->rnpctx.enable_pkesk_v6 = true;
    return RNP_SUCCESS;
}
FFI_GUARD
#endif

#if defined(RNP_EXPERIMENTAL_PQC)
rnp_result_t
rnp_op_encrypt_prefer_pqc_enc_subkey(rnp_op_encrypt_t op)
try {
    if (!op) {
        return RNP_ERROR_NULL_POINTER;
    }

    op->rnpctx.pref_pqc_enc_subkey = true;
    return RNP_SUCCESS;
}
FFI_GUARD
#endif

rnp_result_t
rnp_op_encrypt_add_signature(rnp_op_encrypt_t         op,
                             rnp_key_handle_t         key,
                             rnp_op_sign_signature_t *sig)
try {
    if (!op) {
        return RNP_ERROR_NULL_POINTER;
    }
    return rnp_op_add_signature(op->ffi, op->signatures, key, op->rnpctx, sig);
}
FFI_GUARD

rnp_result_t
rnp_op_encrypt_set_hash(rnp_op_encrypt_t op, const char *hash)
try {
    if (!op) {
        return RNP_ERROR_NULL_POINTER;
    }
    return rnp_op_set_hash(op->ffi, op->rnpctx, hash);
}
FFI_GUARD

rnp_result_t
rnp_op_encrypt_set_creation_time(rnp_op_encrypt_t op, uint32_t create)
try {
    if (!op) {
        return RNP_ERROR_NULL_POINTER;
    }
    return rnp_op_set_creation_time(op->rnpctx, create);
}
FFI_GUARD

rnp_result_t
rnp_op_encrypt_set_expiration_time(rnp_op_encrypt_t op, uint32_t expire)
try {
    if (!op) {
        return RNP_ERROR_NULL_POINTER;
    }
    return rnp_op_set_expiration_time(op->rnpctx, expire);
}
FFI_GUARD

rnp_result_t
rnp_op_encrypt_add_password(rnp_op_encrypt_t op,
                            const char *     password,
                            const char *     s2k_hash,
                            size_t           iterations,
                            const char *     s2k_cipher)
try {
    // checks
    if (!op) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (password && !*password) {
        // no blank passwords
        FFI_LOG(op->ffi, "Blank password");
        return RNP_ERROR_BAD_PARAMETERS;
    }

    // set some defaults
    if (!s2k_hash) {
        s2k_hash = DEFAULT_HASH_ALG;
    }
    if (!s2k_cipher) {
        s2k_cipher = DEFAULT_SYMM_ALG;
    }
    // parse
    pgp_hash_alg_t hash_alg = PGP_HASH_UNKNOWN;
    if (!str_to_hash_alg(s2k_hash, &hash_alg)) {
        FFI_LOG(op->ffi, "Invalid hash: %s", s2k_hash);
        return RNP_ERROR_BAD_PARAMETERS;
    }
    pgp_symm_alg_t symm_alg = PGP_SA_UNKNOWN;
    if (!str_to_cipher(s2k_cipher, &symm_alg)) {
        FFI_LOG(op->ffi, "Invalid cipher: %s", s2k_cipher);
        return RNP_ERROR_BAD_PARAMETERS;
    }
    rnp::secure_vector<char> ask_pass(MAX_PASSWORD_LENGTH, '\0');
    if (!password) {
        pgp_password_ctx_t pswdctx(PGP_OP_ENCRYPT_SYM);
        if (!pgp_request_password(
              &op->ffi->pass_provider, &pswdctx, ask_pass.data(), ask_pass.size())) {
            return RNP_ERROR_BAD_PASSWORD;
        }
        password = ask_pass.data();
    }
    return op->rnpctx.add_encryption_password(password, hash_alg, symm_alg, iterations);
}
FFI_GUARD

rnp_result_t
rnp_op_encrypt_set_armor(rnp_op_encrypt_t op, bool armored)
try {
    // checks
    if (!op) {
        return RNP_ERROR_NULL_POINTER;
    }
    return rnp_op_set_armor(op->rnpctx, armored);
}
FFI_GUARD

static bool
rnp_check_old_ciphers(rnp_ffi_t ffi, const char *cipher)
{
    uint32_t security_level = 0;

    if (rnp_get_security_rule(ffi,
                              RNP_FEATURE_SYMM_ALG,
                              cipher,
                              ffi->context.time(),
                              NULL,
                              NULL,
                              &security_level)) {
        FFI_LOG(ffi, "Failed to get security rules for cipher algorithm \'%s\'!", cipher);
        return false;
    }

    if (security_level < RNP_SECURITY_DEFAULT) {
        FFI_LOG(ffi, "Cipher algorithm \'%s\' is cryptographically weak!", cipher);
        return false;
    }
    /* TODO: check other weak algorithms and key sizes */
    return true;
}

rnp_result_t
rnp_op_encrypt_set_cipher(rnp_op_encrypt_t op, const char *cipher)
try {
    // checks
    if (!op || !cipher) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!rnp_check_old_ciphers(op->ffi, cipher)) {
        FFI_LOG(op->ffi, "Deprecated cipher: %s", cipher);
        return RNP_ERROR_BAD_PARAMETERS;
    }
    if (!str_to_cipher(cipher, &op->rnpctx.ealg)) {
        FFI_LOG(op->ffi, "Invalid cipher: %s", cipher);
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_encrypt_set_aead(rnp_op_encrypt_t op, const char *alg)
try {
    // checks
    if (!op || !alg) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!str_to_aead_alg(alg, &op->rnpctx.aalg)) {
        FFI_LOG(op->ffi, "Invalid AEAD algorithm: %s", alg);
        return RNP_ERROR_BAD_PARAMETERS;
    }
    if (op->rnpctx.aalg == PGP_AEAD_EAX) {
        /* LCOV_EXCL_START */
        /* The define below is not reported as covered however codecov reports error */
        FFI_LOG(op->ffi, "Warning! EAX mode is deprecated and should not be used.");
        /* LCOV_EXCL_END */
    }
#ifdef ENABLE_CRYPTO_REFRESH
    if (op->rnpctx.aalg == PGP_AEAD_NONE && op->rnpctx.enable_pkesk_v6) {
        FFI_LOG(op->ffi,
                "Setting AEAD algorithm to PGP_AEAD_NONE (%s) would contradict the previously "
                "enabled PKESKv6 setting",
                alg);
        return RNP_ERROR_BAD_PARAMETERS;
    }
#endif
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_encrypt_set_aead_bits(rnp_op_encrypt_t op, int bits)
try {
    if (!op) {
        return RNP_ERROR_NULL_POINTER;
    }
    if ((bits < 0) || (bits > 16)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    op->rnpctx.abits = bits;
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_encrypt_set_compression(rnp_op_encrypt_t op, const char *compression, int level)
try {
    // checks
    if (!op) {
        return RNP_ERROR_NULL_POINTER;
    }
    return rnp_op_set_compression(op->ffi, op->rnpctx, compression, level);
}
FFI_GUARD

rnp_result_t
rnp_op_encrypt_set_flags(rnp_op_encrypt_t op, uint32_t flags)
try {
    // checks
    if (!op) {
        return RNP_ERROR_NULL_POINTER;
    }
    return rnp_op_set_flags(op->ffi, op->rnpctx, flags);
}
FFI_GUARD

rnp_result_t
rnp_op_encrypt_set_file_name(rnp_op_encrypt_t op, const char *filename)
try {
    if (!op) {
        return RNP_ERROR_NULL_POINTER;
    }
    return rnp_op_set_file_name(op->rnpctx, filename);
}
FFI_GUARD

rnp_result_t
rnp_op_encrypt_set_file_mtime(rnp_op_encrypt_t op, uint32_t mtime)
try {
    if (!op) {
        return RNP_ERROR_NULL_POINTER;
    }
    return rnp_op_set_file_mtime(op->rnpctx, mtime);
}
FFI_GUARD

static rnp_result_t
rnp_op_add_signatures(rnp_op_sign_signatures_t &opsigs, rnp_ctx_t &ctx)
{
    for (auto &sig : opsigs) {
        if (!sig.signer.key) {
            return RNP_ERROR_NO_SUITABLE_KEY;
        }

        rnp_signer_info_t sinfo = sig.signer;
        if (!sig.hash_set) {
            sinfo.halg = ctx.halg;
        }
        if (!sig.expiry_set) {
            sinfo.sigexpire = ctx.sigexpire;
        }
        if (!sig.create_set) {
            sinfo.sigcreate = ctx.sigcreate;
        }
        ctx.signers.push_back(sinfo);
    }
    return RNP_SUCCESS;
}

rnp_result_t
rnp_op_encrypt_execute(rnp_op_encrypt_t op)
try {
    // checks
    if (!op || !op->input || !op->output) {
        return RNP_ERROR_NULL_POINTER;
    }

    // set the default hash alg if none was specified
    if (!op->rnpctx.halg) {
        op->rnpctx.halg = DEFAULT_PGP_HASH_ALG;
    }

    rnp_result_t ret = RNP_ERROR_GENERIC;
    if (!op->signatures.empty() && (ret = rnp_op_add_signatures(op->signatures, op->rnpctx))) {
        return ret;
    }
    ret = rnp_encrypt_sign_src(op->rnpctx, op->input->src, op->output->dst);

    dst_flush(&op->output->dst);
    op->output->keep = ret == RNP_SUCCESS;
    op->input = NULL;
    op->output = NULL;
    return ret;
}
FFI_GUARD

rnp_result_t
rnp_op_encrypt_destroy(rnp_op_encrypt_t op)
try {
    delete op;
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_sign_create(rnp_op_sign_t *op, rnp_ffi_t ffi, rnp_input_t input, rnp_output_t output)
try {
    // checks
    if (!op || !ffi || !input || !output) {
        return RNP_ERROR_NULL_POINTER;
    }

    *op = new rnp_op_sign_st(ffi, input, output);
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_sign_cleartext_create(rnp_op_sign_t *op,
                             rnp_ffi_t      ffi,
                             rnp_input_t    input,
                             rnp_output_t   output)
try {
    rnp_result_t res = rnp_op_sign_create(op, ffi, input, output);
    if (!res) {
        (*op)->rnpctx.clearsign = true;
    }
    return res;
}
FFI_GUARD

rnp_result_t
rnp_op_sign_detached_create(rnp_op_sign_t *op,
                            rnp_ffi_t      ffi,
                            rnp_input_t    input,
                            rnp_output_t   signature)
try {
    rnp_result_t res = rnp_op_sign_create(op, ffi, input, signature);
    if (!res) {
        (*op)->rnpctx.detached = true;
    }
    return res;
}
FFI_GUARD

rnp_result_t
rnp_op_sign_add_signature(rnp_op_sign_t op, rnp_key_handle_t key, rnp_op_sign_signature_t *sig)
try {
    if (!op) {
        return RNP_ERROR_NULL_POINTER;
    }
    return rnp_op_add_signature(op->ffi, op->signatures, key, op->rnpctx, sig);
}
FFI_GUARD

rnp_result_t
rnp_op_sign_signature_set_hash(rnp_op_sign_signature_t sig, const char *hash)
try {
    if (!sig || !hash) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!str_to_hash_alg(hash, &sig->signer.halg)) {
        FFI_LOG(sig->ffi, "Invalid hash: %s", hash);
        return RNP_ERROR_BAD_PARAMETERS;
    }
    sig->hash_set = true;
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_sign_signature_set_creation_time(rnp_op_sign_signature_t sig, uint32_t create)
try {
    if (!sig) {
        return RNP_ERROR_NULL_POINTER;
    }
    sig->signer.sigcreate = create;
    sig->create_set = true;
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_sign_signature_set_expiration_time(rnp_op_sign_signature_t sig, uint32_t expires)
try {
    if (!sig) {
        return RNP_ERROR_NULL_POINTER;
    }
    sig->signer.sigexpire = expires;
    sig->expiry_set = true;
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_sign_set_armor(rnp_op_sign_t op, bool armored)
try {
    if (!op) {
        return RNP_ERROR_NULL_POINTER;
    }
    return rnp_op_set_armor(op->rnpctx, armored);
}
FFI_GUARD

rnp_result_t
rnp_op_sign_set_compression(rnp_op_sign_t op, const char *compression, int level)
try {
    if (!op) {
        return RNP_ERROR_NULL_POINTER;
    }
    return rnp_op_set_compression(op->ffi, op->rnpctx, compression, level);
}
FFI_GUARD

rnp_result_t
rnp_op_sign_set_hash(rnp_op_sign_t op, const char *hash)
try {
    if (!op) {
        return RNP_ERROR_NULL_POINTER;
    }
    return rnp_op_set_hash(op->ffi, op->rnpctx, hash);
}
FFI_GUARD

rnp_result_t
rnp_op_sign_set_creation_time(rnp_op_sign_t op, uint32_t create)
try {
    if (!op) {
        return RNP_ERROR_NULL_POINTER;
    }
    return rnp_op_set_creation_time(op->rnpctx, create);
}
FFI_GUARD

rnp_result_t
rnp_op_sign_set_expiration_time(rnp_op_sign_t op, uint32_t expire)
try {
    if (!op) {
        return RNP_ERROR_NULL_POINTER;
    }
    return rnp_op_set_expiration_time(op->rnpctx, expire);
}
FFI_GUARD

rnp_result_t
rnp_op_sign_set_file_name(rnp_op_sign_t op, const char *filename)
try {
    if (!op) {
        return RNP_ERROR_NULL_POINTER;
    }
    return rnp_op_set_file_name(op->rnpctx, filename);
}
FFI_GUARD

rnp_result_t
rnp_op_sign_set_file_mtime(rnp_op_sign_t op, uint32_t mtime)
try {
    if (!op) {
        return RNP_ERROR_NULL_POINTER;
    }
    return rnp_op_set_file_mtime(op->rnpctx, mtime);
}
FFI_GUARD

rnp_result_t
rnp_op_sign_execute(rnp_op_sign_t op)
try {
    // checks
    if (!op || !op->input || !op->output) {
        return RNP_ERROR_NULL_POINTER;
    }

    // set the default hash alg if none was specified
    if (!op->rnpctx.halg) {
        op->rnpctx.halg = DEFAULT_PGP_HASH_ALG;
    }
    rnp_result_t ret = RNP_ERROR_GENERIC;
    if ((ret = rnp_op_add_signatures(op->signatures, op->rnpctx))) {
        return ret;
    }
    ret = rnp_sign_src(op->rnpctx, op->input->src, op->output->dst);

    dst_flush(&op->output->dst);
    op->output->keep = ret == RNP_SUCCESS;
    op->input = NULL;
    op->output = NULL;
    return ret;
}
FFI_GUARD

rnp_result_t
rnp_op_sign_destroy(rnp_op_sign_t op)
try {
    delete op;
    return RNP_SUCCESS;
}
FFI_GUARD

static void
rnp_op_verify_on_signatures(const std::vector<rnp::SignatureInfo> &sigs, void *param)
{
    rnp_op_verify_t op = (rnp_op_verify_t) param;

    try {
        /* in case we have multiple signed layers */
        op->signatures_.resize(sigs.size());

        size_t i = 0;
        for (const auto &sinfo : sigs) {
            auto &res = op->signatures_[i++];
            /* sinfo.sig may be NULL */
            if (sinfo.sig) {
                res.sig_pkt = *sinfo.sig;
            }
            res.validity = sinfo.validity;
            res.ffi = op->ffi;

            /* signature is valid */
            if (res.validity.valid()) {
                res.verify_status = RNP_SUCCESS;
                continue;
            }
            /* failed to parse signature */
            if (res.validity.unknown()) {
                res.verify_status = RNP_ERROR_SIGNATURE_UNKNOWN;
                continue;
            }
            /* expired signature */
            if (res.validity.expired()) {
                res.verify_status = RNP_ERROR_SIGNATURE_EXPIRED;
                continue;
            }
            /* signer's key not found */
            if (res.validity.no_signer()) {
                res.verify_status = RNP_ERROR_KEY_NOT_FOUND;
                continue;
            }
            /* other reasons */
            res.verify_status = RNP_ERROR_SIGNATURE_INVALID;
        }
    } catch (const std::exception &e) {
        FFI_LOG(op->ffi, "%s", e.what()); // LCOV_EXCL_LINE
    }
}

static bool
rnp_verify_src_provider(pgp_parse_handler_t *handler, pgp_source_t *src)
{
    /* this one is called only when input for detached signature is needed */
    rnp_op_verify_t op = (rnp_op_verify_t) handler->param;
    if (!op->detached_input) {
        return false;
    }
    *src = op->detached_input->src;
    /* we should give ownership on src to caller */
    memset(&op->detached_input->src, 0, sizeof(op->detached_input->src));
    return true;
};

static bool
rnp_verify_dest_provider(pgp_parse_handler_t *    handler,
                         pgp_dest_t **            dst,
                         bool *                   closedst,
                         const pgp_literal_hdr_t *lithdr)
{
    rnp_op_verify_t op = (rnp_op_verify_t) handler->param;
    if (!op->output) {
        return false;
    }
    *dst = &(op->output->dst);
    *closedst = false;
    op->lithdr = lithdr ? *lithdr : pgp_literal_hdr_t();
    return true;
}

static void
recipient_handle_from_pk_sesskey(rnp_recipient_handle_st &handle,
                                 const pgp_pk_sesskey_t & sesskey)
{
    handle.keyid = sesskey.key_id;
    handle.palg = sesskey.alg;
}

static void
symenc_handle_from_sk_sesskey(rnp_symenc_handle_st &handle, const pgp_sk_sesskey_t &sesskey)
{
    handle.alg = sesskey.alg;
    handle.halg = sesskey.s2k.hash_alg;
    handle.s2k_type = sesskey.s2k.specifier;
    if (sesskey.s2k.specifier == PGP_S2KS_ITERATED_AND_SALTED) {
        handle.iterations = pgp_s2k_decode_iterations(sesskey.s2k.iterations);
    } else {
        handle.iterations = 1;
    }
    handle.aalg = sesskey.aalg;
}

static void
rnp_verify_on_recipients(const std::vector<pgp_pk_sesskey_t> &recipients,
                         const std::vector<pgp_sk_sesskey_t> &passwords,
                         void *                               param)
{
    rnp_op_verify_t op = (rnp_op_verify_t) param;
    /* store only top-level encrypted stream recipients info for now */
    if (op->encrypted_layers++) {
        return;
    }
    if (!recipients.empty()) {
        op->recipients.resize(recipients.size());
        for (size_t i = 0; i < recipients.size(); i++) {
            recipient_handle_from_pk_sesskey(op->recipients[i], recipients[i]);
        }
    }
    if (!passwords.empty()) {
        op->symencs.resize(passwords.size());
        for (size_t i = 0; i < passwords.size(); i++) {
            symenc_handle_from_sk_sesskey(op->symencs[i], passwords[i]);
        }
    }
}

static void
rnp_verify_on_decryption_start(pgp_pk_sesskey_t *pubenc, pgp_sk_sesskey_t *symenc, void *param)
{
    rnp_op_verify_t op = (rnp_op_verify_t) param;
    /* store only top-level encrypted stream info */
    if (op->encrypted_layers > 1) {
        return;
    }
    if (pubenc) {
        op->used_recipient = new (std::nothrow) rnp_recipient_handle_st();
        if (!op->used_recipient) {
            return;
        }
        recipient_handle_from_pk_sesskey(*op->used_recipient, *pubenc);
        return;
    }
    if (symenc) {
        op->used_symenc = new (std::nothrow) rnp_symenc_handle_st();
        if (!op->used_symenc) {
            return;
        }
        symenc_handle_from_sk_sesskey(*op->used_symenc, *symenc);
        return;
    }
    FFI_LOG(op->ffi, "Warning! Both pubenc and symenc are NULL.");
}

static void
rnp_verify_on_decryption_info(bool mdc, pgp_aead_alg_t aead, pgp_symm_alg_t salg, void *param)
{
    rnp_op_verify_t op = (rnp_op_verify_t) param;
    /* store only top-level encrypted stream info for now */
    if (op->encrypted_layers > 1) {
        return;
    }
    op->mdc = mdc;
    op->aead = aead;
    op->salg = salg;
    op->encrypted = true;
}

static void
rnp_verify_on_decryption_done(bool validated, void *param)
{
    rnp_op_verify_t op = (rnp_op_verify_t) param;
    if (op->encrypted_layers > 1) {
        return;
    }
    op->validated = validated;
}

rnp_result_t
rnp_op_verify_create(rnp_op_verify_t *op,
                     rnp_ffi_t        ffi,
                     rnp_input_t      input,
                     rnp_output_t     output)
try {
    if (!op || !ffi || !input || !output) {
        return RNP_ERROR_NULL_POINTER;
    }

    *op = new rnp_op_verify_st(ffi, input, output);
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_verify_detached_create(rnp_op_verify_t *op,
                              rnp_ffi_t        ffi,
                              rnp_input_t      input,
                              rnp_input_t      signature)
try {
    if (!op || !ffi || !input || !signature) {
        return RNP_ERROR_NULL_POINTER;
    }

    *op = new rnp_op_verify_st(ffi, input, signature);
    return RNP_SUCCESS;
}
FFI_GUARD

static rnp::Key *
ffi_decrypt_key_provider(const pgp_key_request_ctx_t *ctx, void *userdata)
{
    rnp_decryption_kp_param_t *kparam = (rnp_decryption_kp_param_t *) userdata;

    auto ffi = kparam->op->ffi;
    bool hidden = false;
    if (ctx->secret && (ctx->search.type() == rnp::KeySearch::Type::KeyID)) {
        auto ksearch = dynamic_cast<const rnp::KeyIDSearch *>(&ctx->search);
        assert(ksearch != nullptr);
        hidden = ksearch && ksearch->hidden();
    }
    /* default to the FFI key provider if not hidden keyid request */
    if (!hidden) {
        return ffi->key_provider.callback(ctx, ffi->key_provider.userdata);
    }
    /* if we had hidden request and last key is NULL then key search was exhausted */
    if (!kparam->op->allow_hidden || (kparam->has_hidden && !kparam->last)) {
        return NULL;
    }
    /* inform user about the hidden recipient before searching through the loaded keys */
    if (!kparam->has_hidden) {
        call_key_callback(ffi, ctx->search, ctx->secret);
    }
    kparam->has_hidden = true;
    kparam->last = find_key(ffi, ctx->search, true, true, kparam->last);
    return kparam->last;
}

rnp_result_t
rnp_op_verify_set_flags(rnp_op_verify_t op, uint32_t flags)
try {
    if (!op) {
        return RNP_ERROR_NULL_POINTER;
    }
    /* Allow to decrypt without valid signatures */
    op->ignore_sigs = extract_flag(flags, RNP_VERIFY_IGNORE_SIGS_ON_DECRYPT);
    /* Strict mode: require all signatures to be valid */
    op->require_all_sigs = extract_flag(flags, RNP_VERIFY_REQUIRE_ALL_SIGS);
    /* Allow hidden recipients if any */
    op->allow_hidden = extract_flag(flags, RNP_VERIFY_ALLOW_HIDDEN_RECIPIENT);

    if (flags) {
        FFI_LOG(op->ffi, "Unknown operation flags: %x", flags);
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_verify_execute(rnp_op_verify_t op)
try {
    if (!op) {
        return RNP_ERROR_NULL_POINTER;
    }

    pgp_parse_handler_t handler;

    handler.password_provider = &op->ffi->pass_provider;

    rnp_decryption_kp_param_t kparam(op);
    rnp::KeyProvider          kprov(ffi_decrypt_key_provider, &kparam);

    handler.key_provider = &kprov;
    handler.on_signatures = rnp_op_verify_on_signatures;
    handler.src_provider = rnp_verify_src_provider;
    handler.dest_provider = rnp_verify_dest_provider;
    handler.on_recipients = rnp_verify_on_recipients;
    handler.on_decryption_start = rnp_verify_on_decryption_start;
    handler.on_decryption_info = rnp_verify_on_decryption_info;
    handler.on_decryption_done = rnp_verify_on_decryption_done;
    handler.param = op;
    handler.ctx = &op->rnpctx;

    rnp_result_t ret = process_pgp_source(&handler, op->input->src);
    /* Allow to decrypt data ignoring the signatures check if requested */
    if (op->ignore_sigs && op->validated && (ret == RNP_ERROR_SIGNATURE_INVALID)) {
        ret = RNP_SUCCESS;
    }
    /* Allow to require all signatures be valid */
    if (op->require_all_sigs && !ret) {
        for (auto &sig : op->signatures_) {
            if (sig.verify_status) {
                ret = RNP_ERROR_SIGNATURE_INVALID;
                break;
            }
        }
    }
    if (op->output) {
        dst_flush(&op->output->dst);
        op->output->keep = ret == RNP_SUCCESS;
    }
    return ret;
}
FFI_GUARD

rnp_result_t
rnp_op_verify_get_signature_count(rnp_op_verify_t op, size_t *count)
try {
    if (!op || !count) {
        return RNP_ERROR_NULL_POINTER;
    }

    *count = op->signatures_.size();
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_verify_get_signature_at(rnp_op_verify_t op, size_t idx, rnp_op_verify_signature_t *sig)
try {
    if (!op || !sig) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (idx >= op->signatures_.size()) {
        FFI_LOG(op->ffi, "Invalid signature index: %zu", idx);
        return RNP_ERROR_BAD_PARAMETERS;
    }
    *sig = &op->signatures_[idx];
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_verify_get_file_info(rnp_op_verify_t op, char **filename, uint32_t *mtime)
try {
    if (!op) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (mtime) {
        *mtime = op->lithdr.timestamp;
    }
    if (!filename) {
        return RNP_SUCCESS;
    }
    const std::string fname(op->lithdr.fname, op->lithdr.fname_len);
    return ret_str_value(fname.c_str(), filename);
}
FFI_GUARD

rnp_result_t
rnp_op_verify_get_format(rnp_op_verify_t op, char *format)
try {
    if (!op || !format) {
        return RNP_ERROR_NULL_POINTER;
    }
    *format = (char) op->lithdr.format;
    return RNP_SUCCESS;
}
FFI_GUARD

static const char *
get_protection_mode(rnp_op_verify_t op)
{
    if (!op->encrypted) {
        return "none";
    }
    if (op->mdc) {
        return "cfb-mdc";
    }
    if (op->aead == PGP_AEAD_NONE) {
        return "cfb";
    }
    switch (op->aead) {
    case PGP_AEAD_EAX:
        return "aead-eax";
    case PGP_AEAD_OCB:
        return "aead-ocb";
    default:
        return "aead-unknown";
    }
}

static const char *
get_protection_cipher(rnp_op_verify_t op)
{
    if (!op->encrypted) {
        return "none";
    }
    return id_str_pair::lookup(symm_alg_map, op->salg);
}

rnp_result_t
rnp_op_verify_get_protection_info(rnp_op_verify_t op, char **mode, char **cipher, bool *valid)
try {
    if (!op || (!mode && !cipher && !valid)) {
        return RNP_ERROR_NULL_POINTER;
    }

    if (mode) {
        *mode = strdup(get_protection_mode(op));
        if (!*mode) {
            return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
        }
    }
    if (cipher) {
        *cipher = strdup(get_protection_cipher(op));
        if (!*cipher) {
            return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
        }
    }
    if (valid) {
        *valid = op->validated;
    }
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_verify_get_recipient_count(rnp_op_verify_t op, size_t *count)
try {
    if (!op || !count) {
        return RNP_ERROR_NULL_POINTER;
    }
    *count = op->recipients.size();
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_verify_get_used_recipient(rnp_op_verify_t op, rnp_recipient_handle_t *recipient)
try {
    if (!op || !recipient) {
        return RNP_ERROR_NULL_POINTER;
    }
    *recipient = op->used_recipient;
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_verify_get_recipient_at(rnp_op_verify_t         op,
                               size_t                  idx,
                               rnp_recipient_handle_t *recipient)
try {
    if (!op || !recipient) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (idx >= op->recipients.size()) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    *recipient = &op->recipients[idx];
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_recipient_get_keyid(rnp_recipient_handle_t recipient, char **keyid)
try {
    if (!recipient || !keyid) {
        return RNP_ERROR_NULL_POINTER;
    }
    return ret_keyid(recipient->keyid, keyid);
}
FFI_GUARD

rnp_result_t
rnp_recipient_get_alg(rnp_recipient_handle_t recipient, char **alg)
try {
    if (!recipient || !alg) {
        return RNP_ERROR_NULL_POINTER;
    }
    return get_map_value(pubkey_alg_map, recipient->palg, alg);
}
FFI_GUARD

rnp_result_t
rnp_op_verify_get_symenc_count(rnp_op_verify_t op, size_t *count)
try {
    if (!op || !count) {
        return RNP_ERROR_NULL_POINTER;
    }
    *count = op->symencs.size();
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_verify_get_used_symenc(rnp_op_verify_t op, rnp_symenc_handle_t *symenc)
try {
    if (!op || !symenc) {
        return RNP_ERROR_NULL_POINTER;
    }
    *symenc = op->used_symenc;
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_verify_get_symenc_at(rnp_op_verify_t op, size_t idx, rnp_symenc_handle_t *symenc)
try {
    if (!op || !symenc) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (idx >= op->symencs.size()) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    *symenc = &op->symencs[idx];
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_symenc_get_cipher(rnp_symenc_handle_t symenc, char **cipher)
try {
    if (!symenc || !cipher) {
        return RNP_ERROR_NULL_POINTER;
    }
    return get_map_value(symm_alg_map, symenc->alg, cipher);
}
FFI_GUARD

rnp_result_t
rnp_symenc_get_aead_alg(rnp_symenc_handle_t symenc, char **alg)
try {
    if (!symenc || !alg) {
        return RNP_ERROR_NULL_POINTER;
    }
    return get_map_value(aead_alg_map, symenc->aalg, alg);
}
FFI_GUARD

rnp_result_t
rnp_symenc_get_hash_alg(rnp_symenc_handle_t symenc, char **alg)
try {
    if (!symenc || !alg) {
        return RNP_ERROR_NULL_POINTER;
    }
    return get_map_value(hash_alg_map, symenc->halg, alg);
}
FFI_GUARD

rnp_result_t
rnp_symenc_get_s2k_type(rnp_symenc_handle_t symenc, char **type)
try {
    if (!symenc || !type) {
        return RNP_ERROR_NULL_POINTER;
    }
    return get_map_value(s2k_type_map, symenc->s2k_type, type);
}
FFI_GUARD

rnp_result_t
rnp_symenc_get_s2k_iterations(rnp_symenc_handle_t symenc, uint32_t *iterations)
try {
    if (!symenc || !iterations) {
        return RNP_ERROR_NULL_POINTER;
    }
    *iterations = symenc->iterations;
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_verify_destroy(rnp_op_verify_t op)
try {
    delete op;
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_op_verify_st::~rnp_op_verify_st()
{
    delete used_recipient;
    delete used_symenc;
}

rnp_result_t
rnp_op_verify_signature_get_status(rnp_op_verify_signature_t sig)
try {
    if (!sig) {
        return RNP_ERROR_NULL_POINTER;
    }
    return sig->verify_status;
}
FFI_GUARD

rnp_result_t
rnp_op_verify_signature_get_handle(rnp_op_verify_signature_t sig,
                                   rnp_signature_handle_t *  handle)
try {
    if (!sig || !handle) {
        return RNP_ERROR_NULL_POINTER;
    }

    std::unique_ptr<rnp::Signature> subsig(new rnp::Signature(sig->sig_pkt));
    *handle = new rnp_signature_handle_st(sig->ffi, nullptr, nullptr, true);
    (*handle)->sig = subsig.release();
    (*handle)->sig->validity = sig->validity;

    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_verify_signature_get_hash(rnp_op_verify_signature_t sig, char **hash)
try {
    if (!sig || !hash) {
        return RNP_ERROR_NULL_POINTER;
    }
    return get_map_value(hash_alg_map, sig->sig_pkt.halg, hash);
}
FFI_GUARD

static rnp_key_handle_t
get_signer_handle(rnp_ffi_t ffi, const pgp::pkt::Signature &sig)
{
    // search the stores
    auto *pub = ffi->pubring->get_signer(sig);
    auto *sec = ffi->secring->get_signer(sig);
    if (!pub && !sec) {
        return nullptr;
    }
    return new rnp_key_handle_st(ffi, pub, sec);
}

rnp_result_t
rnp_op_verify_signature_get_key(rnp_op_verify_signature_t sig, rnp_key_handle_t *key)
try {
    if (!sig || !key) {
        return RNP_ERROR_NULL_POINTER;
    }
    *key = get_signer_handle(sig->ffi, sig->sig_pkt);
    return *key ? RNP_SUCCESS : RNP_ERROR_KEY_NOT_FOUND;
}
FFI_GUARD

rnp_result_t
rnp_op_verify_signature_get_times(rnp_op_verify_signature_t sig,
                                  uint32_t *                create,
                                  uint32_t *                expires)
try {
    if (create) {
        *create = sig->sig_pkt.creation();
    }
    if (expires) {
        *expires = sig->sig_pkt.expiration();
    }

    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_decrypt(rnp_ffi_t ffi, rnp_input_t input, rnp_output_t output)
try {
    // checks
    if (!ffi || !input || !output) {
        return RNP_ERROR_NULL_POINTER;
    }

    rnp_op_verify_t op = NULL;
    rnp_result_t    ret = rnp_op_verify_create(&op, ffi, input, output);
    if (ret) {
        return ret; // LCOV_EXCL_LINE
    }
    ret = rnp_op_verify_set_flags(op, RNP_VERIFY_IGNORE_SIGS_ON_DECRYPT);
    if (!ret) {
        ret = rnp_op_verify_execute(op);
    }
    rnp_op_verify_destroy(op);
    return ret;
}
FFI_GUARD

static rnp_result_t
rnp_locate_key_int(rnp_ffi_t             ffi,
                   const rnp::KeySearch &locator,
                   rnp_key_handle_t *    handle,
                   bool                  require_secret = false)
{
    // search pubring
    auto *pub = ffi->pubring->search(locator);
    // search secring
    auto *sec = ffi->secring->search(locator);

    if (require_secret && !sec) {
        *handle = nullptr;
        return RNP_SUCCESS;
    }

    if (pub || sec) {
        *handle = new rnp_key_handle_st(ffi, pub, sec);
    } else {
        *handle = nullptr;
    }
    return RNP_SUCCESS;
}

rnp_result_t
rnp_locate_key(rnp_ffi_t         ffi,
               const char *      identifier_type,
               const char *      identifier,
               rnp_key_handle_t *handle)
try {
    // checks
    if (!ffi || !identifier_type || !identifier || !handle) {
        return RNP_ERROR_NULL_POINTER;
    }

    // figure out the identifier type
    auto search = rnp::KeySearch::create(identifier_type, identifier);
    if (!search) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return rnp_locate_key_int(ffi, *search, handle);
}
FFI_GUARD

rnp_result_t
rnp_key_export(rnp_key_handle_t handle, rnp_output_t output, uint32_t flags)
try {
    pgp_dest_t *dst = NULL;
    pgp_dest_t  armordst = {};

    // checks
    if (!handle || !output) {
        return RNP_ERROR_NULL_POINTER;
    }
    dst = &output->dst;
    if ((flags & RNP_KEY_EXPORT_PUBLIC) && (flags & RNP_KEY_EXPORT_SECRET)) {
        FFI_LOG(handle->ffi, "Invalid export flags, select only public or secret, not both.");
        return RNP_ERROR_BAD_PARAMETERS;
    }

    // handle flags
    bool           armored = extract_flag(flags, RNP_KEY_EXPORT_ARMORED);
    rnp::Key *     key = nullptr;
    rnp::KeyStore *store = nullptr;
    if (flags & RNP_KEY_EXPORT_PUBLIC) {
        extract_flag(flags, RNP_KEY_EXPORT_PUBLIC);
        key = get_key_require_public(handle);
        store = handle->ffi->pubring;
    } else if (flags & RNP_KEY_EXPORT_SECRET) {
        extract_flag(flags, RNP_KEY_EXPORT_SECRET);
        key = get_key_require_secret(handle);
        store = handle->ffi->secring;
    } else {
        FFI_LOG(handle->ffi, "must specify public or secret key for export");
        return RNP_ERROR_BAD_PARAMETERS;
    }
    bool export_subs = extract_flag(flags, RNP_KEY_EXPORT_SUBKEYS);
    // check for any unrecognized flags
    if (flags) {
        FFI_LOG(handle->ffi, "unrecognized flags remaining: 0x%X", flags);
        return RNP_ERROR_BAD_PARAMETERS;
    }
    // make sure we found our key
    if (!key) {
        FFI_LOG(handle->ffi, "no suitable key found");
        return RNP_ERROR_NO_SUITABLE_KEY;
    }
    // only PGP packets supported for now
    if (key->format != rnp::KeyFormat::GPG && key->format != rnp::KeyFormat::KBX) {
        return RNP_ERROR_NOT_IMPLEMENTED;
    }
    if (armored) {
        auto msgtype = key->is_secret() ? PGP_ARMORED_SECRET_KEY : PGP_ARMORED_PUBLIC_KEY;
        rnp_result_t res = init_armored_dst(&armordst, &output->dst, msgtype);
        if (res) {
            return res;
        }
        dst = &armordst;
    }
    // write
    if (key->is_primary()) {
        // primary key, write just the primary or primary and all subkeys
        key->write_xfer(*dst, export_subs ? store : NULL);
        if (dst->werr) {
            return RNP_ERROR_WRITE;
        }
    } else {
        // subkeys flag is only valid for primary
        if (export_subs) {
            FFI_LOG(handle->ffi, "export with subkeys requested but key is not primary");
            return RNP_ERROR_BAD_PARAMETERS;
        }
        // subkey, write the primary + this subkey only
        auto *primary = store->primary_key(*key);
        if (!primary) {
            // shouldn't happen
            return RNP_ERROR_GENERIC;
        }
        primary->write_xfer(*dst);
        if (dst->werr) {
            return RNP_ERROR_WRITE;
        }
        key->write_xfer(*dst);
        if (dst->werr) {
            return RNP_ERROR_WRITE;
        }
    }
    if (armored) {
        dst_finish(&armordst);
        dst_close(&armordst, false);
    }
    output->keep = true;
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_export_autocrypt(rnp_key_handle_t key,
                         rnp_key_handle_t subkey,
                         const char *     uid,
                         rnp_output_t     output,
                         uint32_t         flags)
try {
    if (!key || !output) {
        return RNP_ERROR_NULL_POINTER;
    }
    bool base64 = extract_flag(flags, RNP_KEY_EXPORT_BASE64);
    if (flags) {
        FFI_LOG(key->ffi, "Unknown flags remaining: 0x%X", flags);
        return RNP_ERROR_BAD_PARAMETERS;
    }
    /* Get the primary key */
    auto *primary = get_key_prefer_public(key);
    if (!primary || !primary->is_primary() || !primary->usable_for(PGP_OP_VERIFY)) {
        FFI_LOG(key->ffi, "No valid signing primary key");
        return RNP_ERROR_BAD_PARAMETERS;
    }
    /* Get encrypting subkey */
    auto *sub = subkey ?
                  get_key_prefer_public(subkey) :
                  find_suitable_key(PGP_OP_ENCRYPT, primary, &key->ffi->key_provider, true);
    if (!sub || sub->is_primary() || !sub->usable_for(PGP_OP_ENCRYPT)) {
        FFI_LOG(key->ffi, "No encrypting subkey");
        return RNP_ERROR_KEY_NOT_FOUND;
    }
    /* Get userid */
    size_t uididx = primary->uid_count();
    if (uid) {
        for (size_t idx = 0; idx < primary->uid_count(); idx++) {
            if (primary->get_uid(idx).str == uid) {
                uididx = idx;
                break;
            }
        }
    } else {
        if (primary->uid_count() > 1) {
            FFI_LOG(key->ffi, "Ambiguous userid");
            return RNP_ERROR_BAD_PARAMETERS;
        }
        uididx = 0;
    }
    if (uididx >= primary->uid_count()) {
        FFI_LOG(key->ffi, "Userid not found");
        return RNP_ERROR_BAD_PARAMETERS;
    }

    /* Check whether base64 is requested */
    bool res = false;
    if (base64) {
        rnp::ArmoredDest armor(output->dst, PGP_ARMORED_BASE64);
        res = primary->write_autocrypt(armor.dst(), *sub, uididx);
    } else {
        res = primary->write_autocrypt(output->dst, *sub, uididx);
    }
    return res ? RNP_SUCCESS : RNP_ERROR_BAD_PARAMETERS;
}
FFI_GUARD

static rnp::Key *
rnp_key_get_revoker(rnp_key_handle_t key)
{
    auto *exkey = get_key_prefer_public(key);
    if (!exkey) {
        return NULL;
    }
    if (exkey->is_subkey()) {
        return key->ffi->secring->primary_key(*exkey);
    }
    // TODO: search through revocation key subpackets as well
    return get_key_require_secret(key);
}

static bool
fill_revocation_reason(rnp_ffi_t        ffi,
                       rnp::Revocation &revinfo,
                       const char *     code,
                       const char *     reason)
{
    revinfo = {};
    if (code && !str_to_revocation_type(code, &revinfo.code)) {
        FFI_LOG(ffi, "Wrong revocation code: %s", code);
        return false;
    }
    if (revinfo.code > PGP_REVOCATION_RETIRED) {
        FFI_LOG(ffi, "Wrong key revocation code: %d", (int) revinfo.code);
        return false;
    }
    if (reason) {
        revinfo.reason = reason;
    }
    return true;
}

static rnp_result_t
rnp_key_get_revocation(rnp_ffi_t            ffi,
                       rnp::Key *           key,
                       rnp::Key *           revoker,
                       const char *         hash,
                       const char *         code,
                       const char *         reason,
                       pgp::pkt::Signature &sig)
{
    if (!hash) {
        hash = DEFAULT_HASH_ALG;
    }
    pgp_hash_alg_t halg = PGP_HASH_UNKNOWN;
    if (!str_to_hash_alg(hash, &halg)) {
        FFI_LOG(ffi, "Unknown hash algorithm: %s", hash);
        return RNP_ERROR_BAD_PARAMETERS;
    }
    rnp::Revocation revinfo;
    if (!fill_revocation_reason(ffi, revinfo, code, reason)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    /* unlock the secret key if needed */
    rnp::KeyLocker revlock(*revoker);
    if (revoker->is_locked() && !revoker->unlock(ffi->pass_provider)) {
        FFI_LOG(ffi, "Failed to unlock secret key");
        return RNP_ERROR_BAD_PASSWORD;
    }
    try {
        revoker->gen_revocation(revinfo, halg, key->pkt(), sig, ffi->context);
    } catch (const std::exception &e) {
        FFI_LOG(ffi, "Failed to generate revocation signature: %s", e.what());
        return RNP_ERROR_BAD_STATE;
    }
    return RNP_SUCCESS;
}

rnp_result_t
rnp_key_export_revocation(rnp_key_handle_t key,
                          rnp_output_t     output,
                          uint32_t         flags,
                          const char *     hash,
                          const char *     code,
                          const char *     reason)
try {
    if (!key || !key->ffi || !output) {
        return RNP_ERROR_NULL_POINTER;
    }
    bool need_armor = extract_flag(flags, RNP_KEY_EXPORT_ARMORED);
    if (flags) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    auto *exkey = get_key_prefer_public(key);
    if (!exkey || !exkey->is_primary()) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    auto *revoker = rnp_key_get_revoker(key);
    if (!revoker) {
        FFI_LOG(key->ffi, "Revoker secret key not found");
        return RNP_ERROR_BAD_PARAMETERS;
    }

    pgp::pkt::Signature sig;
    rnp_result_t        ret =
      rnp_key_get_revocation(key->ffi, exkey, revoker, hash, code, reason, sig);
    if (ret) {
        return ret;
    }

    if (need_armor) {
        rnp::ArmoredDest armor(output->dst, PGP_ARMORED_PUBLIC_KEY);
        sig.write(armor.dst());
        ret = armor.werr();
        dst_flush(&armor.dst());
    } else {
        sig.write(output->dst);
        ret = output->dst.werr;
        dst_flush(&output->dst);
    }
    output->keep = !ret;
    return ret;
}
FFI_GUARD

rnp_result_t
rnp_key_revoke(
  rnp_key_handle_t key, uint32_t flags, const char *hash, const char *code, const char *reason)
try {
    if (!key || !key->ffi) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (flags) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    auto *exkey = get_key_prefer_public(key);
    if (!exkey) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    auto *revoker = rnp_key_get_revoker(key);
    if (!revoker) {
        FFI_LOG(key->ffi, "Revoker secret key not found");
        return RNP_ERROR_BAD_PARAMETERS;
    }

    pgp::pkt::Signature sig;
    rnp_result_t        ret =
      rnp_key_get_revocation(key->ffi, exkey, revoker, hash, code, reason, sig);
    if (ret) {
        return ret;
    }
    pgp_sig_import_status_t pub_status = PGP_SIG_IMPORT_STATUS_UNKNOWN_KEY;
    pgp_sig_import_status_t sec_status = PGP_SIG_IMPORT_STATUS_UNKNOWN_KEY;
    if (key->pub) {
        pub_status = key->ffi->pubring->import_signature(*key->pub, sig);
    }
    if (key->sec) {
        sec_status = key->ffi->secring->import_signature(*key->sec, sig);
    }

    if ((pub_status == PGP_SIG_IMPORT_STATUS_UNKNOWN) ||
        (sec_status == PGP_SIG_IMPORT_STATUS_UNKNOWN)) {
        return RNP_ERROR_GENERIC;
    }
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_25519_bits_tweaked(rnp_key_handle_t key, bool *result)
try {
    if (!key || !result) {
        return RNP_ERROR_NULL_POINTER;
    }
    auto *seckey = get_key_require_secret(key);
    if (!seckey || seckey->is_locked() || (seckey->alg() != PGP_PKA_ECDH) ||
        (seckey->curve() != PGP_CURVE_25519)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    auto material = dynamic_cast<const pgp::ECDHKeyMaterial *>(seckey->material());
    if (!material) {
        return RNP_ERROR_BAD_STATE; // LCOV_EXCL_LINE
    }
    *result = material->x25519_bits_tweaked();
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_25519_bits_tweak(rnp_key_handle_t key)
try {
    if (!key) {
        return RNP_ERROR_NULL_POINTER;
    }
    auto *seckey = get_key_require_secret(key);
    if (!seckey || seckey->is_protected() || (seckey->alg() != PGP_PKA_ECDH) ||
        (seckey->curve() != PGP_CURVE_25519)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    auto material = dynamic_cast<pgp::ECDHKeyMaterial *>(seckey->material());
    if (!material || !material->x25519_tweak_bits()) {
        FFI_LOG(key->ffi, "Failed to tweak 25519 key bits.");
        return RNP_ERROR_BAD_STATE;
    }
    if (!seckey->write_sec_rawpkt(seckey->pkt(), "", key->ffi->context)) {
        FFI_LOG(key->ffi, "Failed to update rawpkt.");
        return RNP_ERROR_BAD_STATE;
    }
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_remove(rnp_key_handle_t key, uint32_t flags)
try {
    if (!key || !key->ffi) {
        return RNP_ERROR_NULL_POINTER;
    }
    bool pub = extract_flag(flags, RNP_KEY_REMOVE_PUBLIC);
    bool sec = extract_flag(flags, RNP_KEY_REMOVE_SECRET);
    bool sub = extract_flag(flags, RNP_KEY_REMOVE_SUBKEYS);
    if (flags) {
        FFI_LOG(key->ffi, "Unknown flags: %" PRIu32, flags);
        return RNP_ERROR_BAD_PARAMETERS;
    }
    if (!pub && !sec) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    if (sub && get_key_prefer_public(key)->is_subkey()) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    if (pub) {
        if (!key->ffi->pubring || !key->pub) {
            return RNP_ERROR_BAD_PARAMETERS;
        }
        if (!key->ffi->pubring->remove_key(*key->pub, sub)) {
            return RNP_ERROR_KEY_NOT_FOUND;
        }
        key->pub = NULL;
    }
    if (sec) {
        if (!key->ffi->secring || !key->sec) {
            return RNP_ERROR_BAD_PARAMETERS;
        }
        if (!key->ffi->secring->remove_key(*key->sec, sub)) {
            return RNP_ERROR_KEY_NOT_FOUND;
        }
        key->sec = NULL;
    }
    return RNP_SUCCESS;
}
FFI_GUARD

static void
report_signature_removal(rnp_ffi_t             ffi,
                         const rnp::Key &      key,
                         rnp_key_signatures_cb sigcb,
                         void *                app_ctx,
                         rnp::Signature &      keysig,
                         bool &                remove)
{
    if (!sigcb) {
        return;
    }

    rnp_signature_handle_t sig = nullptr;
    try {
        sig = new rnp_signature_handle_st(ffi, &key, &keysig, false);
    } catch (const std::exception &e) {
        /* LCOV_EXCL_START */
        FFI_LOG(ffi, "Signature handle allocation failed: %s", e.what());
        return;
        /* LCOV_EXCL_END */
    }

    uint32_t action = remove ? RNP_KEY_SIGNATURE_REMOVE : RNP_KEY_SIGNATURE_KEEP;
    sigcb(ffi, app_ctx, sig, &action);
    switch (action) {
    case RNP_KEY_SIGNATURE_REMOVE:
        remove = true;
        break;
    case RNP_KEY_SIGNATURE_KEEP:
        remove = false;
        break;
    default:
        FFI_LOG(ffi, "Invalid signature removal action: %" PRIu32, action);
        break;
    }
    rnp_signature_handle_destroy(sig);
}

static bool
signature_needs_removal(rnp_ffi_t       ffi,
                        const rnp::Key &key,
                        rnp::Signature &sig,
                        uint32_t        flags)
{
    /* quick check for non-self signatures */
    bool nonself = flags & RNP_KEY_SIGNATURE_NON_SELF_SIG;
    if (nonself && key.is_primary() && !key.is_signer(sig)) {
        return true;
    }
    if (nonself && key.is_subkey()) {
        auto *primary = ffi->pubring->primary_key(key);
        if (primary && !primary->is_signer(sig)) {
            return true;
        }
    }
    /* unknown signer */
    auto *signer = ffi->pubring->get_signer(sig.sig, &ffi->key_provider);
    if (!signer && (flags & RNP_KEY_SIGNATURE_UNKNOWN_KEY)) {
        return true;
    }
    /* validate signature if didn't */
    if (signer && !sig.validity.validated()) {
        signer->validate_sig(key, sig, ffi->context);
    }
    /* we cannot check for invalid/expired if sig was not validated */
    if (!sig.validity.validated()) {
        return false;
    }
    if ((flags & RNP_KEY_SIGNATURE_INVALID) && !sig.validity.valid()) {
        return true;
    }
    return false;
}

static void
remove_key_signatures(rnp_ffi_t             ffi,
                      rnp::Key &            pub,
                      rnp::Key *            sec,
                      uint32_t              flags,
                      rnp_key_signatures_cb sigcb,
                      void *                app_ctx)
{
    pgp::SigIDs sigs;

    for (size_t idx = 0; idx < pub.sig_count(); idx++) {
        auto &sig = pub.get_sig(idx);
        bool  remove = signature_needs_removal(ffi, pub, sig, flags);
        report_signature_removal(ffi, pub, sigcb, app_ctx, sig, remove);
        if (remove) {
            sigs.push_back(sig.sigid);
        }
    }
    size_t deleted = pub.del_sigs(sigs);
    if (deleted != sigs.size()) {
        FFI_LOG(ffi, "Invalid deleted sigs count: %zu instead of %zu.", deleted, sigs.size());
    }
    /* delete from the secret key if any */
    if (sec && (sec != &pub)) {
        sec->del_sigs(sigs);
    }
}

rnp_result_t
rnp_key_remove_signatures(rnp_key_handle_t      handle,
                          uint32_t              flags,
                          rnp_key_signatures_cb sigcb,
                          void *                app_ctx)
try {
    if (!handle) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!flags && !sigcb) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    uint32_t origflags = flags;
    extract_flag(flags,
                 RNP_KEY_SIGNATURE_INVALID | RNP_KEY_SIGNATURE_NON_SELF_SIG |
                   RNP_KEY_SIGNATURE_UNKNOWN_KEY);
    if (flags) {
        FFI_LOG(handle->ffi, "Invalid flags: %" PRIu32, flags);
        return RNP_ERROR_BAD_PARAMETERS;
    }
    flags = origflags;

    auto *key = get_key_prefer_public(handle);
    if (!key) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    /* process key itself */
    auto *sec = get_key_require_secret(handle);
    remove_key_signatures(handle->ffi, *key, sec, flags, sigcb, app_ctx);

    /* process subkeys */
    for (size_t idx = 0; key->is_primary() && (idx < key->subkey_count()); idx++) {
        auto *sub = handle->ffi->pubring->get_subkey(*key, idx);
        if (!sub) {
            FFI_LOG(handle->ffi, "Failed to get subkey at idx %zu.", idx);
            continue;
        }
        auto *subsec = handle->ffi->secring->get_key(sub->fp());
        remove_key_signatures(handle->ffi, *sub, subsec, flags, sigcb, app_ctx);
    }
    /* revalidate key/subkey */
    key->revalidate(*handle->ffi->pubring);
    if (sec) {
        sec->revalidate(*handle->ffi->secring);
    }
    return RNP_SUCCESS;
}
FFI_GUARD

static bool
pk_alg_allows_custom_curve(pgp_pubkey_alg_t pkalg)
{
    switch (pkalg) {
    case PGP_PKA_ECDH:
    case PGP_PKA_ECDSA:
    case PGP_PKA_SM2:
        return true;
    default:
        return false;
    }
}

static bool
parse_preferences(json_object *jso, rnp::UserPrefs &prefs)
{
    /* Preferred hashes */
    std::vector<std::string> strs;
    if (json_get_str_arr(jso, "hashes", strs)) {
        for (auto &str : strs) {
            pgp_hash_alg_t hash_alg = PGP_HASH_UNKNOWN;
            if (!str_to_hash_alg(str.c_str(), &hash_alg)) {
                return false;
            }
            prefs.add_hash_alg(hash_alg);
        }
    }
    /* Preferred symmetric algorithms */
    if (json_get_str_arr(jso, "ciphers", strs)) {
        for (auto &str : strs) {
            pgp_symm_alg_t symm_alg = PGP_SA_UNKNOWN;
            if (!str_to_cipher(str.c_str(), &symm_alg)) {
                return false;
            }
            prefs.add_symm_alg(symm_alg);
        }
    }
    /* Preferred compression algorithms */
    if (json_get_str_arr(jso, "compression", strs)) {
        for (auto &str : strs) {
            pgp_compression_type_t z_alg = PGP_C_UNKNOWN;
            if (!str_to_compression_alg(str.c_str(), &z_alg)) {
                return false;
            }
            prefs.add_z_alg(z_alg);
        }
    }
    /* Preferred key server */
    std::string key_server;
    if (json_get_str(jso, "key server", key_server)) {
        prefs.key_server = std::move(key_server);
    }
    /* Do not allow extra unknown keys */
    return !json_object_object_length(jso);
}

static std::unique_ptr<rnp::KeygenParams>
parse_keygen_params(rnp_ffi_t ffi, json_object *jso)
{
    /* Type */
    std::string      str;
    pgp_pubkey_alg_t alg = PGP_PKA_RSA;
    if (json_get_str(jso, "type", str) && !str_to_pubkey_alg(str.c_str(), &alg)) {
        return nullptr;
    }
    std::unique_ptr<rnp::KeygenParams> params(new rnp::KeygenParams(alg, ffi->context));
    /* Length */
    int bits = 0;
    if (json_get_int(jso, "length", bits)) {
        auto bit_params = dynamic_cast<pgp::BitsKeyParams *>(&params->key_params());
        if (!bit_params) {
            return nullptr;
        }
        bit_params->set_bits(bits);
    }
    /* Curve */
    if (json_get_str(jso, "curve", str)) {
        if (!pk_alg_allows_custom_curve(params->alg())) {
            return nullptr;
        }
        auto        ecc_params = dynamic_cast<pgp::ECCKeyParams *>(&params->key_params());
        pgp_curve_t curve = PGP_CURVE_UNKNOWN;
        if (!ecc_params || !curve_str_to_type(str.c_str(), &curve)) {
            return nullptr;
        }
        ecc_params->set_curve(curve);
    }
    /* Hash algorithm */
    if (json_get_str(jso, "hash", str)) {
        pgp_hash_alg_t hash = PGP_HASH_UNKNOWN;
        if (!str_to_hash_alg(str.c_str(), &hash)) {
            return nullptr;
        }
        params->set_hash(hash);
    }
    return params;
}

static bool
parse_protection(json_object *jso, rnp_key_protection_params_t &protection)
{
    /* Cipher */
    std::string str;
    if (json_get_str(jso, "cipher", str)) {
        if (!str_to_cipher(str.c_str(), &protection.symm_alg)) {
            return false;
        }
    }
    /* Mode */
    if (json_get_str(jso, "mode", str)) {
        if (!str_to_cipher_mode(str.c_str(), &protection.cipher_mode)) {
            return false;
        }
    }
    /* Iterations */
    int iterations = 0;
    if (json_get_int(jso, "iterations", iterations)) {
        protection.iterations = iterations;
    }
    /* Hash algorithm */
    if (json_get_str(jso, "hash", str)) {
        if (!str_to_hash_alg(str.c_str(), &protection.hash_alg)) {
            return false;
        }
    }
    /* Do not allow extra unknown keys */
    return !json_object_object_length(jso);
}

static bool
parse_keygen_common_fields(json_object *                jso,
                           uint8_t &                    usage,
                           uint32_t &                   expiry,
                           rnp_key_protection_params_t &prot)
{
    /* Key/subkey usage flags */
    std::string              str;
    std::vector<std::string> strs;
    if (json_get_str(jso, "usage", str)) {
        strs.push_back(std::move(str));
    } else {
        json_get_str_arr(jso, "usage", strs);
    }
    for (auto &st : strs) {
        uint8_t flag = 0;
        if (!str_to_key_flag(st.c_str(), &flag) || (usage & flag)) {
            return false;
        }
        usage |= flag;
    }
    /* Key/subkey expiration */
    uint64_t keyexp = 0;
    if (json_get_uint64(jso, "expiration", keyexp)) {
        keyexp = expiry;
    }
    /* Protection */
    auto obj = json_get_obj(jso, "protection");
    if (obj) {
        if (!parse_protection(obj, prot)) {
            return false;
        }
        json_object_object_del(jso, "protection");
    }
    return true;
}

static std::unique_ptr<rnp::KeygenParams>
parse_keygen_primary(rnp_ffi_t                    ffi,
                     json_object *                jso,
                     rnp::CertParams &            cert,
                     rnp_key_protection_params_t &prot)
{
    /* Parse keygen params first */
    auto params = parse_keygen_params(ffi, jso);
    if (!params) {
        return nullptr;
    }
    /* Parse common key/subkey fields */
    if (!parse_keygen_common_fields(jso, cert.flags, cert.key_expiration, prot)) {
        return nullptr;
    }
    /* UserID */
    std::string str;
    if (json_get_str(jso, "userid", str)) {
        if (str.size() > MAX_ID_LENGTH) {
            return nullptr;
        }
        cert.userid = std::move(str);
    }
    /* Preferences */
    auto obj = json_get_obj(jso, "preferences");
    if (obj) {
        if (!parse_preferences(obj, cert.prefs)) {
            return nullptr;
        }
        json_object_object_del(jso, "preferences");
    }
    /* Do not allow unknown extra fields */
    return json_object_object_length(jso) ? nullptr : std::move(params);
}

static std::unique_ptr<rnp::KeygenParams>
parse_keygen_sub(rnp_ffi_t                    ffi,
                 json_object *                jso,
                 rnp::BindingParams &         binding,
                 rnp_key_protection_params_t &prot)
{
    /* Parse keygen params first */
    auto params = parse_keygen_params(ffi, jso);
    if (!params) {
        return nullptr;
    }
    /* Parse common with primary key fields */
    if (!parse_keygen_common_fields(jso, binding.flags, binding.key_expiration, prot)) {
        return nullptr;
    }
    /* Do not allow unknown extra fields */
    return json_object_object_length(jso) ? nullptr : std::move(params);
}

static bool
gen_json_grips(char **result, const rnp::Key *primary, const rnp::Key *sub)
{
    if (!result) {
        return true;
    }

    json_object *jso = json_object_new_object();
    if (!jso) {
        return false;
    }
    rnp::JSONObject jsowrap(jso);

    char grip[PGP_KEY_GRIP_SIZE * 2 + 1];
    if (primary) {
        json_object *jsoprimary = json_object_new_object();
        if (!jsoprimary) {
            return false;
        }
        if (!json_add(jso, "primary", jsoprimary) ||
            !rnp::hex_encode(
              primary->grip().data(), primary->grip().size(), grip, sizeof(grip)) ||
            !json_add(jsoprimary, "grip", grip)) {
            return false;
        }
    }
    if (sub) {
        json_object *jsosub = json_object_new_object();
        if (!jsosub || !json_add(jso, "sub", jsosub) ||
            !rnp::hex_encode(sub->grip().data(), sub->grip().size(), grip, sizeof(grip)) ||
            !json_add(jsosub, "grip", grip)) {
            return false;
        }
    }
    *result = strdup(json_object_to_json_string_ext(jso, JSON_C_TO_STRING_PRETTY));
    return *result;
}

static rnp_result_t
gen_json_primary_key(rnp_ffi_t                    ffi,
                     json_object *                jsoparams,
                     rnp_key_protection_params_t &prot,
                     pgp::Fingerprint &           fp,
                     bool                         protect)
{
    rnp::CertParams cert;
    cert.key_expiration = DEFAULT_KEY_EXPIRATION;

    auto keygen = parse_keygen_primary(ffi, jsoparams, cert, prot);
    if (!keygen) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    rnp::Key pub;
    rnp::Key sec;
    if (!keygen->generate(cert, sec, pub, ffi->secring->format)) {
        return RNP_ERROR_GENERIC;
    }

    if (!ffi->pubring->add_key(pub)) {
        return RNP_ERROR_OUT_OF_MEMORY;
    }
    /* encrypt secret key if specified */
    if (protect && prot.symm_alg && !sec.protect(prot, ffi->pass_provider, ffi->context)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    if (!ffi->secring->add_key(sec)) {
        return RNP_ERROR_OUT_OF_MEMORY;
    }
    fp = pub.fp();
    return RNP_SUCCESS;
}

static rnp_result_t
gen_json_subkey(rnp_ffi_t         ffi,
                json_object *     jsoparams,
                rnp::Key &        prim_pub,
                rnp::Key &        prim_sec,
                pgp::Fingerprint &fp)
{
    rnp::BindingParams          binding;
    rnp_key_protection_params_t prot = {};

    binding.key_expiration = DEFAULT_KEY_EXPIRATION;
    auto keygen = parse_keygen_sub(ffi, jsoparams, binding, prot);
    if (!keygen) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    if (!binding.flags) {
        /* Generate encrypt-only subkeys by default */
        binding.flags = PGP_KF_ENCRYPT;
    }
    rnp::Key pub;
    rnp::Key sec;
    if (!keygen->generate(
          binding, prim_sec, prim_pub, sec, pub, ffi->pass_provider, ffi->secring->format)) {
        return RNP_ERROR_GENERIC;
    }
    if (!ffi->pubring->add_key(pub)) {
        return RNP_ERROR_OUT_OF_MEMORY;
    }
    /* encrypt subkey if specified */
    if (prot.symm_alg && !sec.protect(prot, ffi->pass_provider, ffi->context)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    if (!ffi->secring->add_key(sec)) {
        return RNP_ERROR_OUT_OF_MEMORY;
    }
    fp = pub.fp();
    return RNP_SUCCESS;
}

rnp_result_t
rnp_generate_key_json(rnp_ffi_t ffi, const char *json, char **results)
try {
    // checks
    if (!ffi || !ffi->secring || !json) {
        return RNP_ERROR_NULL_POINTER;
    }

    // parse the JSON
    json_tokener_error error;
    json_object *      jso = json_tokener_parse_verbose(json, &error);
    if (!jso) {
        // syntax error or some other issue
        FFI_LOG(ffi, "Invalid JSON: %s", json_tokener_error_desc(error));
        return RNP_ERROR_BAD_FORMAT;
    }
    rnp::JSONObject jsowrap(jso);

    // locate the appropriate sections
    rnp_result_t ret = RNP_ERROR_GENERIC;
    json_object *jsoprimary = NULL;
    json_object *jsosub = NULL;
    {
        json_object_object_foreach(jso, key, value)
        {
            json_object **dest = NULL;

            if (rnp::str_case_eq(key, "primary")) {
                dest = &jsoprimary;
            } else if (rnp::str_case_eq(key, "sub")) {
                dest = &jsosub;
            } else {
                // unrecognized key in the object
                FFI_LOG(ffi, "Unexpected key in JSON: %s", key);
                return RNP_ERROR_BAD_PARAMETERS;
            }

            // duplicate "primary"/"sub"
            if (*dest) {
                return RNP_ERROR_BAD_PARAMETERS;
            }
            *dest = value;
        }
    }

    if (!jsoprimary && !jsosub) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    // generate primary key
    rnp::Key *                  prim_pub = nullptr;
    rnp::Key *                  prim_sec = nullptr;
    rnp_key_protection_params_t prim_prot = {};
    pgp::Fingerprint            fp;
    if (jsoprimary) {
        ret = gen_json_primary_key(ffi, jsoprimary, prim_prot, fp, !jsosub);
        if (ret) {
            return ret;
        }
        prim_pub = ffi->pubring->get_key(fp);
        if (!jsosub) {
            if (!gen_json_grips(results, prim_pub, NULL)) {
                return RNP_ERROR_OUT_OF_MEMORY;
            }
            return RNP_SUCCESS;
        }
        prim_sec = ffi->secring->get_key(fp);
    } else {
        /* generate subkey only - find primary key via JSON  params */
        json_object *jsoparent = NULL;
        if (!json_object_object_get_ex(jsosub, "primary", &jsoparent) ||
            json_object_object_length(jsoparent) != 1) {
            return RNP_ERROR_BAD_PARAMETERS;
        }
        const char *identifier_type = NULL;
        const char *identifier = NULL;
        json_object_object_foreach(jsoparent, key, value)
        {
            if (!json_object_is_type(value, json_type_string)) {
                return RNP_ERROR_BAD_PARAMETERS;
            }
            identifier_type = key;
            identifier = json_object_get_string(value);
        }
        if (!identifier_type || !identifier) {
            return RNP_ERROR_BAD_STATE;
        }

        auto search = rnp::KeySearch::create(identifier_type, identifier);
        if (!search) {
            return RNP_ERROR_BAD_PARAMETERS;
        }

        prim_pub = ffi->pubring->search(*search);
        prim_sec = ffi->secring->search(*search);
        if (!prim_sec || !prim_pub) {
            return RNP_ERROR_KEY_NOT_FOUND;
        }
        json_object_object_del(jsosub, "primary");
    }

    /* Generate subkey */
    ret = gen_json_subkey(ffi, jsosub, *prim_pub, *prim_sec, fp);
    if (ret) {
        if (jsoprimary) {
            /* do not leave generated primary key in keyring */
            ffi->pubring->remove_key(*prim_pub);
            ffi->secring->remove_key(*prim_sec);
        }
        return ret;
    }
    /* Protect the primary key now */
    if (prim_prot.symm_alg &&
        !prim_sec->protect(prim_prot, ffi->pass_provider, ffi->context)) {
        ffi->pubring->remove_key(*prim_pub);
        ffi->secring->remove_key(*prim_sec);
        return RNP_ERROR_BAD_PARAMETERS;
    }

    auto *sub_pub = ffi->pubring->get_key(fp);
    bool  res = gen_json_grips(results, jsoprimary ? prim_pub : NULL, sub_pub);
    return res ? RNP_SUCCESS : RNP_ERROR_OUT_OF_MEMORY;
}
FFI_GUARD

rnp_result_t
rnp_generate_key_ex(rnp_ffi_t         ffi,
                    const char *      key_alg,
                    const char *      sub_alg,
                    uint32_t          key_bits,
                    uint32_t          sub_bits,
                    const char *      key_curve,
                    const char *      sub_curve,
                    const char *      userid,
                    const char *      password,
                    rnp_key_handle_t *key)
try {
    rnp_op_generate_t op = NULL;
    rnp_op_generate_t subop = NULL;
    rnp_key_handle_t  primary = NULL;
    rnp_key_handle_t  subkey = NULL;
    rnp_result_t      ret = RNP_ERROR_KEY_GENERATION;

    /* generate primary key */
    if ((ret = rnp_op_generate_create(&op, ffi, key_alg))) {
        return ret;
    }
    if (key_bits && (ret = rnp_op_generate_set_bits(op, key_bits))) {
        goto done;
    }
    if (key_curve && (ret = rnp_op_generate_set_curve(op, key_curve))) {
        goto done;
    }
    if ((ret = rnp_op_generate_set_userid(op, userid))) {
        goto done;
    }
    if ((ret = rnp_op_generate_add_usage(op, "sign"))) {
        goto done;
    }
    if ((ret = rnp_op_generate_add_usage(op, "certify"))) {
        goto done;
    }
    if ((ret = rnp_op_generate_execute(op))) {
        goto done;
    }
    if ((ret = rnp_op_generate_get_key(op, &primary))) {
        goto done;
    }
    /* generate subkey if requested */
    if (!sub_alg) {
        goto done;
    }
    if ((ret = rnp_op_generate_subkey_create(&subop, ffi, primary, sub_alg))) {
        goto done;
    }
    if (sub_bits && (ret = rnp_op_generate_set_bits(subop, sub_bits))) {
        goto done;
    }
    if (sub_curve && (ret = rnp_op_generate_set_curve(subop, sub_curve))) {
        goto done;
    }
    if (password && (ret = rnp_op_generate_set_protection_password(subop, password))) {
        goto done;
    }
    if (pgp_pk_alg_capabilities(subop->keygen.alg()) & PGP_KF_ENCRYPT) {
        if ((ret = rnp_op_generate_add_usage(subop, "encrypt"))) {
            goto done;
        }
    } else {
        if ((ret = rnp_op_generate_add_usage(subop, "sign"))) {
            goto done;
        }
    }
    if ((ret = rnp_op_generate_execute(subop))) {
        goto done;
    }
    if ((ret = rnp_op_generate_get_key(subop, &subkey))) {
        goto done;
    }
done:
    /* only now will protect the primary key - to not spend time on unlocking to sign
     * subkey */
    if (!ret && password) {
        ret = rnp_key_protect(primary, password, NULL, NULL, NULL, 0);
    }
    if (ret && primary) {
        rnp_key_remove(primary, RNP_KEY_REMOVE_PUBLIC | RNP_KEY_REMOVE_SECRET);
    }
    if (ret && subkey) {
        rnp_key_remove(subkey, RNP_KEY_REMOVE_PUBLIC | RNP_KEY_REMOVE_SECRET);
    }
    if (!ret && key) {
        *key = primary;
    } else {
        rnp_key_handle_destroy(primary);
    }
    rnp_key_handle_destroy(subkey);
    rnp_op_generate_destroy(op);
    rnp_op_generate_destroy(subop);
    return ret;
}
FFI_GUARD

rnp_result_t
rnp_generate_key_rsa(rnp_ffi_t         ffi,
                     uint32_t          bits,
                     uint32_t          subbits,
                     const char *      userid,
                     const char *      password,
                     rnp_key_handle_t *key)
try {
    return rnp_generate_key_ex(ffi,
                               RNP_ALGNAME_RSA,
                               subbits ? RNP_ALGNAME_RSA : NULL,
                               bits,
                               subbits,
                               NULL,
                               NULL,
                               userid,
                               password,
                               key);
}
FFI_GUARD

rnp_result_t
rnp_generate_key_dsa_eg(rnp_ffi_t         ffi,
                        uint32_t          bits,
                        uint32_t          subbits,
                        const char *      userid,
                        const char *      password,
                        rnp_key_handle_t *key)
try {
    return rnp_generate_key_ex(ffi,
                               RNP_ALGNAME_DSA,
                               subbits ? RNP_ALGNAME_ELGAMAL : NULL,
                               bits,
                               subbits,
                               NULL,
                               NULL,
                               userid,
                               password,
                               key);
}
FFI_GUARD

rnp_result_t
rnp_generate_key_ec(rnp_ffi_t         ffi,
                    const char *      curve,
                    const char *      userid,
                    const char *      password,
                    rnp_key_handle_t *key)
try {
    return rnp_generate_key_ex(
      ffi, RNP_ALGNAME_ECDSA, RNP_ALGNAME_ECDH, 0, 0, curve, curve, userid, password, key);
}
FFI_GUARD

rnp_result_t
rnp_generate_key_25519(rnp_ffi_t         ffi,
                       const char *      userid,
                       const char *      password,
                       rnp_key_handle_t *key)
try {
    return rnp_generate_key_ex(ffi,
                               RNP_ALGNAME_EDDSA,
                               RNP_ALGNAME_ECDH,
                               0,
                               0,
                               NULL,
                               "Curve25519",
                               userid,
                               password,
                               key);
}
FFI_GUARD

rnp_result_t
rnp_generate_key_sm2(rnp_ffi_t         ffi,
                     const char *      userid,
                     const char *      password,
                     rnp_key_handle_t *key)
try {
    return rnp_generate_key_ex(
      ffi, RNP_ALGNAME_SM2, RNP_ALGNAME_SM2, 0, 0, NULL, NULL, userid, password, key);
}
FFI_GUARD

pgp_key_flags_t
rnp_op_generate_st::default_key_flags(pgp_pubkey_alg_t alg, bool subkey)
{
    switch (alg) {
    case PGP_PKA_RSA:
        return subkey ? PGP_KF_ENCRYPT : pgp_key_flags_t(PGP_KF_SIGN | PGP_KF_CERTIFY);
    case PGP_PKA_DSA:
    case PGP_PKA_ECDSA:
    case PGP_PKA_EDDSA:
        return subkey ? PGP_KF_SIGN : pgp_key_flags_t(PGP_KF_SIGN | PGP_KF_CERTIFY);
    case PGP_PKA_SM2:
        return subkey ? PGP_KF_ENCRYPT : pgp_key_flags_t(PGP_KF_SIGN | PGP_KF_CERTIFY);
    case PGP_PKA_ECDH:
    case PGP_PKA_ELGAMAL:
        return PGP_KF_ENCRYPT;
#if defined(ENABLE_CRYPTO_REFRESH)
    case PGP_PKA_ED25519:
        return subkey ? PGP_KF_SIGN : pgp_key_flags_t(PGP_KF_SIGN | PGP_KF_CERTIFY);
    case PGP_PKA_X25519:
        return PGP_KF_ENCRYPT;
#endif
#if defined(ENABLE_PQC)
    case PGP_PKA_KYBER768_X25519:
        FALLTHROUGH_STATEMENT;
    // TODO: Add case for PGP_PKA_KYBER1024_X448 with FALLTHROUGH_STATEMENT;
    case PGP_PKA_KYBER768_P256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_KYBER1024_P384:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_KYBER768_BP256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_KYBER1024_BP384:
        return PGP_KF_ENCRYPT;
    case PGP_PKA_DILITHIUM3_ED25519:
        FALLTHROUGH_STATEMENT;
    // TODO: Add case for PGP_PKA_DILITHIUM5_ED448 with FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM3_P256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM5_P384:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM3_BP256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM5_BP384:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_SPHINCSPLUS_SHA2:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_SPHINCSPLUS_SHAKE:
        return subkey ? PGP_KF_SIGN : pgp_key_flags_t(PGP_KF_SIGN | PGP_KF_CERTIFY);
#endif
    default:
        return PGP_KF_NONE;
    }
}

rnp_result_t
rnp_op_generate_create(rnp_op_generate_t *op, rnp_ffi_t ffi, const char *alg)
try {
    pgp_pubkey_alg_t key_alg = PGP_PKA_NOTHING;

    if (!op || !ffi || !alg) {
        return RNP_ERROR_NULL_POINTER;
    }

    if (!ffi->pubring || !ffi->secring) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    if (!str_to_pubkey_alg(alg, &key_alg)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    if (!(pgp_pk_alg_capabilities(key_alg) & PGP_KF_SIGN)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    *op = new rnp_op_generate_st(ffi, key_alg);
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_generate_subkey_create(rnp_op_generate_t *op,
                              rnp_ffi_t          ffi,
                              rnp_key_handle_t   primary,
                              const char *       alg)
try {
    if (!op || !ffi || !alg || !primary) {
        return RNP_ERROR_NULL_POINTER;
    }

    if (!ffi->pubring || !ffi->secring) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    /* TODO: should we do these checks here or may leave it up till generate call? */
    if (!primary->sec || !primary->sec->usable_for(PGP_OP_ADD_SUBKEY)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    pgp_pubkey_alg_t key_alg = PGP_PKA_NOTHING;
    if (!str_to_pubkey_alg(alg, &key_alg)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    *op = new rnp_op_generate_st(ffi, key_alg, primary);
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_generate_set_bits(rnp_op_generate_t op, uint32_t bits)
try {
    if (!op) {
        return RNP_ERROR_NULL_POINTER;
    }

    auto bitkeygen = dynamic_cast<pgp::BitsKeyParams *>(&op->keygen.key_params());
    if (!bitkeygen) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    bitkeygen->set_bits(bits);
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_generate_set_hash(rnp_op_generate_t op, const char *hash)
try {
    if (!op || !hash) {
        return RNP_ERROR_NULL_POINTER;
    }
    pgp_hash_alg_t halg = PGP_HASH_UNKNOWN;
    if (!str_to_hash_alg(hash, &halg)) {
        FFI_LOG(op->ffi, "Invalid hash: %s", hash);
        return RNP_ERROR_BAD_PARAMETERS;
    }
    op->keygen.set_hash(halg);
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_generate_set_dsa_qbits(rnp_op_generate_t op, uint32_t qbits)
try {
    if (!op) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (op->keygen.alg() != PGP_PKA_DSA) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    auto &dsa = dynamic_cast<pgp::DSAKeyParams &>(op->keygen.key_params());
    dsa.set_qbits(qbits);
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_generate_set_curve(rnp_op_generate_t op, const char *curve)
try {
    if (!op || !curve) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!pk_alg_allows_custom_curve(op->keygen.alg())) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    pgp_curve_t eccurve = PGP_CURVE_UNKNOWN;
    if (!curve_str_to_type(curve, &eccurve)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    auto &ecc = dynamic_cast<pgp::ECCKeyParams &>(op->keygen.key_params());
    ecc.set_curve(eccurve);
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_generate_set_protection_password(rnp_op_generate_t op, const char *password)
try {
    if (!op || !password) {
        return RNP_ERROR_NULL_POINTER;
    }
    op->password.assign(password, password + strlen(password) + 1);
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_generate_set_request_password(rnp_op_generate_t op, bool request)
try {
    if (!op || !request) {
        return RNP_ERROR_NULL_POINTER;
    }
    op->request_password = request;
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_generate_set_protection_cipher(rnp_op_generate_t op, const char *cipher)
try {
    if (!op || !cipher) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!str_to_cipher(cipher, &op->protection.symm_alg)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_generate_set_protection_hash(rnp_op_generate_t op, const char *hash)
try {
    if (!op || !hash) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!str_to_hash_alg(hash, &op->protection.hash_alg)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_generate_set_protection_mode(rnp_op_generate_t op, const char *mode)
try {
    if (!op || !mode) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!str_to_cipher_mode(mode, &op->protection.cipher_mode)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_generate_set_protection_iterations(rnp_op_generate_t op, uint32_t iterations)
try {
    if (!op) {
        return RNP_ERROR_NULL_POINTER;
    }
    op->protection.iterations = iterations;
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_generate_add_usage(rnp_op_generate_t op, const char *usage)
try {
    if (!op || !usage) {
        return RNP_ERROR_NULL_POINTER;
    }
    uint8_t flag = 0;
    if (!str_to_key_flag(usage, &flag)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    if (!(pgp_pk_alg_capabilities(op->keygen.alg()) & flag)) {
        return RNP_ERROR_NOT_SUPPORTED;
    }
    if (op->primary) {
        op->cert.flags |= flag;
    } else {
        op->binding.flags |= flag;
    }
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_generate_clear_usage(rnp_op_generate_t op)
try {
    if (!op) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (op->primary) {
        op->cert.flags = 0;
    } else {
        op->binding.flags = 0;
    }
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_generate_set_userid(rnp_op_generate_t op, const char *userid)
try {
    if (!op || !userid) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!op->primary) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    if (strlen(userid) > MAX_ID_LENGTH) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    op->cert.userid = userid;
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_generate_set_expiration(rnp_op_generate_t op, uint32_t expiration)
try {
    if (!op) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (op->primary) {
        op->cert.key_expiration = expiration;
    } else {
        op->binding.key_expiration = expiration;
    }
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_generate_clear_pref_hashes(rnp_op_generate_t op)
try {
    if (!op) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!op->primary) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    op->cert.prefs.hash_algs.clear();
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_generate_add_pref_hash(rnp_op_generate_t op, const char *hash)
try {
    if (!op || !hash) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!op->primary) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    pgp_hash_alg_t hash_alg = PGP_HASH_UNKNOWN;
    if (!str_to_hash_alg(hash, &hash_alg)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    op->cert.prefs.add_hash_alg(hash_alg);
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_generate_clear_pref_compression(rnp_op_generate_t op)
try {
    if (!op) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!op->primary) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    op->cert.prefs.z_algs.clear();
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_generate_add_pref_compression(rnp_op_generate_t op, const char *compression)
try {
    if (!op || !compression) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!op->primary) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    pgp_compression_type_t z_alg = PGP_C_UNKNOWN;
    if (!str_to_compression_alg(compression, &z_alg)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    op->cert.prefs.add_z_alg(z_alg);
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_generate_clear_pref_ciphers(rnp_op_generate_t op)
try {
    if (!op) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!op->primary) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    op->cert.prefs.symm_algs.clear();
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_generate_add_pref_cipher(rnp_op_generate_t op, const char *cipher)
try {
    if (!op || !cipher) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!op->primary) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    pgp_symm_alg_t symm_alg = PGP_SA_UNKNOWN;
    if (!str_to_cipher(cipher, &symm_alg)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    op->cert.prefs.add_symm_alg(symm_alg);
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_generate_set_pref_keyserver(rnp_op_generate_t op, const char *keyserver)
try {
    if (!op) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!op->primary) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    op->cert.prefs.key_server = keyserver ? keyserver : "";
    return RNP_SUCCESS;
}
FFI_GUARD

#if defined(RNP_EXPERIMENTAL_CRYPTO_REFRESH)
rnp_result_t
rnp_op_generate_set_v6_key(rnp_op_generate_t op)
try {
    if (!op) {
        return RNP_ERROR_NULL_POINTER;
    }
    op->keygen.set_version(PGP_V6);
    return RNP_SUCCESS;
}
FFI_GUARD
#endif

#if defined(RNP_EXPERIMENTAL_PQC)
rnp_result_t
rnp_op_generate_set_sphincsplus_param(rnp_op_generate_t op, const char *param_cstr)
try {
    if (!op || !param_cstr) {
        return RNP_ERROR_NULL_POINTER;
    }
    auto slhdsa = dynamic_cast<pgp::SlhdsaKeyParams *>(&op->keygen.key_params());
    if (!slhdsa) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    sphincsplus_parameter_t param;
    std::string             param_str = param_cstr;

    if (param_str == "128s") {
        param = sphincsplus_simple_128s;
    } else if (param_str == "128f") {
        param = sphincsplus_simple_128f;
    } else if (param_str == "192s") {
        param = sphincsplus_simple_192s;
    } else if (param_str == "192f") {
        param = sphincsplus_simple_192f;
    } else if (param_str == "256s") {
        param = sphincsplus_simple_256s;
    } else if (param_str == "256f") {
        param = sphincsplus_simple_256f;
    } else {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    slhdsa->set_param(param);
    return RNP_SUCCESS;
}
FFI_GUARD
#endif

rnp_result_t
rnp_op_generate_execute(rnp_op_generate_t op)
try {
    if (!op || !op->ffi) {
        return RNP_ERROR_NULL_POINTER;
    }

    rnp_result_t            ret = RNP_ERROR_GENERIC;
    rnp::Key                pub;
    rnp::Key                sec;
    pgp_password_provider_t prov;

    if (op->primary) {
        if (!op->keygen.generate(op->cert, sec, pub, op->ffi->secring->format)) {
            return RNP_ERROR_KEY_GENERATION;
        }
    } else {
        /* subkey generation */
        if (!op->keygen.generate(op->binding,
                                 *op->primary_sec,
                                 *op->primary_pub,
                                 sec,
                                 pub,
                                 op->ffi->pass_provider,
                                 op->ffi->secring->format)) {
            return RNP_ERROR_KEY_GENERATION;
        }
    }

    /* add public key part to the keyring */
    if (!(op->gen_pub = op->ffi->pubring->add_key(pub))) {
        ret = RNP_ERROR_OUT_OF_MEMORY;
        goto done;
    }

    /* encrypt secret key if requested */
    if (!op->password.empty()) {
        prov = {rnp_password_provider_string, (void *) op->password.data()};
    } else if (op->request_password) {
        prov = {rnp_password_cb_bounce, op->ffi};
    }
    if (prov.callback && !sec.protect(op->protection, prov, op->ffi->context)) {
        FFI_LOG(op->ffi, "failed to encrypt the key");
        ret = RNP_ERROR_BAD_PARAMETERS;
        goto done;
    }

    /* add secret key to the keyring */
    if (!(op->gen_sec = op->ffi->secring->add_key(sec))) {
        ret = RNP_ERROR_OUT_OF_MEMORY;
        goto done;
    }
    ret = RNP_SUCCESS;
done:
    op->password.clear();
    if (ret && op->gen_pub) {
        op->ffi->pubring->remove_key(*op->gen_pub);
        op->gen_pub = NULL;
    }
    if (ret && op->gen_sec) {
        op->ffi->secring->remove_key(*op->gen_sec);
        op->gen_sec = NULL;
    }
    return ret;
}
FFI_GUARD

rnp_result_t
rnp_op_generate_get_key(rnp_op_generate_t op, rnp_key_handle_t *handle)
try {
    if (!op || !handle) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!op->gen_sec || !op->gen_pub) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    *handle = new rnp_key_handle_st(op->ffi, op->gen_pub, op->gen_sec);
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_op_generate_destroy(rnp_op_generate_t op)
try {
    delete op;
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_handle_destroy(rnp_key_handle_t key)
try {
    delete key;
    return RNP_SUCCESS;
}
FFI_GUARD

void
rnp_buffer_destroy(void *ptr)
{
    free(ptr);
}

void
rnp_buffer_clear(void *ptr, size_t size)
{
    if (ptr) {
        secure_clear(ptr, size);
    }
}

static rnp::Key *
get_key_require_public(rnp_key_handle_t handle)
{
    if (!handle->pub && handle->sec) {
        // try fingerprint
        rnp::KeyFingerprintSearch fpsrch(handle->sec->fp());
        handle->pub = handle->ffi->key_provider.request_key(fpsrch);
        if (handle->pub) {
            return handle->pub;
        }

        // try keyid
        rnp::KeyIDSearch idsrch(handle->sec->keyid());
        handle->pub = handle->ffi->key_provider.request_key(idsrch);
    }
    return handle->pub;
}

static rnp::Key *
get_key_prefer_public(rnp_key_handle_t handle)
{
    auto *pub = get_key_require_public(handle);
    return pub ? pub : get_key_require_secret(handle);
}

static rnp::Key *
get_key_require_secret(rnp_key_handle_t handle)
{
    if (!handle->sec && handle->pub) {
        // try fingerprint
        rnp::KeyFingerprintSearch fpsrch(handle->pub->fp());
        handle->sec = handle->ffi->key_provider.request_key(fpsrch, PGP_OP_UNKNOWN, true);
        if (handle->sec) {
            return handle->sec;
        }

        // try keyid
        rnp::KeyIDSearch idsrch(handle->pub->keyid());
        handle->sec = handle->ffi->key_provider.request_key(idsrch, PGP_OP_UNKNOWN, true);
    }
    return handle->sec;
}

static rnp_result_t
key_get_uid_at(rnp::Key *key, size_t idx, char **uid)
{
    if (!key || !uid) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (idx >= key->uid_count()) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return ret_str_value(key->get_uid(idx).str.c_str(), uid);
}

rnp_result_t
rnp_key_add_uid(rnp_key_handle_t handle,
                const char *     uid,
                const char *     hash,
                uint32_t         expiration,
                uint8_t          key_flags,
                bool             primary)
try {
    if (!handle || !uid) {
        return RNP_ERROR_NULL_POINTER;
    }
    /* setup parameters */
    if (!hash) {
        hash = DEFAULT_HASH_ALG;
    }
    pgp_hash_alg_t hash_alg = PGP_HASH_UNKNOWN;
    if (!str_to_hash_alg(hash, &hash_alg)) {
        FFI_LOG(handle->ffi, "Invalid hash: %s", hash);
        return RNP_ERROR_BAD_PARAMETERS;
    }

    if (strlen(uid) > MAX_ID_LENGTH) {
        FFI_LOG(handle->ffi, "UserID too long");
        return RNP_ERROR_BAD_PARAMETERS;
    }
    rnp::CertParams info;
    info.userid = uid;
    info.flags = key_flags;
    info.key_expiration = expiration;
    info.primary = primary;

    /* obtain and unlok secret key */
    auto *secret_key = get_key_require_secret(handle);
    if (!secret_key || !secret_key->usable_for(PGP_OP_ADD_USERID)) {
        return RNP_ERROR_NO_SUITABLE_KEY;
    }
    auto *public_key = get_key_prefer_public(handle);
    if (!public_key && secret_key->format == rnp::KeyFormat::G10) {
        return RNP_ERROR_NO_SUITABLE_KEY;
    }
    rnp::KeyLocker seclock(*secret_key);
    if (secret_key->is_locked() &&
        !secret_key->unlock(handle->ffi->pass_provider, PGP_OP_ADD_USERID)) {
        return RNP_ERROR_BAD_PASSWORD;
    }
    /* add and certify userid */
    secret_key->add_uid_cert(info, hash_alg, handle->ffi->context, public_key);
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_get_primary_uid(rnp_key_handle_t handle, char **uid)
try {
    if (!handle || !uid) {
        return RNP_ERROR_NULL_POINTER;
    }

    auto *key = get_key_prefer_public(handle);
    if (key->has_primary_uid()) {
        return key_get_uid_at(key, key->get_primary_uid(), uid);
    }
    for (size_t i = 0; i < key->uid_count(); i++) {
        if (!key->get_uid(i).valid) {
            continue;
        }
        return key_get_uid_at(key, i, uid);
    }
    return RNP_ERROR_BAD_PARAMETERS;
}
FFI_GUARD

rnp_result_t
rnp_key_get_uid_count(rnp_key_handle_t handle, size_t *count)
try {
    if (!handle || !count) {
        return RNP_ERROR_NULL_POINTER;
    }

    *count = get_key_prefer_public(handle)->uid_count();
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_get_uid_at(rnp_key_handle_t handle, size_t idx, char **uid)
try {
    if (!handle || !uid) {
        return RNP_ERROR_NULL_POINTER;
    }

    auto *key = get_key_prefer_public(handle);
    return key_get_uid_at(key, idx, uid);
}
FFI_GUARD

rnp_result_t
rnp_key_get_uid_handle_at(rnp_key_handle_t key, size_t idx, rnp_uid_handle_t *uid)
try {
    if (!key || !uid) {
        return RNP_ERROR_NULL_POINTER;
    }

    auto *akey = get_key_prefer_public(key);
    if (!akey) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    if (idx >= akey->uid_count()) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    *uid = (rnp_uid_handle_t) malloc(sizeof(**uid));
    if (!*uid) {
        return RNP_ERROR_OUT_OF_MEMORY;
    }

    (*uid)->ffi = key->ffi;
    (*uid)->key = akey;
    (*uid)->idx = idx;
    return RNP_SUCCESS;
}
FFI_GUARD

static rnp::UserID *
rnp_uid_handle_get_uid(rnp_uid_handle_t uid)
{
    if (!uid || !uid->key) {
        return NULL;
    }
    return &uid->key->get_uid(uid->idx);
}

rnp_result_t
rnp_uid_get_type(rnp_uid_handle_t uid, uint32_t *type)
try {
    if (!type) {
        return RNP_ERROR_NULL_POINTER;
    }
    auto id = rnp_uid_handle_get_uid(uid);
    if (!id) {
        return RNP_ERROR_NULL_POINTER;
    }
    switch (id->pkt.tag) {
    case PGP_PKT_USER_ID:
        *type = RNP_USER_ID;
        return RNP_SUCCESS;
    case PGP_PKT_USER_ATTR:
        *type = RNP_USER_ATTR;
        return RNP_SUCCESS;
    default:
        return RNP_ERROR_BAD_STATE;
    }
}
FFI_GUARD

rnp_result_t
rnp_uid_get_data(rnp_uid_handle_t uid, void **data, size_t *size)
try {
    if (!data || !size) {
        return RNP_ERROR_NULL_POINTER;
    }
    auto id = rnp_uid_handle_get_uid(uid);
    if (!id) {
        return RNP_ERROR_NULL_POINTER;
    }
    *data = malloc(id->pkt.uid.size());
    if (id->pkt.uid.size() && !*data) {
        return RNP_ERROR_OUT_OF_MEMORY;
    }
    memcpy(*data, id->pkt.uid.data(), id->pkt.uid.size());
    *size = id->pkt.uid.size();
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_uid_is_primary(rnp_uid_handle_t uid, bool *primary)
try {
    if (!primary) {
        return RNP_ERROR_NULL_POINTER;
    }
    auto id = rnp_uid_handle_get_uid(uid);
    if (!id) {
        return RNP_ERROR_NULL_POINTER;
    }
    *primary = uid->key->has_primary_uid() && (uid->key->get_primary_uid() == uid->idx);
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_uid_is_valid(rnp_uid_handle_t uid, bool *valid)
try {
    if (!valid) {
        return RNP_ERROR_NULL_POINTER;
    }
    auto id = rnp_uid_handle_get_uid(uid);
    if (!id) {
        return RNP_ERROR_NULL_POINTER;
    }
    *valid = id->valid;
    return RNP_SUCCESS;
}
FFI_GUARD

static rnp_result_t
rnp_key_return_signature(rnp_ffi_t               ffi,
                         rnp::Key *              key,
                         rnp::Signature *        subsig,
                         rnp_signature_handle_t *sig)
{
    try {
        *sig = new rnp_signature_handle_st(ffi, key, subsig);
    } catch (const std::exception &e) {
        /* LCOV_EXCL_START */
        FFI_LOG(ffi, "%s", e.what());
        return RNP_ERROR_OUT_OF_MEMORY;
        /* LCOV_EXCL_END */
    }
    return RNP_SUCCESS;
}

rnp_result_t
rnp_key_get_signature_count(rnp_key_handle_t handle, size_t *count)
try {
    if (!handle || !count) {
        return RNP_ERROR_NULL_POINTER;
    }
    auto *key = get_key_prefer_public(handle);
    if (!key) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    *count = key->keysig_count();
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_get_signature_at(rnp_key_handle_t handle, size_t idx, rnp_signature_handle_t *sig)
try {
    if (!handle || !sig) {
        return RNP_ERROR_NULL_POINTER;
    }

    auto *key = get_key_prefer_public(handle);
    if (!key || (idx >= key->keysig_count())) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return rnp_key_return_signature(handle->ffi, key, &key->get_keysig(idx), sig);
}
FFI_GUARD

static rnp_result_t
create_key_signature(rnp_ffi_t               ffi,
                     rnp::Key &              sigkey,
                     rnp::Key &              tgkey,
                     uint32_t                uid,
                     rnp_signature_handle_t &sig,
                     pgp_sig_type_t          type)
{
    switch (type) {
    case PGP_CERT_GENERIC:
    case PGP_CERT_PERSONA:
    case PGP_CERT_CASUAL:
    case PGP_CERT_POSITIVE:
        assert(uid != rnp::UserID::None);
        if (!sigkey.is_primary() || !tgkey.is_primary()) {
            return RNP_ERROR_BAD_PARAMETERS;
        }
        break;
    case PGP_SIG_DIRECT:
        assert(uid == rnp::UserID::None);
        if (!tgkey.is_primary()) {
            return RNP_ERROR_BAD_PARAMETERS;
        }
        break;
    case PGP_SIG_REV_KEY:
        assert(uid == rnp::UserID::None);
        if (!tgkey.is_primary()) {
            type = PGP_SIG_REV_SUBKEY;
        }
        break;
    default:
        return RNP_ERROR_NOT_IMPLEMENTED;
    }
    pgp::pkt::Signature sigpkt;
    sigkey.sign_init(
      ffi->rng(), sigpkt, DEFAULT_PGP_HASH_ALG, ffi->context.time(), sigkey.version());
    sigpkt.set_type(type);
    std::unique_ptr<rnp::Signature> subsig(new rnp::Signature(sigpkt));
    subsig->uid = uid;
    sig = new rnp_signature_handle_st(ffi, &tgkey, nullptr, true, true);
    sig->sig = subsig.release();
    return RNP_SUCCESS;
}

static rnp_result_t
create_key_signature(rnp_key_handle_t        signer,
                     rnp_key_handle_t        target,
                     rnp_signature_handle_t *sig,
                     pgp_sig_type_t          type)
{
    if (!signer || !sig) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!target) {
        target = signer;
    }
    auto *sigkey = get_key_require_secret(signer);
    auto *tgkey = get_key_prefer_public(target);
    if (!sigkey || !tgkey) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return create_key_signature(signer->ffi, *sigkey, *tgkey, rnp::UserID::None, *sig, type);
}

rnp_result_t
rnp_key_direct_signature_create(rnp_key_handle_t        signer,
                                rnp_key_handle_t        target,
                                rnp_signature_handle_t *sig)
try {
    return create_key_signature(signer, target, sig, PGP_SIG_DIRECT);
}
FFI_GUARD

rnp_result_t
rnp_key_certification_create(rnp_key_handle_t        signer,
                             rnp_uid_handle_t        uid,
                             const char *            type,
                             rnp_signature_handle_t *sig)
try {
    if (!signer || !uid || !sig) {
        return RNP_ERROR_NULL_POINTER;
    }
    auto *sigkey = get_key_require_secret(signer);
    auto *tgkey = uid->key;
    if (!sigkey || !tgkey) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    if (!type) {
        type =
          sigkey->fp() == tgkey->fp() ? RNP_CERTIFICATION_POSITIVE : RNP_CERTIFICATION_GENERIC;
    }
    auto sigtype = static_cast<pgp_sig_type_t>(id_str_pair::lookup(cert_type_map, type));
    if (!sigtype) {
        FFI_LOG(signer->ffi, "Invalid certification type: %s", type);
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return create_key_signature(signer->ffi, *sigkey, *tgkey, uid->idx, *sig, sigtype);
}
FFI_GUARD

rnp_result_t
rnp_key_revocation_signature_create(rnp_key_handle_t        signer,
                                    rnp_key_handle_t        target,
                                    rnp_signature_handle_t *sig)
try {
    return create_key_signature(signer, target, sig, PGP_SIG_REV_KEY);
}
FFI_GUARD

rnp_result_t
rnp_key_signature_set_hash(rnp_signature_handle_t sig, const char *hash)
try {
    if (!sig || !hash) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!sig->new_sig) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    if (!str_to_hash_alg(hash, &sig->sig->sig.halg)) {
        FFI_LOG(sig->ffi, "Invalid hash: %s", hash);
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_signature_set_creation(rnp_signature_handle_t sig, uint32_t ctime)
try {
    if (!sig) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!sig->new_sig) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    sig->sig->sig.set_creation(ctime);
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_signature_set_key_flags(rnp_signature_handle_t sig, uint32_t flags)
try {
    if (!sig) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!sig->new_sig) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    uint32_t check = flags;
    extract_flag(check,
                 PGP_KF_SIGN | PGP_KF_CERTIFY | PGP_KF_ENCRYPT_COMMS | PGP_KF_ENCRYPT_STORAGE |
                   PGP_KF_SPLIT | PGP_KF_AUTH | PGP_KF_SHARED);
    if (check) {
        FFI_LOG(sig->ffi, "Unknown key flags: %#" PRIx32, check);
        return RNP_ERROR_BAD_PARAMETERS;
    }
    sig->sig->sig.set_key_flags(flags & 0xff);
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_signature_set_key_expiration(rnp_signature_handle_t sig, uint32_t expiry)
try {
    if (!sig) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!sig->new_sig) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    sig->sig->sig.set_key_expiration(expiry);
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_signature_set_features(rnp_signature_handle_t sig, uint32_t features)
try {
    if (!sig) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!sig->new_sig) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    uint32_t flags = features;
    extract_flag(flags, RNP_KEY_FEATURE_MDC | RNP_KEY_FEATURE_AEAD | RNP_KEY_FEATURE_V5);
    if (flags) {
        FFI_LOG(sig->ffi, "Unknown key features: %#" PRIx32, flags);
        return RNP_ERROR_BAD_PARAMETERS;
    }
    sig->sig->sig.set_key_features(features & 0xff);
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_signature_add_preferred_alg(rnp_signature_handle_t sig, const char *alg)
try {
    if (!sig || !alg) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!sig->new_sig) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    auto symm_alg = id_str_pair::lookup(symm_alg_map, alg, PGP_SA_UNKNOWN);
    if (symm_alg == PGP_SA_UNKNOWN) {
        FFI_LOG(sig->ffi, "Unknown symmetric algorithm: %s", alg);
        return RNP_ERROR_BAD_PARAMETERS;
    }
    auto algs = sig->sig->sig.preferred_symm_algs();
    algs.push_back(symm_alg);
    sig->sig->sig.set_preferred_symm_algs(algs);
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_signature_add_preferred_hash(rnp_signature_handle_t sig, const char *hash)
try {
    if (!sig || !hash) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!sig->new_sig) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    auto hash_alg = id_str_pair::lookup(hash_alg_map, hash, PGP_HASH_UNKNOWN);
    if (hash_alg == PGP_HASH_UNKNOWN) {
        FFI_LOG(sig->ffi, "Unknown hash algorithm: %s", hash);
        return RNP_ERROR_BAD_PARAMETERS;
    }
    auto algs = sig->sig->sig.preferred_hash_algs();
    algs.push_back(hash_alg);
    sig->sig->sig.set_preferred_hash_algs(algs);
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_signature_add_preferred_zalg(rnp_signature_handle_t sig, const char *zalg)
try {
    if (!sig || !zalg) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!sig->new_sig) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    auto z_alg = id_str_pair::lookup(compress_alg_map, zalg, PGP_C_UNKNOWN);
    if (z_alg == PGP_C_UNKNOWN) {
        FFI_LOG(sig->ffi, "Unknown compression algorithm: %s", zalg);
        return RNP_ERROR_BAD_PARAMETERS;
    }
    auto algs = sig->sig->sig.preferred_z_algs();
    algs.push_back(z_alg);
    sig->sig->sig.set_preferred_z_algs(algs);
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_signature_set_primary_uid(rnp_signature_handle_t sig, bool primary)
try {
    if (!sig) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!sig->new_sig) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    sig->sig->sig.set_primary_uid(primary);
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_signature_set_key_server(rnp_signature_handle_t sig, const char *keyserver)
try {
    if (!sig) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!sig->new_sig) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    if (!keyserver) {
        keyserver = "";
    }
    sig->sig->sig.set_key_server(keyserver);
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_signature_set_key_server_prefs(rnp_signature_handle_t sig, uint32_t flags)
try {
    if (!sig) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!sig->new_sig) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    uint32_t check = flags;
    extract_flag(check, RNP_KEY_SERVER_NO_MODIFY);
    if (check) {
        FFI_LOG(sig->ffi, "Unknown key server prefs: %#" PRIx32, check);
        return RNP_ERROR_BAD_PARAMETERS;
    }
    sig->sig->sig.set_key_server_prefs(flags & 0xff);
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_signature_set_revocation_reason(rnp_signature_handle_t sig,
                                        const char *           code,
                                        const char *           reason)
try {
    if (!sig) {
        return RNP_ERROR_NULL_POINTER;
    }
    rnp::Revocation rev;
    if (!fill_revocation_reason(sig->ffi, rev, code, reason)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    sig->sig->sig.set_revocation_reason(rev.code, rev.reason);
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_signature_set_revoker(rnp_signature_handle_t sig,
                              rnp_key_handle_t       revoker,
                              uint32_t               flags)
try {
    if (!sig || !revoker) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!sig->new_sig) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    bool sensitive = extract_flag(flags, RNP_REVOKER_SENSITIVE);
    if (flags) {
        FFI_LOG(sig->ffi, "Unsupported flags: %" PRIu32, flags);
        return RNP_ERROR_BAD_PARAMETERS;
    }
    auto key = get_key_prefer_public(revoker);
    if (!key) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    sig->sig->sig.set_revoker(*key, sensitive);
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_signature_set_trust_level(rnp_signature_handle_t sig, uint8_t level, uint8_t amount)
try {
    if (!sig) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!sig->new_sig) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    sig->sig->sig.set_trust(level, amount);
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_signature_sign(rnp_signature_handle_t sig)
try {
    if (!sig) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!sig->new_sig) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    auto &sigpkt = sig->sig->sig;
    /* Get signer and target */
    auto signer = sig->ffi->secring->get_signer(sigpkt);
    if (!signer) {
        return RNP_ERROR_BAD_STATE;
    }
    /* Unlock if needed */
    rnp::KeyLocker seclock(*signer);
    if (signer->is_locked() && !signer->unlock(sig->ffi->pass_provider)) {
        FFI_LOG(sig->ffi, "Failed to unlock secret key");
        return RNP_ERROR_BAD_PASSWORD;
    }
    /* Sign */
    const pgp_userid_pkt_t *uidptr = nullptr;
    bool                    front = false;
    switch (sigpkt.type()) {
    case PGP_CERT_GENERIC:
    case PGP_CERT_PERSONA:
    case PGP_CERT_CASUAL:
    case PGP_CERT_POSITIVE: {
        assert(sig->sig->uid != rnp::UserID::None);
        assert(sig->sig->uid < sig->key->uid_count());
        if (sig->sig->uid >= sig->key->uid_count()) {
            return RNP_ERROR_BAD_STATE;
        }
        auto &uidpkt = sig->key->get_uid(sig->sig->uid).pkt;
        signer->sign_cert(sig->key->pkt(), uidpkt, sigpkt, sig->ffi->context);
        uidptr = &uidpkt;
        break;
    }
    case PGP_SIG_DIRECT:
        signer->sign_direct(sig->key->pkt(), sigpkt, sig->ffi->context);
        front = true;
        break;
    case PGP_SIG_REV_KEY:
        signer->sign_direct(sig->key->pkt(), sigpkt, sig->ffi->context);
        front = true;
        break;
    case PGP_SIG_REV_SUBKEY:
        signer->sign_binding(sig->key->pkt(), sigpkt, sig->ffi->context);
        front = true;
        break;
    default:
        FFI_LOG(sig->ffi, "Not yet supported signature type.");
        return RNP_ERROR_BAD_STATE;
    }
    /* Add to the keyring(s) */
    auto secsig = sig->ffi->secring->add_key_sig(sig->key->fp(), sigpkt, uidptr, front);
    auto pubsig = sig->ffi->pubring->add_key_sig(sig->key->fp(), sigpkt, uidptr, front);
    /* Should not happen but let's check */
    if (!secsig && !pubsig) {
        return RNP_ERROR_BAD_STATE;
    }
    /* Replace owned sig with pointer to the key's one */
    delete sig->sig;
    sig->sig = pubsig ? pubsig : secsig;
    sig->own_sig = false;
    sig->new_sig = false;

    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_get_revoker_count(rnp_key_handle_t handle, size_t *count)
try {
    if (!handle || !count) {
        return RNP_ERROR_NULL_POINTER;
    }
    auto *key = get_key_prefer_public(handle);
    if (!key) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    *count = key->revoker_count();
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_get_revoker_at(rnp_key_handle_t handle, size_t idx, char **revoker)
try {
    if (!handle || !revoker) {
        return RNP_ERROR_NULL_POINTER;
    }
    auto *key = get_key_prefer_public(handle);
    if (!key || (idx >= key->revoker_count())) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return ret_fingerprint(key->get_revoker(idx), revoker);
}
FFI_GUARD

rnp_result_t
rnp_key_get_revocation_signature(rnp_key_handle_t handle, rnp_signature_handle_t *sig)
try {
    if (!handle || !sig) {
        return RNP_ERROR_NULL_POINTER;
    }
    auto *key = get_key_prefer_public(handle);
    if (!key) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    if (!key->revoked()) {
        *sig = NULL;
        return RNP_SUCCESS;
    }
    if (!key->has_sig(key->revocation().sigid)) {
        return RNP_ERROR_BAD_STATE;
    }
    return rnp_key_return_signature(
      handle->ffi, key, &key->get_sig(key->revocation().sigid), sig);
}
FFI_GUARD

rnp_result_t
rnp_uid_get_signature_count(rnp_uid_handle_t handle, size_t *count)
try {
    if (!handle || !count) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!handle->key) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    *count = handle->key->get_uid(handle->idx).sig_count();
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_uid_get_signature_at(rnp_uid_handle_t handle, size_t idx, rnp_signature_handle_t *sig)
try {
    if (!handle || !sig) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!handle->key) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    auto &uid = handle->key->get_uid(handle->idx);
    if (idx >= uid.sig_count()) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    auto &sigid = uid.get_sig(idx);
    if (!handle->key->has_sig(sigid)) {
        return RNP_ERROR_BAD_STATE;
    }
    return rnp_key_return_signature(
      handle->ffi, handle->key, &handle->key->get_sig(sigid), sig);
}
FFI_GUARD

rnp_result_t
rnp_signature_get_type(rnp_signature_handle_t handle, char **type)
try {
    if (!handle || !type) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!handle->sig) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    auto sigtype = id_str_pair::lookup(sig_type_map, handle->sig->sig.type());
    return ret_str_value(sigtype, type);
}
FFI_GUARD

rnp_result_t
rnp_signature_get_alg(rnp_signature_handle_t handle, char **alg)
try {
    if (!handle || !alg) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!handle->sig) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return get_map_value(pubkey_alg_map, handle->sig->sig.palg, alg);
}
FFI_GUARD

rnp_result_t
rnp_signature_get_hash_alg(rnp_signature_handle_t handle, char **alg)
try {
    if (!handle || !alg) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!handle->sig) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return get_map_value(hash_alg_map, handle->sig->sig.halg, alg);
}
FFI_GUARD

rnp_result_t
rnp_signature_get_creation(rnp_signature_handle_t handle, uint32_t *create)
try {
    if (!handle || !create) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!handle->sig) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    *create = handle->sig->sig.creation();
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_signature_subpacket_count(rnp_signature_handle_t handle, size_t *count)
try {
    if (!handle || !count) {
        return RNP_ERROR_NULL_POINTER;
    }
    *count = handle->sig->sig.subpkts.size();
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_signature_subpacket_at(rnp_signature_handle_t handle,
                           size_t                 idx,
                           rnp_sig_subpacket_t *  subpkt)
try {
    if (!handle || !subpkt) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (idx >= handle->sig->sig.subpkts.size()) {
        return RNP_ERROR_NOT_FOUND;
    }
    *subpkt = new rnp_sig_subpacket_st(*handle->sig->sig.subpkts[idx]);
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_signature_subpacket_find(rnp_signature_handle_t handle,
                             uint8_t                type,
                             bool                   hashed,
                             size_t                 skip,
                             rnp_sig_subpacket_t *  subpkt)
try {
    if (!handle || !subpkt) {
        return RNP_ERROR_NULL_POINTER;
    }
    auto idx = handle->sig->sig.find_subpkt(type, hashed, skip);
    return rnp_signature_subpacket_at(handle, idx, subpkt);
}
FFI_GUARD

rnp_result_t
rnp_signature_subpacket_info(rnp_sig_subpacket_t subpkt,
                             uint8_t *           type,
                             bool *              hashed,
                             bool *              critical)
try {
    if (!subpkt) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (type) {
        *type = subpkt->sub.raw_type();
    }
    if (hashed) {
        *hashed = subpkt->sub.hashed();
    }
    if (critical) {
        *critical = subpkt->sub.critical();
    }
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_signature_subpacket_data(rnp_sig_subpacket_t subpkt, uint8_t **data, size_t *size)
try {
    if (!subpkt || !data || !size) {
        return RNP_ERROR_NULL_POINTER;
    }
    auto &subdata = subpkt->sub.data();
    if (!subdata.size()) {
        return RNP_ERROR_BAD_STATE;
    }
    return ret_vec_value(subdata, data, size);
}
FFI_GUARD

rnp_result_t
rnp_signature_subpacket_destroy(rnp_sig_subpacket_t subpkt)
try {
    delete subpkt;
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_signature_get_expiration(rnp_signature_handle_t handle, uint32_t *expires)
try {
    if (!handle || !expires) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!handle->sig) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    *expires = handle->sig->sig.expiration();
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_signature_get_features(rnp_signature_handle_t handle, uint32_t *features)
try {
    if (!handle || !features) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!handle->sig) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    *features = handle->sig->sig.key_get_features();
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_signature_get_preferred_alg_count(rnp_signature_handle_t sig, size_t *count)
try {
    if (!sig || !count) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!sig->sig) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    *count = sig->sig->sig.preferred_symm_algs().size();
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_signature_get_preferred_alg(rnp_signature_handle_t sig, size_t idx, char **alg)
try {
    if (!sig || !alg) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!sig->sig) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    auto algs = sig->sig->sig.preferred_symm_algs();
    if (idx >= algs.size()) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return get_map_value(symm_alg_map, algs[idx], alg);
}
FFI_GUARD

rnp_result_t
rnp_signature_get_preferred_hash_count(rnp_signature_handle_t sig, size_t *count)
try {
    if (!sig || !count) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!sig->sig) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    *count = sig->sig->sig.preferred_hash_algs().size();
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_signature_get_preferred_hash(rnp_signature_handle_t sig, size_t idx, char **alg)
try {
    if (!sig || !alg) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!sig->sig) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    auto algs = sig->sig->sig.preferred_hash_algs();
    if (idx >= algs.size()) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return get_map_value(hash_alg_map, algs[idx], alg);
}
FFI_GUARD

rnp_result_t
rnp_signature_get_preferred_zalg_count(rnp_signature_handle_t sig, size_t *count)
try {
    if (!sig || !count) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!sig->sig) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    *count = sig->sig->sig.preferred_z_algs().size();
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_signature_get_preferred_zalg(rnp_signature_handle_t sig, size_t idx, char **alg)
try {
    if (!sig || !alg) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!sig->sig) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    auto algs = sig->sig->sig.preferred_z_algs();
    if (idx >= algs.size()) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return get_map_value(compress_alg_map, algs[idx], alg);
}
FFI_GUARD

rnp_result_t
rnp_signature_get_key_flags(rnp_signature_handle_t sig, uint32_t *flags)
try {
    if (!sig || !flags) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!sig->sig) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    *flags = sig->sig->sig.key_flags();
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_signature_get_key_expiration(rnp_signature_handle_t sig, uint32_t *expiry)
try {
    if (!sig || !expiry) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!sig->sig) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    *expiry = sig->sig->sig.key_expiration();
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_signature_get_primary_uid(rnp_signature_handle_t sig, bool *primary)
try {
    if (!sig || !primary) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!sig->sig) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    *primary = sig->sig->sig.primary_uid();
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_signature_get_key_server(rnp_signature_handle_t sig, char **keyserver)
try {
    if (!sig || !keyserver) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!sig->sig) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return ret_str_value(sig->sig->sig.key_server().c_str(), keyserver);
}
FFI_GUARD

rnp_result_t
rnp_signature_get_key_server_prefs(rnp_signature_handle_t sig, uint32_t *flags)
try {
    if (!sig || !flags) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!sig->sig) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    *flags = sig->sig->sig.key_server_prefs();
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_signature_get_keyid(rnp_signature_handle_t handle, char **result)
try {
    if (!handle || !result) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!handle->sig) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    if (!handle->sig->sig.has_keyid()) {
        *result = NULL;
        return RNP_SUCCESS;
    }
    return ret_keyid(handle->sig->sig.keyid(), result);
}
FFI_GUARD

rnp_result_t
rnp_signature_get_key_fprint(rnp_signature_handle_t handle, char **result)
try {
    if (!handle || !result) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!handle->sig) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    if (!handle->sig->sig.has_keyfp()) {
        *result = NULL;
        return RNP_SUCCESS;
    }
    return ret_fingerprint(handle->sig->sig.keyfp(), result);
}
FFI_GUARD

rnp_result_t
rnp_signature_get_signer(rnp_signature_handle_t sig, rnp_key_handle_t *key)
try {
    if (!sig || !sig->sig || !key) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    *key = get_signer_handle(sig->ffi, sig->sig->sig);
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_signature_get_revoker(rnp_signature_handle_t handle, char **revoker)
try {
    if (!handle || !revoker) {
        return RNP_ERROR_NULL_POINTER;
    }
    auto &sig = handle->sig->sig;
    if (!sig.has_revoker()) {
        return ret_str_value("", revoker);
    }
    return ret_fingerprint(sig.revoker(), revoker);
}
FFI_GUARD

rnp_result_t
rnp_signature_get_revocation_reason(rnp_signature_handle_t sig, char **code, char **reason)
try {
    if (!sig) {
        return RNP_ERROR_NULL_POINTER;
    }
    std::string rcode;
    std::string rreason;
    if (sig->sig->sig.has_revocation_reason()) {
        rcode = id_str_pair::lookup(revocation_code_map, sig->sig->sig.revocation_code(), "");
        rreason = sig->sig->sig.revocation_reason();
    }
    if (code) {
        rnp_result_t ret = ret_str_value(rcode.c_str(), code);
        if (ret) {
            return ret;
        }
    }
    if (reason) {
        rnp_result_t ret = ret_str_value(rreason.c_str(), reason);
        if (ret) {
            if (code) {
                free(*code);
                *code = NULL;
            }
            return ret;
        }
    }
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_signature_get_trust_level(rnp_signature_handle_t sig, uint8_t *level, uint8_t *amount)
try {
    if (!sig) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!sig->sig) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    if (level) {
        *level = sig->sig->sig.trust_level();
    }
    if (amount) {
        *amount = sig->sig->sig.trust_amount();
    }
    return RNP_SUCCESS;
}
FFI_GUARD

static rnp_result_t
validation_status(const rnp::SigValidity &validity)
{
    if (!validity.validated()) {
        return RNP_ERROR_VERIFICATION_FAILED;
    }
    if (validity.expired()) {
        return RNP_ERROR_SIGNATURE_EXPIRED;
    }
    if (validity.no_signer()) {
        return RNP_ERROR_KEY_NOT_FOUND;
    }
    return validity.valid() ? RNP_SUCCESS : RNP_ERROR_SIGNATURE_INVALID;
}

rnp_result_t
rnp_signature_is_valid(rnp_signature_handle_t sig, uint32_t flags)
try {
    if (!sig) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!sig->sig || sig->new_sig) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    bool revalidate = extract_flag(flags, RNP_SIGNATURE_REVALIDATE);
    if (flags) {
        RNP_LOG("Unknown flags: %" PRIu32, flags);
        return RNP_ERROR_BAD_PARAMETERS;
    }
    /* We do not revalidate document or new signatures */
    if (sig->own_sig) {
        if (revalidate) {
            RNP_LOG("revalidate flag for document signature - ignoring");
        }
        return validation_status(sig->sig->validity);
    }

    auto ssig = sig->sig;
    if (revalidate) {
        ssig->validity.reset();
    }
    if (!ssig->validity.validated()) {
        auto *signer = sig->ffi->pubring->get_signer(ssig->sig, &sig->ffi->key_provider);
        if (!signer) {
            ssig->validity.mark_validated(RNP_ERROR_SIG_NO_SIGNER_KEY);
            return RNP_ERROR_KEY_NOT_FOUND;
        }
        signer->validate_sig(*sig->key, *ssig, sig->ffi->context);
    }

    return validation_status(ssig->validity);
}
FFI_GUARD

rnp_result_t
rnp_signature_error_count(rnp_signature_handle_t sig, size_t *count)
try {
    if (!sig || !count) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!sig->sig->validity.validated()) {
        return RNP_ERROR_VERIFICATION_FAILED;
    }
    *count = sig->sig->validity.errors().size();
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_signature_error_at(rnp_signature_handle_t sig, size_t idx, rnp_result_t *error)
try {
    if (!sig || !error) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (idx >= sig->sig->validity.errors().size()) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    *error = sig->sig->validity.errors().at(idx);
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_signature_packet_to_json(rnp_signature_handle_t sig, uint32_t flags, char **json)
try {
    if (!sig || !json) {
        return RNP_ERROR_NULL_POINTER;
    }

    rnp::MemoryDest memdst;
    sig->sig->sig.write(memdst.dst());
    auto              vec = memdst.to_vector();
    rnp::MemorySource memsrc(vec);
    return rnp_dump_src_to_json(memsrc.src(), flags, json);
}
FFI_GUARD

rnp_result_t
rnp_signature_remove(rnp_key_handle_t key, rnp_signature_handle_t sig)
try {
    if (!key || !sig) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (sig->own_sig || !sig->sig) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    auto *pkey = get_key_require_public(key);
    auto *skey = get_key_require_secret(key);
    if (!pkey && !skey) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    auto sigid = sig->sig->sigid;
    bool ok = false;
    if (pkey) {
        ok = pkey->del_sig(sigid);
        pkey->revalidate(*key->ffi->pubring);
    }
    if (skey) {
        /* secret key may not have signature, but we still need to delete it at least once to
         * succeed */
        ok = skey->del_sig(sigid) || ok;
        skey->revalidate(*key->ffi->secring);
    }
    return ok ? RNP_SUCCESS : RNP_ERROR_NO_SIGNATURES_FOUND;
}
FFI_GUARD

static rnp_result_t
write_signature(rnp_signature_handle_t sig, pgp_dest_t &dst)
{
    sig->sig->raw.write(dst);
    dst_flush(&dst);
    return dst.werr;
}

rnp_result_t
rnp_signature_export(rnp_signature_handle_t sig, rnp_output_t output, uint32_t flags)
try {
    if (!sig || !sig->sig || !output) {
        return RNP_ERROR_NULL_POINTER;
    }
    bool need_armor = extract_flag(flags, RNP_KEY_EXPORT_ARMORED);
    if (flags) {
        FFI_LOG(sig->ffi, "Invalid flags: %" PRIu32, flags);
        return RNP_ERROR_BAD_PARAMETERS;
    }
    rnp_result_t ret;
    if (need_armor) {
        rnp::ArmoredDest armor(output->dst, PGP_ARMORED_PUBLIC_KEY);
        ret = write_signature(sig, armor.dst());
    } else {
        ret = write_signature(sig, output->dst);
    }
    output->keep = !ret;
    return ret;
}
FFI_GUARD

rnp_result_t
rnp_signature_handle_destroy(rnp_signature_handle_t sig)
try {
    if (sig && sig->own_sig) {
        delete sig->sig;
    }
    delete sig;
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_uid_is_revoked(rnp_uid_handle_t uid, bool *result)
try {
    if (!uid || !result) {
        return RNP_ERROR_NULL_POINTER;
    }

    if (!uid->key) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    *result = uid->key->get_uid(uid->idx).revoked;
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_uid_get_revocation_signature(rnp_uid_handle_t uid, rnp_signature_handle_t *sig)
try {
    if (!uid || !sig) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!uid->key) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    if (uid->idx >= uid->key->uid_count()) {
        return RNP_ERROR_BAD_STATE;
    }
    const auto &userid = uid->key->get_uid(uid->idx);
    if (!userid.revoked) {
        *sig = NULL;
        return RNP_SUCCESS;
    }
    if (!uid->key->has_sig(userid.revocation.sigid)) {
        return RNP_ERROR_BAD_STATE;
    }
    return rnp_key_return_signature(
      uid->ffi, uid->key, &uid->key->get_sig(userid.revocation.sigid), sig);
}
FFI_GUARD

rnp_result_t
rnp_uid_remove(rnp_key_handle_t key, rnp_uid_handle_t uid)
try {
    if (!key || !uid) {
        return RNP_ERROR_NULL_POINTER;
    }
    auto *pkey = get_key_require_public(key);
    auto *skey = get_key_require_secret(key);
    if (!pkey && !skey) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    if ((uid->key != pkey) && (uid->key != skey)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    bool ok = false;
    if (pkey && (pkey->uid_count() > uid->idx)) {
        pkey->del_uid(uid->idx);
        pkey->revalidate(*key->ffi->pubring);
        ok = true;
    }
    if (skey && (skey->uid_count() > uid->idx)) {
        skey->del_uid(uid->idx);
        skey->revalidate(*key->ffi->secring);
        ok = true;
    }
    return ok ? RNP_SUCCESS : RNP_ERROR_BAD_PARAMETERS;
}
FFI_GUARD

rnp_result_t
rnp_uid_handle_destroy(rnp_uid_handle_t uid)
try {
    free(uid);
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_get_version(rnp_key_handle_t handle, uint32_t *version)
try {
    if (!handle || !version) {
        return RNP_ERROR_NULL_POINTER;
    }

    *version = get_key_prefer_public(handle)->version();
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_get_subkey_count(rnp_key_handle_t handle, size_t *count)
try {
    if (!handle || !count) {
        return RNP_ERROR_NULL_POINTER;
    }
    auto *key = get_key_prefer_public(handle);
    *count = key->subkey_count();
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_get_subkey_at(rnp_key_handle_t handle, size_t idx, rnp_key_handle_t *subkey)
try {
    if (!handle || !subkey) {
        return RNP_ERROR_NULL_POINTER;
    }
    auto *key = get_key_prefer_public(handle);
    if (idx >= key->subkey_count()) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    rnp::KeyFingerprintSearch search(key->get_subkey_fp(idx));
    return rnp_locate_key_int(handle->ffi, search, subkey);
}
FFI_GUARD

rnp_result_t
rnp_key_get_default_key(rnp_key_handle_t  primary_key,
                        const char *      usage,
                        uint32_t          flags,
                        rnp_key_handle_t *default_key)
try {
    if (!primary_key || !usage || !default_key) {
        return RNP_ERROR_NULL_POINTER;
    }
    uint8_t keyflag = 0;
    if (!str_to_key_flag(usage, &keyflag)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    bool no_primary = extract_flag(flags, RNP_KEY_SUBKEYS_ONLY);
#if defined(ENABLE_PQC)
    bool prefer_pqc_enc_subkey = extract_flag(flags, RNP_KEY_PREFER_PQC_ENC_SUBKEY);
#else
    bool prefer_pqc_enc_subkey = false;
#endif
    if (flags) {
        FFI_LOG(primary_key->ffi, "Invalid flags: %" PRIu32, flags);
        return RNP_ERROR_BAD_PARAMETERS;
    }
    pgp_op_t op = PGP_OP_UNKNOWN;
    bool     secret = false;
    switch (keyflag) {
    case PGP_KF_SIGN:
        op = PGP_OP_SIGN;
        secret = true;
        break;
    case PGP_KF_CERTIFY:
        op = PGP_OP_CERTIFY;
        secret = true;
        break;
    case PGP_KF_ENCRYPT:
        op = PGP_OP_ENCRYPT;
        break;
    default:
        return RNP_ERROR_BAD_PARAMETERS;
    }
    auto *key = get_key_prefer_public(primary_key);
    if (!key) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    auto *defkey = find_suitable_key(
      op, key, &primary_key->ffi->key_provider, no_primary, prefer_pqc_enc_subkey);
    if (!defkey) {
        *default_key = NULL;
        return RNP_ERROR_NO_SUITABLE_KEY;
    }

    rnp::KeyFingerprintSearch search(defkey->fp());
    rnp_result_t ret = rnp_locate_key_int(primary_key->ffi, search, default_key, secret);

    if (!*default_key && !ret) {
        return RNP_ERROR_NO_SUITABLE_KEY;
    }
    return ret;
}
FFI_GUARD

rnp_result_t
rnp_key_get_alg(rnp_key_handle_t handle, char **alg)
try {
    if (!handle || !alg) {
        return RNP_ERROR_NULL_POINTER;
    }
    auto *key = get_key_prefer_public(handle);
    return get_map_value(pubkey_alg_map, key->alg(), alg);
}
FFI_GUARD

#if defined(RNP_EXPERIMENTAL_PQC)
rnp_result_t
rnp_key_sphincsplus_get_param(rnp_key_handle_t handle, char **param)
try {
    if (!handle || !param) {
        return RNP_ERROR_NULL_POINTER;
    }
    auto *key = get_key_prefer_public(handle);
    if (key->alg() != PGP_PKA_SPHINCSPLUS_SHA2 && key->alg() != PGP_PKA_SPHINCSPLUS_SHAKE) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    auto material = dynamic_cast<const pgp::SlhdsaKeyMaterial *>(key->material());
    if (!material) {
        return RNP_ERROR_BAD_STATE;
    }
    return get_map_value(sphincsplus_params_map, material->pub().param(), param);
}
FFI_GUARD
#endif

rnp_result_t
rnp_key_get_bits(rnp_key_handle_t handle, uint32_t *bits)
try {
    if (!handle || !bits) {
        return RNP_ERROR_NULL_POINTER;
    }
    auto *key = get_key_prefer_public(handle);
    if (!key || !key->material()) {
        return RNP_ERROR_BAD_PARAMETERS; // LCOV_EXCL_LINE
    }
    size_t _bits = key->material()->bits();
    if (!_bits) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    *bits = _bits;
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_get_dsa_qbits(rnp_key_handle_t handle, uint32_t *qbits)
try {
    if (!handle || !qbits) {
        return RNP_ERROR_NULL_POINTER;
    }
    auto *key = get_key_prefer_public(handle);
    auto  material = dynamic_cast<const pgp::DSAKeyMaterial *>(key->material());
    if (!material) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    *qbits = material->qbits();
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_get_curve(rnp_key_handle_t handle, char **curve)
try {
    if (!handle || !curve) {
        return RNP_ERROR_NULL_POINTER;
    }
    auto *      key = get_key_prefer_public(handle);
    pgp_curve_t _curve = key->curve();
    if (_curve == PGP_CURVE_UNKNOWN) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    const char *curvename = NULL;
    if (!curve_type_to_str(_curve, &curvename)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return ret_str_value(curvename, curve);
}
FFI_GUARD

rnp_result_t
rnp_key_get_fprint(rnp_key_handle_t handle, char **fprint)
try {
    if (!handle || !fprint) {
        return RNP_ERROR_NULL_POINTER;
    }
    return ret_fingerprint(get_key_prefer_public(handle)->fp(), fprint);
}
FFI_GUARD

rnp_result_t
rnp_key_get_keyid(rnp_key_handle_t handle, char **keyid)
try {
    if (!handle || !keyid) {
        return RNP_ERROR_NULL_POINTER;
    }
    return ret_keyid(get_key_prefer_public(handle)->keyid(), keyid);
}
FFI_GUARD

rnp_result_t
rnp_key_get_grip(rnp_key_handle_t handle, char **grip)
try {
    if (!handle || !grip) {
        return RNP_ERROR_NULL_POINTER;
    }

    return ret_grip(get_key_prefer_public(handle)->grip(), grip);
}
FFI_GUARD

static const pgp::KeyGrip *
rnp_get_grip_by_fp(rnp_ffi_t ffi, const pgp::Fingerprint &fp)
{
    auto *key = ffi->pubring->get_key(fp);
    if (!key) {
        key = ffi->secring->get_key(fp);
    }
    return key ? &key->grip() : nullptr;
}

rnp_result_t
rnp_key_get_primary_grip(rnp_key_handle_t handle, char **grip)
try {
    if (!handle || !grip) {
        return RNP_ERROR_NULL_POINTER;
    }

    auto *key = get_key_prefer_public(handle);
    if (!key->is_subkey()) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    if (!key->has_primary_fp()) {
        *grip = NULL;
        return RNP_SUCCESS;
    }
    const pgp::KeyGrip *pgrip = rnp_get_grip_by_fp(handle->ffi, key->primary_fp());
    if (!pgrip) {
        *grip = NULL;
        return RNP_SUCCESS;
    }
    return ret_grip(*pgrip, grip);
}
FFI_GUARD

rnp_result_t
rnp_key_get_primary_fprint(rnp_key_handle_t handle, char **fprint)
try {
    if (!handle || !fprint) {
        return RNP_ERROR_NULL_POINTER;
    }

    auto *key = get_key_prefer_public(handle);
    if (!key->is_subkey()) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    if (!key->has_primary_fp()) {
        *fprint = NULL;
        return RNP_SUCCESS;
    }
    return ret_fingerprint(key->primary_fp(), fprint);
}
FFI_GUARD

rnp_result_t
rnp_key_allows_usage(rnp_key_handle_t handle, const char *usage, bool *result)
try {
    if (!handle || !usage || !result) {
        return RNP_ERROR_NULL_POINTER;
    }
    uint8_t flag = 0;
    if (!str_to_key_flag(usage, &flag)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    auto *key = get_key_prefer_public(handle);
    if (!key) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    *result = key->flags() & flag;
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_get_creation(rnp_key_handle_t handle, uint32_t *result)
try {
    if (!handle || !result) {
        return RNP_ERROR_NULL_POINTER;
    }
    auto *key = get_key_prefer_public(handle);
    if (!key) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    *result = key->creation();
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_is_revoked(rnp_key_handle_t handle, bool *result)
try {
    if (!handle || !result) {
        return RNP_ERROR_NULL_POINTER;
    }
    auto *key = get_key_prefer_public(handle);
    if (!key) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    *result = key->revoked();
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_is_valid(rnp_key_handle_t handle, bool *result)
try {
    if (!handle || !result) {
        return RNP_ERROR_NULL_POINTER;
    }
    auto *key = get_key_require_public(handle);
    if (!key) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    if (!key->validated()) {
        key->validate(*handle->ffi->pubring);
    }
    if (!key->validated()) {
        return RNP_ERROR_VERIFICATION_FAILED;
    }
    *result = key->valid();
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_valid_till(rnp_key_handle_t handle, uint32_t *result)
try {
    if (!result) {
        return RNP_ERROR_NULL_POINTER;
    }
    uint64_t     res = 0;
    rnp_result_t ret = rnp_key_valid_till64(handle, &res);
    if (ret) {
        return ret;
    }
    if (res == UINT64_MAX) {
        *result = UINT32_MAX;
    } else if (res >= UINT32_MAX) {
        *result = UINT32_MAX - 1;
    } else {
        *result = (uint32_t) res;
    }
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_valid_till64(rnp_key_handle_t handle, uint64_t *result)
try {
    if (!handle || !result) {
        return RNP_ERROR_NULL_POINTER;
    }
    auto *key = get_key_require_public(handle);
    if (!key) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    if (!key->validated()) {
        key->validate(*handle->ffi->pubring);
    }
    if (!key->validated()) {
        return RNP_ERROR_VERIFICATION_FAILED;
    }

    if (key->is_subkey()) {
        /* check validity time of the primary key as well */
        auto *primary = handle->ffi->pubring->primary_key(*key);
        if (!primary) {
            /* no primary key - subkey considered as never valid */
            *result = 0;
            return RNP_SUCCESS;
        }
        if (!primary->validated()) {
            primary->validate(*handle->ffi->pubring);
        }
        if (!primary->validated()) {
            return RNP_ERROR_VERIFICATION_FAILED;
        }
        *result = key->valid_till();
    } else {
        *result = key->valid_till();
    }
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_get_expiration(rnp_key_handle_t handle, uint32_t *result)
try {
    if (!handle || !result) {
        return RNP_ERROR_NULL_POINTER;
    }
    auto *key = get_key_prefer_public(handle);
    if (!key) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    *result = key->expiration();
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_set_expiration(rnp_key_handle_t key, uint32_t expiry)
try {
    if (!key) {
        return RNP_ERROR_NULL_POINTER;
    }

    auto *pkey = get_key_prefer_public(key);
    if (!pkey) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    auto *skey = get_key_require_secret(key);
    if (!skey) {
        FFI_LOG(key->ffi, "Secret key required.");
        return RNP_ERROR_BAD_PARAMETERS;
    }

    if (pkey->is_primary()) {
        if (!pgp_key_set_expiration(
              pkey, skey, expiry, key->ffi->pass_provider, key->ffi->context)) {
            return RNP_ERROR_GENERIC;
        }
        pkey->revalidate(*key->ffi->pubring);
        if (pkey != skey) {
            skey->revalidate(*key->ffi->secring);
        }
        return RNP_SUCCESS;
    }

    /* for subkey we need primary key */
    if (!pkey->has_primary_fp()) {
        FFI_LOG(key->ffi, "Primary key fp not available.");
        return RNP_ERROR_BAD_PARAMETERS;
    }

    rnp::KeyFingerprintSearch search(pkey->primary_fp());
    auto *                    prim_sec = find_key(key->ffi, search, true, true);
    if (!prim_sec) {
        FFI_LOG(key->ffi, "Primary secret key not found.");
        return RNP_ERROR_KEY_NOT_FOUND;
    }
    if (!pgp_subkey_set_expiration(
          pkey, prim_sec, skey, expiry, key->ffi->pass_provider, key->ffi->context)) {
        return RNP_ERROR_GENERIC;
    }
    prim_sec->revalidate(*key->ffi->secring);
    auto *prim_pub = find_key(key->ffi, search, false, true);
    if (prim_pub) {
        prim_pub->revalidate(*key->ffi->pubring);
    }
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_get_revocation_reason(rnp_key_handle_t handle, char **result)
try {
    if (!handle || !result) {
        return RNP_ERROR_NULL_POINTER;
    }
    auto *key = get_key_prefer_public(handle);
    if (!key || !key->revoked()) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return ret_str_value(key->revocation().reason.c_str(), result);
}
FFI_GUARD

static rnp_result_t
rnp_key_is_revoked_with_code(rnp_key_handle_t handle, bool *result, int code)
{
    if (!handle || !result) {
        return RNP_ERROR_NULL_POINTER;
    }
    auto *key = get_key_prefer_public(handle);
    if (!key || !key->revoked()) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    *result = key->revocation().code == code;
    return RNP_SUCCESS;
}

rnp_result_t
rnp_key_is_superseded(rnp_key_handle_t handle, bool *result)
try {
    return rnp_key_is_revoked_with_code(handle, result, PGP_REVOCATION_SUPERSEDED);
}
FFI_GUARD

rnp_result_t
rnp_key_is_compromised(rnp_key_handle_t handle, bool *result)
try {
    return rnp_key_is_revoked_with_code(handle, result, PGP_REVOCATION_COMPROMISED);
}
FFI_GUARD

rnp_result_t
rnp_key_is_retired(rnp_key_handle_t handle, bool *result)
try {
    return rnp_key_is_revoked_with_code(handle, result, PGP_REVOCATION_RETIRED);
}
FFI_GUARD

rnp_result_t
rnp_key_is_expired(rnp_key_handle_t handle, bool *result)
try {
    if (!handle || !result) {
        return RNP_ERROR_NULL_POINTER;
    }
    auto *key = get_key_prefer_public(handle);
    if (!key) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    *result = key->expired();
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_get_protection_type(rnp_key_handle_t key, char **type)
try {
    if (!key || !type) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!key->sec) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    const pgp_s2k_t &s2k = key->sec->pkt().sec_protection.s2k;
    const char *     res = "Unknown";
    if (s2k.usage == PGP_S2KU_NONE) {
        res = "None";
    }
    if ((s2k.usage == PGP_S2KU_ENCRYPTED) && (s2k.specifier != PGP_S2KS_EXPERIMENTAL)) {
        res = "Encrypted";
    }
    if ((s2k.usage == PGP_S2KU_ENCRYPTED_AND_HASHED) &&
        (s2k.specifier != PGP_S2KS_EXPERIMENTAL)) {
        res = "Encrypted-Hashed";
    }
    if ((s2k.specifier == PGP_S2KS_EXPERIMENTAL) &&
        (s2k.gpg_ext_num == PGP_S2K_GPG_NO_SECRET)) {
        res = "GPG-None";
    }
    if ((s2k.specifier == PGP_S2KS_EXPERIMENTAL) &&
        (s2k.gpg_ext_num == PGP_S2K_GPG_SMARTCARD)) {
        res = "GPG-Smartcard";
    }

    return ret_str_value(res, type);
}
FFI_GUARD

rnp_result_t
rnp_key_get_protection_mode(rnp_key_handle_t key, char **mode)
try {
    if (!key || !mode) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!key->sec) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    if (key->sec->pkt().sec_protection.s2k.usage == PGP_S2KU_NONE) {
        return ret_str_value("None", mode);
    }
    if (key->sec->pkt().sec_protection.s2k.specifier == PGP_S2KS_EXPERIMENTAL) {
        return ret_str_value("Unknown", mode);
    }

    return get_map_value(cipher_mode_map, key->sec->pkt().sec_protection.cipher_mode, mode);
}
FFI_GUARD

static bool
pgp_key_has_encryption_info(const rnp::Key *key)
{
    return (key->pkt().sec_protection.s2k.usage != PGP_S2KU_NONE) &&
           (key->pkt().sec_protection.s2k.specifier != PGP_S2KS_EXPERIMENTAL);
}

rnp_result_t
rnp_key_get_protection_cipher(rnp_key_handle_t key, char **cipher)
try {
    if (!key || !cipher) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!key->sec) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    if (!pgp_key_has_encryption_info(key->sec)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    return get_map_value(symm_alg_map, key->sec->pkt().sec_protection.symm_alg, cipher);
}
FFI_GUARD

rnp_result_t
rnp_key_get_protection_hash(rnp_key_handle_t key, char **hash)
try {
    if (!key || !hash) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!key->sec) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    if (!pgp_key_has_encryption_info(key->sec)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    return get_map_value(hash_alg_map, key->sec->pkt().sec_protection.s2k.hash_alg, hash);
}
FFI_GUARD

rnp_result_t
rnp_key_get_protection_iterations(rnp_key_handle_t key, size_t *iterations)
try {
    if (!key || !iterations) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!key->sec) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    if (!pgp_key_has_encryption_info(key->sec)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    if (key->sec->pkt().sec_protection.s2k.specifier == PGP_S2KS_ITERATED_AND_SALTED) {
        *iterations = pgp_s2k_decode_iterations(key->sec->pkt().sec_protection.s2k.iterations);
    } else {
        *iterations = 1;
    }
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_is_locked(rnp_key_handle_t handle, bool *result)
try {
    if (handle == NULL || result == NULL)
        return RNP_ERROR_NULL_POINTER;

    auto *key = get_key_require_secret(handle);
    if (!key) {
        return RNP_ERROR_NO_SUITABLE_KEY;
    }
    *result = key->is_locked();
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_lock(rnp_key_handle_t handle)
try {
    if (handle == NULL)
        return RNP_ERROR_NULL_POINTER;

    auto *key = get_key_require_secret(handle);
    if (!key) {
        return RNP_ERROR_NO_SUITABLE_KEY;
    }
    if (!key->lock()) {
        return RNP_ERROR_GENERIC;
    }
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_unlock(rnp_key_handle_t handle, const char *password)
try {
    if (!handle) {
        return RNP_ERROR_NULL_POINTER;
    }
    auto *key = get_key_require_secret(handle);
    if (!key) {
        return RNP_ERROR_NO_SUITABLE_KEY;
    }
    bool ok = false;
    if (password) {
        pgp_password_provider_t prov(rnp_password_provider_string,
                                     reinterpret_cast<void *>(const_cast<char *>(password)));
        ok = key->unlock(prov);
    } else {
        ok = key->unlock(handle->ffi->pass_provider);
    }
    if (!ok) {
        // likely a bad password
        return RNP_ERROR_BAD_PASSWORD;
    }
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_is_protected(rnp_key_handle_t handle, bool *result)
try {
    if (handle == NULL || result == NULL)
        return RNP_ERROR_NULL_POINTER;

    auto *key = get_key_require_secret(handle);
    if (!key) {
        return RNP_ERROR_NO_SUITABLE_KEY;
    }
    *result = key->is_protected();
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_protect(rnp_key_handle_t handle,
                const char *     password,
                const char *     cipher,
                const char *     cipher_mode,
                const char *     hash,
                size_t           iterations)
try {
    rnp_key_protection_params_t protection = {};

    // checks
    if (!handle || !password) {
        return RNP_ERROR_NULL_POINTER;
    }

    if (cipher && !str_to_cipher(cipher, &protection.symm_alg)) {
        FFI_LOG(handle->ffi, "Invalid cipher: %s", cipher);
        return RNP_ERROR_BAD_PARAMETERS;
    }
    if (cipher_mode && !str_to_cipher_mode(cipher_mode, &protection.cipher_mode)) {
        FFI_LOG(handle->ffi, "Invalid cipher mode: %s", cipher_mode);
        return RNP_ERROR_BAD_PARAMETERS;
    }
    if (hash && !str_to_hash_alg(hash, &protection.hash_alg)) {
        FFI_LOG(handle->ffi, "Invalid hash: %s", hash);
        return RNP_ERROR_BAD_PARAMETERS;
    }
    protection.iterations = iterations;

    // get the key
    auto *key = get_key_require_secret(handle);
    if (!key) {
        return RNP_ERROR_NO_SUITABLE_KEY;
    }
    pgp_key_pkt_t *   decrypted_key = NULL;
    const std::string pass = password;
    if (key->encrypted()) {
        pgp_password_ctx_t ctx(PGP_OP_PROTECT, key);
        decrypted_key = pgp_decrypt_seckey(*key, handle->ffi->pass_provider, ctx);
        if (!decrypted_key) {
            return RNP_ERROR_GENERIC;
        }
    }
    bool res = key->protect(
      decrypted_key ? *decrypted_key : key->pkt(), protection, pass, handle->ffi->context);
    delete decrypted_key;
    return res ? RNP_SUCCESS : RNP_ERROR_GENERIC;
}
FFI_GUARD

rnp_result_t
rnp_key_unprotect(rnp_key_handle_t handle, const char *password)
try {
    // checks
    if (!handle) {
        return RNP_ERROR_NULL_POINTER;
    }

    // get the key
    auto *key = get_key_require_secret(handle);
    if (!key) {
        return RNP_ERROR_NO_SUITABLE_KEY;
    }
    bool ok = false;
    if (password) {
        pgp_password_provider_t prov(rnp_password_provider_string,
                                     reinterpret_cast<void *>(const_cast<char *>(password)));
        ok = key->unprotect(prov, handle->ffi->context);
    } else {
        ok = key->unprotect(handle->ffi->pass_provider, handle->ffi->context);
    }
    if (!ok) {
        // likely a bad password
        return RNP_ERROR_BAD_PASSWORD;
    }
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_is_primary(rnp_key_handle_t handle, bool *result)
try {
    if (handle == NULL || result == NULL)
        return RNP_ERROR_NULL_POINTER;

    auto *key = get_key_prefer_public(handle);
    if (key->format == rnp::KeyFormat::G10) {
        // we can't currently determine this for a G10 secret key
        return RNP_ERROR_NO_SUITABLE_KEY;
    }
    *result = key->is_primary();
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_is_sub(rnp_key_handle_t handle, bool *result)
try {
    if (handle == NULL || result == NULL)
        return RNP_ERROR_NULL_POINTER;

    auto *key = get_key_prefer_public(handle);
    if (key->format == rnp::KeyFormat::G10) {
        // we can't currently determine this for a G10 secret key
        return RNP_ERROR_NO_SUITABLE_KEY;
    }
    *result = key->is_subkey();
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_have_secret(rnp_key_handle_t handle, bool *result)
try {
    if (handle == NULL || result == NULL)
        return RNP_ERROR_NULL_POINTER;

    *result = handle->sec != NULL;
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_key_have_public(rnp_key_handle_t handle, bool *result)
try {
    if (handle == NULL || result == NULL)
        return RNP_ERROR_NULL_POINTER;
    *result = handle->pub != NULL;
    return RNP_SUCCESS;
}
FFI_GUARD

static rnp_result_t
key_to_bytes(rnp::Key *key, uint8_t **buf, size_t *buf_len)
{
    auto vec = key->write_vec();
    return ret_vec_value(vec, buf, buf_len);
}

rnp_result_t
rnp_get_public_key_data(rnp_key_handle_t handle, uint8_t **buf, size_t *buf_len)
try {
    // checks
    if (!handle || !buf || !buf_len) {
        return RNP_ERROR_NULL_POINTER;
    }

    rnp::Key *key = handle->pub;
    if (!key) {
        return RNP_ERROR_NO_SUITABLE_KEY;
    }
    return key_to_bytes(key, buf, buf_len);
}
FFI_GUARD

rnp_result_t
rnp_get_secret_key_data(rnp_key_handle_t handle, uint8_t **buf, size_t *buf_len)
try {
    // checks
    if (!handle || !buf || !buf_len) {
        return RNP_ERROR_NULL_POINTER;
    }

    rnp::Key *key = handle->sec;
    if (!key) {
        return RNP_ERROR_NO_SUITABLE_KEY;
    }
    return key_to_bytes(key, buf, buf_len);
}
FFI_GUARD

static bool
add_json_key_usage(json_object *jso, uint8_t key_flags)
{
    json_object *jsoarr = json_object_new_array();
    if (!jsoarr) {
        return false;
    }
    rnp::JSONObject jawrap(jsoarr);
    for (size_t i = 0; i < ARRAY_SIZE(key_usage_map); i++) {
        if ((key_usage_map[i].id & key_flags) &&
            !json_array_add(jsoarr, key_usage_map[i].str)) {
            return false;
        }
    }
    return json_object_array_length(jsoarr) ? json_add(jso, "usage", jawrap.release()) : true;
}

static bool
add_json_key_flags(json_object *jso, uint8_t key_flags)
{
    json_object *jsoarr = json_object_new_array();
    if (!jsoarr) {
        return false;
    }
    rnp::JSONObject jawrap(jsoarr);
    for (size_t i = 0; i < ARRAY_SIZE(key_flags_map); i++) {
        if ((key_flags_map[i].id & key_flags) &&
            !json_array_add(jsoarr, key_flags_map[i].str)) {
            return false;
        }
    }
    return json_object_array_length(jsoarr) ? json_add(jso, "flags", jawrap.release()) : true;
}

static rnp_result_t
add_json_mpis(json_object *jso, ...)
{
    va_list      ap;
    const char * name;
    rnp_result_t ret = RNP_ERROR_GENERIC;

    va_start(ap, jso);
    while ((name = va_arg(ap, const char *))) {
        pgp::mpi *val = va_arg(ap, pgp::mpi *);
        if (!val) {
            ret = RNP_ERROR_BAD_PARAMETERS;
            goto done;
        }
        if (!json_add_hex(jso, name, val->data(), val->size())) {
            ret = RNP_ERROR_OUT_OF_MEMORY;
            goto done;
        }
    }
    ret = RNP_SUCCESS;
done:
    va_end(ap);
    return ret;
}

static rnp_result_t
add_json_mpis(json_object *jso, rnp::Key *key, bool secret = false)
{
    if (!key->material()) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    auto &km = *key->material();
    switch (km.alg()) {
    case PGP_PKA_RSA:
    case PGP_PKA_RSA_ENCRYPT_ONLY:
    case PGP_PKA_RSA_SIGN_ONLY: {
        auto &rsa = dynamic_cast<pgp::RSAKeyMaterial &>(km);
        if (!secret) {
            return add_json_mpis(jso, "n", &rsa.n(), "e", &rsa.e(), NULL);
        }
        return add_json_mpis(
          jso, "d", &rsa.d(), "p", &rsa.p(), "q", &rsa.q(), "u", &rsa.u(), NULL);
    }
    case PGP_PKA_ELGAMAL:
    case PGP_PKA_ELGAMAL_ENCRYPT_OR_SIGN: {
        auto &eg = dynamic_cast<pgp::EGKeyMaterial &>(km);
        if (!secret) {
            return add_json_mpis(jso, "p", &eg.p(), "g", &eg.g(), "y", &eg.y(), NULL);
        }
        return add_json_mpis(jso, "x", &eg.x(), NULL);
    }
    case PGP_PKA_DSA: {
        auto &dsa = dynamic_cast<pgp::DSAKeyMaterial &>(km);
        if (!secret) {
            return add_json_mpis(
              jso, "p", &dsa.p(), "q", &dsa.q(), "g", &dsa.g(), "y", &dsa.y(), NULL);
        }
        return add_json_mpis(jso, "x", &dsa.x(), NULL);
    }
    case PGP_PKA_ECDH:
    case PGP_PKA_ECDSA:
    case PGP_PKA_EDDSA:
    case PGP_PKA_SM2: {
        auto &ec = dynamic_cast<pgp::ECKeyMaterial &>(km);
        if (!secret) {
            return add_json_mpis(jso, "point", &ec.p(), NULL);
        }
        return add_json_mpis(jso, "x", &ec.x(), NULL);
    }
#if defined(ENABLE_CRYPTO_REFRESH)
    case PGP_PKA_ED25519:
    case PGP_PKA_X25519:
        return RNP_SUCCESS; /* TODO */
#endif
#if defined(ENABLE_PQC)
    case PGP_PKA_KYBER768_X25519:
        FALLTHROUGH_STATEMENT;
    // TODO: Add case for PGP_PKA_KYBER1024_X448 with FALLTHROUGH_STATEMENT;
    case PGP_PKA_KYBER768_P256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_KYBER1024_P384:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_KYBER768_BP256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_KYBER1024_BP384:
        return RNP_SUCCESS; /* TODO */
    case PGP_PKA_DILITHIUM3_ED25519:
        FALLTHROUGH_STATEMENT;
    // TODO: Add case for PGP_PKA_DILITHIUM5_ED448 with FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM3_P256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM5_P384:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM3_BP256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM5_BP384:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_SPHINCSPLUS_SHA2:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_SPHINCSPLUS_SHAKE:
        return RNP_SUCCESS; /* TODO */
#endif
    default:
        return RNP_ERROR_NOT_SUPPORTED;
    }
    return RNP_SUCCESS;
}

static rnp_result_t
add_json_sig_mpis(json_object *jso, const pgp::pkt::Signature *sig)
{
    auto material = sig->parse_material();
    if (!material) {
        return RNP_ERROR_NOT_SUPPORTED;
    }
    switch (sig->palg) {
    case PGP_PKA_RSA:
    case PGP_PKA_RSA_ENCRYPT_ONLY:
    case PGP_PKA_RSA_SIGN_ONLY: {
        auto &rsa = dynamic_cast<const pgp::RSASigMaterial &>(*material);
        return add_json_mpis(jso, "sig", &rsa.sig.s, NULL);
    }
    case PGP_PKA_ELGAMAL:
    case PGP_PKA_ELGAMAL_ENCRYPT_OR_SIGN: {
        auto &eg = dynamic_cast<const pgp::EGSigMaterial &>(*material);
        return add_json_mpis(jso, "r", &eg.sig.r, "s", &eg.sig.s, NULL);
    }
    case PGP_PKA_DSA: {
        auto &dsa = dynamic_cast<const pgp::DSASigMaterial &>(*material);
        return add_json_mpis(jso, "r", &dsa.sig.r, "s", &dsa.sig.s, NULL);
    }
    case PGP_PKA_ECDSA:
    case PGP_PKA_EDDSA:
    case PGP_PKA_SM2: {
        auto &ec = dynamic_cast<const pgp::ECSigMaterial &>(*material);
        return add_json_mpis(jso, "r", &ec.sig.r, "s", &ec.sig.s, NULL);
    }
#if defined(ENABLE_CRYPTO_REFRESH)
    case PGP_PKA_ED25519:
    case PGP_PKA_X25519:
        return RNP_SUCCESS; /* TODO */
#endif
#if defined(ENABLE_PQC)
    case PGP_PKA_DILITHIUM3_ED25519:
        FALLTHROUGH_STATEMENT;
    // TODO: Add case for PGP_PKA_DILITHIUM5_ED448 with FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM3_P256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM5_P384:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM3_BP256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM5_BP384:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_SPHINCSPLUS_SHA2:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_SPHINCSPLUS_SHAKE:
        return RNP_SUCCESS; /* TODO */
#endif
    default:
        // TODO: we could use info->unknown and add a hex string of raw data here
        return RNP_ERROR_NOT_SUPPORTED;
    }
    return RNP_SUCCESS;
}

static bool
add_json_array_lookup(json_object *        jso,
                      std::vector<uint8_t> vals,
                      const char *         name,
                      const id_str_pair *  map)
{
    if (vals.empty()) {
        return true;
    }
    json_object *jsoarr = json_object_new_array();
    if (!jsoarr || !json_add(jso, name, jsoarr)) {
        return false;
    }
    for (auto val : vals) {
        const char *vname = id_str_pair::lookup(map, val, "Unknown");
        if (!json_array_add(jsoarr, vname)) {
            return false;
        }
    }
    return true;
}

static bool
add_json_user_prefs(json_object *jso, const rnp::UserPrefs &prefs)
{
    // TODO: instead of using a string "Unknown" as a fallback for these,
    // we could add a string of hex/dec (or even an int)
    if (!add_json_array_lookup(jso, prefs.symm_algs, "ciphers", symm_alg_map)) {
        return false;
    }
    if (!add_json_array_lookup(jso, prefs.hash_algs, "hashes", hash_alg_map)) {
        return false;
    }
    if (!add_json_array_lookup(jso, prefs.z_algs, "compression", compress_alg_map)) {
        return false;
    }
    if (!add_json_array_lookup(
          jso, prefs.ks_prefs, "key server preferences", key_server_prefs_map)) {
        return false;
    }
    if (!prefs.key_server.empty()) {
        if (!json_add(jso, "key server", prefs.key_server.c_str())) {
            return false;
        }
    }
    return true;
}

static rnp_result_t
add_json_subsig(json_object *jso, bool is_sub, uint32_t flags, const rnp::Signature *subsig)
{
    // userid (if applicable)
    if (!is_sub && !json_add(jso, "userid", (int) subsig->uid)) {
        return RNP_ERROR_OUT_OF_MEMORY;
    }
    // trust
    json_object *jsotrust = json_object_new_object();
    if (!jsotrust || !json_add(jso, "trust", jsotrust)) {
        return RNP_ERROR_OUT_OF_MEMORY;
    }
    // trust level and amount
    if (!json_add(jsotrust, "level", (int) subsig->sig.trust_level()) ||
        !json_add(jsotrust, "amount", (int) subsig->sig.trust_amount())) {
        return RNP_ERROR_OUT_OF_MEMORY;
    }
    // key flags (usage)
    if (!add_json_key_usage(jso, subsig->sig.key_flags())) {
        return RNP_ERROR_OUT_OF_MEMORY;
    }
    // key flags (other)
    if (!add_json_key_flags(jso, subsig->sig.key_flags())) {
        return RNP_ERROR_OUT_OF_MEMORY;
    }
    // preferences
    const rnp::UserPrefs prefs(subsig->sig);
    if (!prefs.symm_algs.empty() || !prefs.hash_algs.empty() || !prefs.z_algs.empty() ||
        !prefs.ks_prefs.empty() || !prefs.key_server.empty()) {
        json_object *jsoprefs = json_object_new_object();
        if (!jsoprefs || !json_add(jso, "preferences", jsoprefs) ||
            !add_json_user_prefs(jsoprefs, prefs)) {
            return RNP_ERROR_OUT_OF_MEMORY;
        }
    }
    const pgp::pkt::Signature *sig = &subsig->sig;
    // version
    if (!json_add(jso, "version", (int) sig->version)) {
        return RNP_ERROR_OUT_OF_MEMORY;
    }
    // signature type
    auto type = id_str_pair::lookup(sig_type_map, sig->type());
    if (!json_add(jso, "type", type)) {
        return RNP_ERROR_OUT_OF_MEMORY;
    }
    // signer key type
    const char *key_type = id_str_pair::lookup(pubkey_alg_map, sig->palg);
    if (!json_add(jso, "key type", key_type)) {
        return RNP_ERROR_OUT_OF_MEMORY;
    }
    // hash
    const char *hash = id_str_pair::lookup(hash_alg_map, sig->halg);
    if (!json_add(jso, "hash", hash)) {
        return RNP_ERROR_OUT_OF_MEMORY;
    }
    // creation time
    if (!json_add(jso, "creation time", (uint64_t) sig->creation())) {
        return RNP_ERROR_OUT_OF_MEMORY;
    }
    // expiration (seconds)
    if (!json_add(jso, "expiration", (uint64_t) sig->expiration())) {
        return RNP_ERROR_OUT_OF_MEMORY;
    }
    // signer
    json_object *jsosigner = NULL;
    // TODO: add signer fingerprint as well (no support internally yet)
    if (sig->has_keyid()) {
        jsosigner = json_object_new_object();
        if (!jsosigner) {
            return RNP_ERROR_OUT_OF_MEMORY;
        }
        char       keyid[PGP_KEY_ID_SIZE * 2 + 1];
        pgp::KeyID signer = sig->keyid();
        if (!rnp::hex_encode(signer.data(), signer.size(), keyid, sizeof(keyid))) {
            return RNP_ERROR_GENERIC;
        }
        if (!json_add(jsosigner, "keyid", keyid)) {
            json_object_put(jsosigner);
            return RNP_ERROR_OUT_OF_MEMORY;
        }
    }
    json_object_object_add(jso, "signer", jsosigner);
    // mpis
    json_object *jsompis = NULL;
    if (flags & RNP_JSON_SIGNATURE_MPIS) {
        jsompis = json_object_new_object();
        if (!jsompis) {
            return RNP_ERROR_OUT_OF_MEMORY;
        }
        rnp_result_t tmpret;
        if ((tmpret = add_json_sig_mpis(jsompis, sig))) {
            json_object_put(jsompis);
            return tmpret;
        }
    }
    json_object_object_add(jso, "mpis", jsompis);
    return RNP_SUCCESS;
}

static rnp_result_t
key_to_json(json_object *jso, rnp_key_handle_t handle, uint32_t flags)
{
    rnp::Key *key = get_key_prefer_public(handle);

    // type
    const char *str = id_str_pair::lookup(pubkey_alg_map, key->alg(), NULL);
    if (!str) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    if (!json_add(jso, "type", str)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    // length
    auto km = key->material();
    if (!km) {
        return RNP_ERROR_BAD_PARAMETERS; // LCOV_EXCL_LINE
    }
    if (!json_add(jso, "length", (int) km->bits())) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    // curve / alg-specific items
    switch (key->alg()) {
    case PGP_PKA_ECDH: {
        auto ecdh = dynamic_cast<pgp::ECDHKeyMaterial *>(km);
        if (!ecdh) {
            return RNP_ERROR_BAD_PARAMETERS;
        }
        const char *hash_name = id_str_pair::lookup(hash_alg_map, ecdh->kdf_hash_alg(), NULL);
        if (!hash_name) {
            return RNP_ERROR_BAD_PARAMETERS;
        }
        const char *cipher_name =
          id_str_pair::lookup(symm_alg_map, ecdh->key_wrap_alg(), NULL);
        if (!cipher_name) {
            return RNP_ERROR_BAD_PARAMETERS;
        }
        if (!json_add(jso, "kdf hash", hash_name) ||
            !json_add(jso, "key wrap cipher", cipher_name)) {
            return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
        }
    }
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_ECDSA:
    case PGP_PKA_EDDSA:
    case PGP_PKA_SM2: {
        const char *curve_name = NULL;
        if (!curve_type_to_str(km->curve(), &curve_name)) {
            return RNP_ERROR_BAD_PARAMETERS;
        }
        if (!json_add(jso, "curve", curve_name)) {
            return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
        }
    } break;
#if defined(ENABLE_CRYPTO_REFRESH)
    case PGP_PKA_ED25519:
    case PGP_PKA_X25519:
        return RNP_SUCCESS; /* TODO */
#endif
#if defined(ENABLE_PQC)
    case PGP_PKA_KYBER768_X25519:
        FALLTHROUGH_STATEMENT;
    // TODO: Add case for PGP_PKA_KYBER1024_X448 with FALLTHROUGH_STATEMENT;
    case PGP_PKA_KYBER768_P256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_KYBER1024_P384:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_KYBER768_BP256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_KYBER1024_BP384:
        return RNP_SUCCESS; /* TODO */
    case PGP_PKA_DILITHIUM3_ED25519:
        FALLTHROUGH_STATEMENT;
    // TODO: Add case for PGP_PKA_DILITHIUM5_ED448 with FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM3_P256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM5_P384:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM3_BP256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM5_BP384:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_SPHINCSPLUS_SHA2:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_SPHINCSPLUS_SHAKE:
        return RNP_SUCCESS; /* TODO */
#endif
    default:
        break;
    }

    // keyid
    char keyid[PGP_KEY_ID_SIZE * 2 + 1];
    if (!rnp::hex_encode(key->keyid().data(), key->keyid().size(), keyid, sizeof(keyid))) {
        return RNP_ERROR_GENERIC;
    }
    if (!json_add(jso, "keyid", keyid)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    // fingerprint
    char fpr[PGP_FINGERPRINT_HEX_SIZE] = {0};
    if (!rnp::hex_encode(key->fp().data(), key->fp().size(), fpr, sizeof(fpr))) {
        return RNP_ERROR_GENERIC;
    }
    if (!json_add(jso, "fingerprint", fpr)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    // grip
    char grip[PGP_KEY_GRIP_SIZE * 2 + 1];
    if (!rnp::hex_encode(key->grip().data(), key->grip().size(), grip, sizeof(grip))) {
        return RNP_ERROR_GENERIC;
    }
    if (!json_add(jso, "grip", grip)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    // revoked
    if (!json_add(jso, "revoked", key->revoked())) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    // creation time
    if (!json_add(jso, "creation time", (uint64_t) key->creation())) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    // expiration
    if (!json_add(jso, "expiration", (uint64_t) key->expiration())) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    // key flags (usage)
    if (!add_json_key_usage(jso, key->flags())) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    // key flags (other)
    if (!add_json_key_flags(jso, key->flags())) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    // parent / subkeys
    if (key->is_primary()) {
        json_object *jsosubkeys_arr = json_object_new_array();
        if (!jsosubkeys_arr || !json_add(jso, "subkey grips", jsosubkeys_arr)) {
            return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
        }
        for (auto &subfp : key->subkey_fps()) {
            const pgp::KeyGrip *subgrip = rnp_get_grip_by_fp(handle->ffi, subfp);
            if (!subgrip) {
                continue;
            }
            if (!rnp::hex_encode(subgrip->data(), subgrip->size(), grip, sizeof(grip))) {
                return RNP_ERROR_GENERIC;
            }
            if (!json_array_add(jsosubkeys_arr, grip)) {
                return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
            }
        }
    } else if (key->has_primary_fp()) {
        auto pgrip = rnp_get_grip_by_fp(handle->ffi, key->primary_fp());
        if (pgrip) {
            if (!rnp::hex_encode(pgrip->data(), pgrip->size(), grip, sizeof(grip))) {
                return RNP_ERROR_GENERIC;
            }
            if (!json_add(jso, "primary key grip", grip)) {
                return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
            }
        }
    }
    // public
    json_object *jsopublic = json_object_new_object();
    if (!jsopublic || !json_add(jso, "public key", jsopublic)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    bool have_sec = handle->sec != NULL;
    bool have_pub = handle->pub != NULL;
    if (!json_add(jsopublic, "present", have_pub)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    if (flags & RNP_JSON_PUBLIC_MPIS) {
        json_object *jsompis = json_object_new_object();
        if (!jsompis || !json_add(jsopublic, "mpis", jsompis)) {
            return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
        }
        rnp_result_t tmpret;
        if ((tmpret = add_json_mpis(jsompis, key))) {
            return tmpret;
        }
    }
    // secret
    json_object *jsosecret = json_object_new_object();
    if (!jsosecret || !json_add(jso, "secret key", jsosecret) ||
        !json_add(jsosecret, "present", have_sec)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    if (have_sec) {
        bool locked = handle->sec->is_locked();
        if (flags & RNP_JSON_SECRET_MPIS) {
            if (locked) {
                json_object_object_add(jsosecret, "mpis", NULL);
            } else {
                json_object *jsompis = json_object_new_object();
                if (!jsompis || !json_add(jsosecret, "mpis", jsompis)) {
                    return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
                }
                rnp_result_t tmpret;
                if ((tmpret = add_json_mpis(jsompis, handle->sec, true))) {
                    return tmpret;
                }
            }
        }
        if (!json_add(jsosecret, "locked", locked) ||
            !json_add(jsosecret, "protected", handle->sec->is_protected())) {
            return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
        }
    }
    // userids
    if (key->is_primary()) {
        json_object *jsouids_arr = json_object_new_array();
        if (!jsouids_arr || !json_add(jso, "userids", jsouids_arr)) {
            return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
        }
        for (size_t i = 0; i < key->uid_count(); i++) {
            if (!json_array_add(jsouids_arr, key->get_uid(i).str.c_str())) {
                return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
            }
        }
    }
    // signatures
    if (flags & RNP_JSON_SIGNATURES) {
        json_object *jsosigs_arr = json_object_new_array();
        if (!jsosigs_arr || !json_add(jso, "signatures", jsosigs_arr)) {
            return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
        }
        for (size_t i = 0; i < key->sig_count(); i++) {
            json_object *jsosig = json_object_new_object();
            if (!jsosig || json_object_array_add(jsosigs_arr, jsosig)) {
                json_object_put(jsosig);
                return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
            }
            rnp_result_t tmpret;
            if ((tmpret =
                   add_json_subsig(jsosig, key->is_subkey(), flags, &key->get_sig(i)))) {
                return tmpret;
            }
        }
    }
    return RNP_SUCCESS;
}

rnp_result_t
rnp_key_to_json(rnp_key_handle_t handle, uint32_t flags, char **result)
try {
    // checks
    if (!handle || !result) {
        return RNP_ERROR_NULL_POINTER;
    }
    json_object *jso = json_object_new_object();
    if (!jso) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    rnp::JSONObject jsowrap(jso);
    rnp_result_t    ret = RNP_ERROR_GENERIC;
    if ((ret = key_to_json(jso, handle, flags))) {
        return ret;
    }
    return ret_str_value(json_object_to_json_string_ext(jso, JSON_C_TO_STRING_PRETTY), result);
}
FFI_GUARD

static rnp_result_t
rnp_dump_src_to_json(pgp_source_t &src, uint32_t flags, char **result)
{
    json_object *        jso = NULL;
    rnp::DumpContextJson dumpctx(src, &jso);

    dumpctx.set_dump_mpi(extract_flag(flags, RNP_JSON_DUMP_MPI));
    dumpctx.set_dump_packets(extract_flag(flags, RNP_JSON_DUMP_RAW));
    dumpctx.set_dump_grips(extract_flag(flags, RNP_JSON_DUMP_GRIP));
    if (flags) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    rnp_result_t ret = dumpctx.dump();
    if (ret) {
        json_object_put(jso);
        return ret;
    }

    rnp::JSONObject jsowrap(jso);
    return ret_str_value(json_object_to_json_string_ext(jso, JSON_C_TO_STRING_PRETTY), result);
}

rnp_result_t
rnp_key_packets_to_json(rnp_key_handle_t handle, bool secret, uint32_t flags, char **result)
try {
    if (!handle || !result) {
        return RNP_ERROR_NULL_POINTER;
    }

    rnp::Key *key = secret ? handle->sec : handle->pub;
    if (!key || (key->format == rnp::KeyFormat::G10)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    auto              vec = key->write_vec();
    rnp::MemorySource mem(vec);
    return rnp_dump_src_to_json(mem.src(), flags, result);
}
FFI_GUARD

rnp_result_t
rnp_dump_packets_to_json(rnp_input_t input, uint32_t flags, char **result)
try {
    if (!input || !result) {
        return RNP_ERROR_NULL_POINTER;
    }
    return rnp_dump_src_to_json(input->src, flags, result);
}
FFI_GUARD

rnp_result_t
rnp_dump_packets_to_output(rnp_input_t input, rnp_output_t output, uint32_t flags)
try {
    if (!input || !output) {
        return RNP_ERROR_NULL_POINTER;
    }

    rnp::DumpContextDst dumpctx(input->src, output->dst);
    dumpctx.set_dump_mpi(extract_flag(flags, RNP_JSON_DUMP_MPI));
    dumpctx.set_dump_packets(extract_flag(flags, RNP_JSON_DUMP_RAW));
    dumpctx.set_dump_grips(extract_flag(flags, RNP_JSON_DUMP_GRIP));
    if (flags) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    rnp_result_t ret = dumpctx.dump();
    output->keep = true;
    return ret;
}
FFI_GUARD

// move to next key
static bool
key_iter_next_key(rnp_identifier_iterator_t it)
{
    // check if we not reached the end of the ring
    *it->keyp = std::next(*it->keyp);
    if (*it->keyp != it->store->keys.end()) {
        it->uididx = 0;
        return true;
    }
    // if we are currently on pubring, switch to secring (if not empty)
    if (it->store == it->ffi->pubring && !it->ffi->secring->keys.empty()) {
        it->store = it->ffi->secring;
        *it->keyp = it->store->keys.begin();
        it->uididx = 0;
        return true;
    }
    // we've gone through both rings
    it->store = NULL;
    return false;
}

// move to next item (key or userid)
static bool
key_iter_next_item(rnp_identifier_iterator_t it)
{
    switch (it->type) {
    case rnp::KeySearch::Type::KeyID:
    case rnp::KeySearch::Type::Fingerprint:
    case rnp::KeySearch::Type::Grip:
        return key_iter_next_key(it);
    case rnp::KeySearch::Type::UserID:
        it->uididx++;
        while (it->uididx >= (*it->keyp)->uid_count()) {
            if (!key_iter_next_key(it)) {
                return false;
            }
            it->uididx = 0;
        }
        break;
    default:
        assert(false);
        break;
    }
    return true;
}

static bool
key_iter_first_key(rnp_identifier_iterator_t it)
{
    if (it->ffi->pubring->key_count()) {
        it->store = it->ffi->pubring;
    } else if (it->ffi->secring->key_count()) {
        it->store = it->ffi->secring;
    } else {
        it->store = NULL;
        return false;
    }
    *it->keyp = it->store->keys.begin();
    it->uididx = 0;
    return true;
}

static bool
key_iter_first_item(rnp_identifier_iterator_t it)
{
    switch (it->type) {
    case rnp::KeySearch::Type::KeyID:
    case rnp::KeySearch::Type::Fingerprint:
    case rnp::KeySearch::Type::Grip:
        return key_iter_first_key(it);
    case rnp::KeySearch::Type::UserID:
        if (!key_iter_first_key(it)) {
            return false;
        }
        it->uididx = 0;
        while (it->uididx >= (*it->keyp)->uid_count()) {
            if (!key_iter_next_key(it)) {
                return false;
            }
        }
        break;
    default:
        assert(false);
        break;
    }
    return true;
}

static std::string
key_iter_get_item(const rnp_identifier_iterator_t it)
{
    auto &key = **it->keyp;
    switch (it->type) {
    case rnp::KeySearch::Type::KeyID:
        return rnp::bin_to_hex(key.keyid().data(), key.keyid().size());
    case rnp::KeySearch::Type::Fingerprint:
        return rnp::bin_to_hex(key.fp().data(), key.fp().size());
    case rnp::KeySearch::Type::Grip:
        return rnp::bin_to_hex(key.grip().data(), key.grip().size());
    case rnp::KeySearch::Type::UserID:
        if (it->uididx >= key.uid_count()) {
            return "";
        }
        return key.get_uid(it->uididx).str;
    default:
        assert(false);
        return "";
    }
}

rnp_result_t
rnp_identifier_iterator_create(rnp_ffi_t                  ffi,
                               rnp_identifier_iterator_t *it,
                               const char *               identifier_type)
try {
    // checks
    if (!ffi || !it || !identifier_type) {
        return RNP_ERROR_NULL_POINTER;
    }
    // create iterator
    auto type = rnp::KeySearch::find_type(identifier_type);
    if (type == rnp::KeySearch::Type::Unknown) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    *it = new rnp_identifier_iterator_st(ffi, type);
    // move to first item (if any)
    key_iter_first_item(*it);
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_identifier_iterator_next(rnp_identifier_iterator_t it, const char **identifier)
try {
    // checks
    if (!it || !identifier) {
        return RNP_ERROR_NULL_POINTER;
    }
    // initialize the result to NULL
    *identifier = NULL;
    // this means we reached the end of the rings
    if (!it->store) {
        return RNP_SUCCESS;
    }
    // get the item
    it->item = key_iter_get_item(it);
    if (it->item.empty()) {
        return RNP_ERROR_GENERIC;
    }
    bool exists;
    bool iterator_valid = true;
    while ((exists = (it->tbl.find(it->item) != it->tbl.end()))) {
        if (!((iterator_valid = key_iter_next_item(it)))) {
            break;
        }
        it->item = key_iter_get_item(it);
        if (it->item.empty()) {
            return RNP_ERROR_GENERIC;
        }
    }
    // see if we actually found a new entry
    if (!exists) {
        it->tbl.insert(it->item);
        *identifier = it->item.c_str();
    }
    // prepare for the next one
    if (iterator_valid) {
        key_iter_next_item(it);
    }
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_identifier_iterator_destroy(rnp_identifier_iterator_t it)
try {
    if (it) {
        delete it;
    }
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_guess_contents(rnp_input_t input, char **contents)
try {
    if (!input || !contents) {
        return RNP_ERROR_NULL_POINTER;
    }

    pgp_armored_msg_t msgtype = PGP_ARMORED_UNKNOWN;
    if (input->src.is_cleartext()) {
        msgtype = PGP_ARMORED_CLEARTEXT;
    } else if (input->src.is_armored()) {
        msgtype = rnp_armored_get_type(&input->src);
    } else {
        msgtype = rnp_armor_guess_type(&input->src);
    }
    const char *msg = id_str_pair::lookup(armor_type_map, msgtype);
    size_t      len = strlen(msg);
    *contents = (char *) calloc(1, len + 1);
    if (!*contents) {
        return RNP_ERROR_OUT_OF_MEMORY;
    }
    memcpy(*contents, msg, len);
    return RNP_SUCCESS;
}
FFI_GUARD

rnp_result_t
rnp_enarmor(rnp_input_t input, rnp_output_t output, const char *type)
try {
    pgp_armored_msg_t msgtype = PGP_ARMORED_UNKNOWN;
    if (!input || !output) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (type) {
        msgtype = static_cast<pgp_armored_msg_t>(
          id_str_pair::lookup(armor_type_map, type, PGP_ARMORED_UNKNOWN));
        if (msgtype == PGP_ARMORED_UNKNOWN) {
            RNP_LOG("Unsupported armor type: %s", type);
            return RNP_ERROR_BAD_PARAMETERS;
        }
    } else {
        msgtype = rnp_armor_guess_type(&input->src);
        if (!msgtype) {
            RNP_LOG("Unrecognized data to armor (try specifying a type)");
            return RNP_ERROR_BAD_PARAMETERS;
        }
    }
    rnp_result_t ret = rnp_armor_source(&input->src, &output->dst, msgtype);
    output->keep = !ret;
    return ret;
}
FFI_GUARD

rnp_result_t
rnp_dearmor(rnp_input_t input, rnp_output_t output)
try {
    if (!input || !output) {
        return RNP_ERROR_NULL_POINTER;
    }
    rnp_result_t ret = rnp_dearmor_source(&input->src, &output->dst);
    output->keep = !ret;
    return ret;
}
FFI_GUARD

rnp_result_t
rnp_output_pipe(rnp_input_t input, rnp_output_t output)
try {
    if (!input || !output) {
        return RNP_ERROR_NULL_POINTER;
    }
    rnp_result_t ret = dst_write_src(&input->src, &output->dst);
    output->keep = !ret;
    return ret;
}
FFI_GUARD

rnp_result_t
rnp_output_armor_set_line_length(rnp_output_t output, size_t llen)
try {
    if (!output || !llen) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return armored_dst_set_line_length(&output->dst, llen);
}
FFI_GUARD

const char *
rnp_backend_string()
{
    return rnp::backend_string();
}

const char *
rnp_backend_version()
{
    return rnp::backend_version();
}
