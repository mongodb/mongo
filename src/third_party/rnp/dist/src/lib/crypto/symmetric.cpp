/*
 * Copyright (c) 2017-2024, [Ribose Inc](https://www.ribose.com).
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1.  Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 * 2.  Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "defaults.h"

#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <botan/ffi.h>
#include <botan/build.h>
#include "utils.h"
#include "repgp/repgp_def.h"
#include "symmetric.h"

static const char *
pgp_sa_to_botan_string(int alg, bool silent = false)
{
    switch (alg) {
#if defined(BOTAN_HAS_IDEA) && defined(ENABLE_IDEA)
    case PGP_SA_IDEA:
        return "IDEA";
#endif

#if defined(BOTAN_HAS_DES)
    case PGP_SA_TRIPLEDES:
        return "TripleDES";
#endif

#if defined(BOTAN_HAS_CAST) && defined(ENABLE_CAST5)
    case PGP_SA_CAST5:
        return "CAST-128";
#endif

#if defined(BOTAN_HAS_BLOWFISH) && defined(ENABLE_BLOWFISH)
    case PGP_SA_BLOWFISH:
        return "Blowfish";
#endif

#if defined(BOTAN_HAS_AES)
    case PGP_SA_AES_128:
        return "AES-128";
    case PGP_SA_AES_192:
        return "AES-192";
    case PGP_SA_AES_256:
        return "AES-256";
#endif

#if defined(BOTAN_HAS_SM4) && defined(ENABLE_SM2)
    case PGP_SA_SM4:
        return "SM4";
#endif

#if defined(BOTAN_HAS_TWOFISH) && defined(ENABLE_TWOFISH)
    case PGP_SA_TWOFISH:
        return "Twofish";
#endif

#if defined(BOTAN_HAS_CAMELLIA)
    case PGP_SA_CAMELLIA_128:
        return "Camellia-128";
    case PGP_SA_CAMELLIA_192:
        return "Camellia-192";
    case PGP_SA_CAMELLIA_256:
        return "Camellia-256";
#endif

    default:
        if (!silent) {
            RNP_LOG("Unsupported symmetric algorithm %d", alg);
        }
        return NULL;
    }
}

#if defined(ENABLE_AEAD)
static bool
pgp_aead_to_botan_string(pgp_symm_alg_t ealg, pgp_aead_alg_t aalg, char *buf, size_t len)
{
    const char *ealg_name = pgp_sa_to_botan_string(ealg);
    size_t      ealg_len;

    if (!ealg_name) {
        return false;
    }

    ealg_len = strlen(ealg_name);

    if (len < ealg_len + 5) {
        RNP_LOG("buffer too small");
        return false;
    }

    switch (aalg) {
    case PGP_AEAD_EAX:
        memcpy(buf, ealg_name, ealg_len);
        strncpy(buf + ealg_len, "/EAX", len - ealg_len);
        break;
    case PGP_AEAD_OCB:
        memcpy(buf, ealg_name, ealg_len);
        strncpy(buf + ealg_len, "/OCB", len - ealg_len);
        break;
    default:
        RNP_LOG("unsupported AEAD alg %d", (int) aalg);
        return false;
    }

    return true;
}
#endif

bool
pgp_cipher_cfb_start(pgp_crypt_t *  crypt,
                     pgp_symm_alg_t alg,
                     const uint8_t *key,
                     const uint8_t *iv)
{
    memset(crypt, 0x0, sizeof(*crypt));

    const char *cipher_name = pgp_sa_to_botan_string(alg);
    if (!cipher_name) {
        return false;
    }

    crypt->alg = alg;
    crypt->blocksize = pgp_block_size(alg);

    // This shouldn't happen if pgp_sa_to_botan_string returned a ptr
    if (botan_block_cipher_init(&(crypt->cfb.obj), cipher_name) != 0) {
        RNP_LOG("Block cipher '%s' not available", cipher_name);
        return false;
    }

    const size_t keysize = pgp_key_size(alg);

    if (botan_block_cipher_set_key(crypt->cfb.obj, key, keysize) != 0) {
        RNP_LOG("Failure setting key on block cipher object");
        return false;
    }

    if (iv != NULL) {
        // Otherwise left as all zeros via memset at start of function
        memcpy(crypt->cfb.iv, iv, crypt->blocksize);
    }

    crypt->cfb.remaining = 0;

    return true;
}

int
pgp_cipher_cfb_finish(pgp_crypt_t *crypt)
{
    if (!crypt) {
        return 0;
    }
    if (crypt->cfb.obj) {
        botan_block_cipher_destroy(crypt->cfb.obj);
        crypt->cfb.obj = NULL;
    }
    botan_scrub_mem((uint8_t *) crypt, sizeof(*crypt));
    return 0;
}

int
pgp_cipher_encrypt_block(pgp_crypt_t *crypt, uint8_t *iv, size_t blsize)
{
    return botan_block_cipher_encrypt_blocks(crypt->cfb.obj, iv, iv, 1);
}

bool
pgp_is_sa_supported(int alg, bool silent)
{
    return pgp_sa_to_botan_string(alg, silent);
}

#if defined(ENABLE_AEAD)
bool
pgp_cipher_aead_init(pgp_crypt_t *  crypt,
                     pgp_symm_alg_t ealg,
                     pgp_aead_alg_t aalg,
                     const uint8_t *key,
                     bool           decrypt)
{
    char     cipher_name[32];
    uint32_t flags;

    memset(crypt, 0x0, sizeof(*crypt));

    if (!pgp_aead_to_botan_string(ealg, aalg, cipher_name, sizeof(cipher_name))) {
        return false;
    }

    crypt->alg = ealg;
    crypt->blocksize = pgp_block_size(ealg);
    crypt->aead.alg = aalg;
    crypt->aead.decrypt = decrypt;
    crypt->aead.taglen = PGP_AEAD_EAX_OCB_TAG_LEN; /* it's the same for EAX and OCB */

    flags = decrypt ? BOTAN_CIPHER_INIT_FLAG_DECRYPT : BOTAN_CIPHER_INIT_FLAG_ENCRYPT;

    if (botan_cipher_init(&(crypt->aead.obj), cipher_name, flags)) {
        RNP_LOG("cipher %s is not available", cipher_name);
        return false;
    }

    if (botan_cipher_set_key(crypt->aead.obj, key, (size_t) pgp_key_size(ealg))) {
        RNP_LOG("failed to set key");
        return false;
    }

    if (botan_cipher_get_update_granularity(crypt->aead.obj, &crypt->aead.granularity)) {
        RNP_LOG("failed to get update granularity");
        return false;
    }

    return true;
}

bool
pgp_cipher_aead_set_ad(pgp_crypt_t *crypt, const uint8_t *ad, size_t len)
{
    return botan_cipher_set_associated_data(crypt->aead.obj, ad, len) == 0;
}

bool
pgp_cipher_aead_start(pgp_crypt_t *crypt, const uint8_t *nonce, size_t len)
{
    return botan_cipher_start(crypt->aead.obj, nonce, len) == 0;
}

bool
pgp_cipher_aead_update(
  pgp_crypt_t &crypt, uint8_t *out, const uint8_t *in, size_t len, size_t &read)
{
    if (len % crypt.aead.granularity) {
        RNP_LOG("aead wrong update len");
        return false;
    }

    size_t outwr = 0;
    size_t inread = 0;
    if (botan_cipher_update(crypt.aead.obj, 0, out, len, &outwr, in, len, &inread)) {
        RNP_LOG("aead update failed");
        return false;
    }

    if (outwr != inread) {
        RNP_LOG("wrong aead usage: %zu vs %zu, len is %zu", outwr, inread, len);
        return false;
    }

    read = inread;
    return true;
}

void
pgp_cipher_aead_reset(pgp_crypt_t *crypt)
{
    botan_cipher_reset(crypt->aead.obj);
}

bool
pgp_cipher_aead_finish(pgp_crypt_t *crypt, uint8_t *out, const uint8_t *in, size_t len)
{
    uint32_t flags = BOTAN_CIPHER_UPDATE_FLAG_FINAL;
    size_t   inread = 0;
    size_t   outwr = 0;
    int      res;

    if (crypt->aead.decrypt) {
        size_t datalen = len - crypt->aead.taglen;
        /* for decryption we should have tag for the final update call */
        res =
          botan_cipher_update(crypt->aead.obj, flags, out, datalen, &outwr, in, len, &inread);
        if (res != 0) {
            if (res != BOTAN_FFI_ERROR_BAD_MAC) {
                RNP_LOG("aead finish failed: %d", res);
            }
            return false;
        }

        if ((outwr != datalen) || (inread != len)) {
            RNP_LOG("wrong decrypt aead finish usage");
            return false;
        }
    } else {
        /* for encryption tag will be generated */
        size_t outlen = len + crypt->aead.taglen;
        if (botan_cipher_update(
              crypt->aead.obj, flags, out, outlen, &outwr, in, len, &inread) != 0) {
            RNP_LOG("aead finish failed");
            return false;
        }

        if ((outwr != outlen) || (inread != len)) {
            RNP_LOG("wrong encrypt aead finish usage");
            return false;
        }
    }

    pgp_cipher_aead_reset(crypt);
    return true;
}

void
pgp_cipher_aead_destroy(pgp_crypt_t *crypt)
{
    if (crypt->aead.obj) {
        botan_cipher_destroy(crypt->aead.obj);
    }
    memset(crypt, 0x0, sizeof(*crypt));
}

#endif // ENABLE_AEAD
