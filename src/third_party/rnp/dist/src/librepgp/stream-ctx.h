/*
 * Copyright (c) 2019-2020, [Ribose Inc](https://www.ribose.com).
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

#ifndef STREAM_CTX_H_
#define STREAM_CTX_H_

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include "types.h"
#include <string>
#include <list>
#include "key.hpp"
#include "crypto/mem.h"
#include "key-provider.h"
#include "pass-provider.h"
#include "sec_profile.hpp"

/* signature info structure */
typedef struct rnp_signer_info_t {
    rnp::Key *     key{};
    pgp_hash_alg_t halg{};
    int64_t        sigcreate{};
    uint64_t       sigexpire{};
} rnp_signer_info_t;

typedef struct rnp_symmetric_pass_info_t {
    pgp_s2k_t      s2k{};
    pgp_symm_alg_t s2k_cipher{};

    rnp::secure_array<uint8_t, PGP_MAX_KEY_SIZE> key;
} rnp_symmetric_pass_info_t;

/** rnp operation context : contains configuration data about the currently ongoing operation.
 *
 *  Common fields which make sense for every operation:
 *  - overwrite : silently overwrite output file if exists
 *  - armor : except cleartext signing, which outputs text in clear and always armor signature,
 *    this controls whether output is armored (base64-encoded). For armor/dearmor operation it
 *    controls the direction of the conversion (true means enarmor, false - dearmor),
 *  - rng : random number generator
 *  - operation : current operation type
 *
 *  For operations with OpenPGP embedded data (i.e. encrypted data and attached signatures):
 *  - filename, filemtime : to specify information about the contents of literal data packet
 *  - zalg, zlevel : compression algorithm and level, zlevel = 0 to disable compression
 *
 *  For encryption operation (including encrypt-and-sign):
 *  - halg : hash algorithm used during key derivation for password-based encryption
 *  - ealg, aalg, abits : symmetric encryption algorithm and AEAD parameters if used
 *  - recipients : list of key ids used to encrypt data to
 *  - enable_pkesk_v6 (Only if defined: ENABLE_CRYPTO_REFRESH): if true and each recipient in
 * the  list of recipients has the capability, allows PKESKv6/SEIPDv2
 *  - pref_pqc_enc_subkey (Only if defined: ENABLE_PQC): if true, prefers PQC subkey over
 * non-PQC subkey for encryption.
 *  - passwords : list of passwords used for password-based encryption
 *  - filename, filemtime, zalg, zlevel : see previous
 *  - pkeskv6_capable() : returns true if all keys support PKESKv6+SEIPDv2, false otherwise
 * (will use PKESKv3 + SEIPDv1)
 *
 *  For signing of any kind (attached, detached, cleartext):
 *  - clearsign, detached : controls kind of the signed data. Both are mutually-exclusive.
 *    If both are false then attached signing is used.
 *  - halg : hash algorithm used to calculate signature(s)
 *  - signers : list of rnp_signer_info_t structures describing signing key and parameters
 *  - sigcreate, sigexpire : default signature(s) creation and expiration times
 *  - filename, filemtime, zalg, zlevel : only for attached signatures, see previous
 *
 *  For data decryption and/or verification there is not much of fields:
 *  - discard: discard the output data (i.e. just decrypt and/or verify signatures)
 *
 */

typedef struct rnp_ctx_t {
    std::string    filename;    /* name of the input file to store in literal data packet */
    int64_t        filemtime{}; /* file modification time to store in literal data packet */
    int64_t        sigcreate{}; /* signature creation time */
    uint64_t       sigexpire{}; /* signature expiration time */
    bool           clearsign{}; /* cleartext signature */
    bool           detached{};  /* detached signature */
    pgp_hash_alg_t halg;        /* hash algorithm */
    pgp_symm_alg_t ealg;        /* encryption algorithm */
    int            zalg{};      /* compression algorithm used */
    int            zlevel{};    /* compression level */
    pgp_aead_alg_t aalg;        /* non-zero to use AEAD */
    int            abits;       /* AEAD chunk bits */
    bool           overwrite{}; /* allow to overwrite output file if exists */
    bool           armor{};     /* whether to use ASCII armor on output */
    bool           no_wrap{};   /* do not wrap source in literal data packet */
#if defined(ENABLE_CRYPTO_REFRESH)
    bool enable_pkesk_v6{}; /* allows pkesk v6 if list of recipients is suitable */
#endif
#if defined(ENABLE_PQC)
    bool pref_pqc_enc_subkey{}; /* prefer to encrypt to PQC subkey */
#endif
    std::list<rnp::Key *>                recipients; /* recipients of the encrypted message */
    std::list<rnp_symmetric_pass_info_t> passwords;  /* passwords to encrypt message */
    std::list<rnp_signer_info_t>         signers;    /* keys to which sign message */
    rnp::SecurityContext &               sec_ctx;    /* security context */
    rnp::KeyProvider &                   key_provider;  /* Key provider */
    pgp_password_provider_t &            pass_provider; /* Password provider */

    rnp_ctx_t(rnp::SecurityContext &   sctx,
              rnp::KeyProvider &       kprov,
              pgp_password_provider_t &pprov)
        : halg(DEFAULT_PGP_HASH_ALG), ealg(DEFAULT_PGP_SYMM_ALG), aalg(PGP_AEAD_NONE),
          abits(DEFAULT_AEAD_CHUNK_BITS), sec_ctx(sctx), key_provider(kprov),
          pass_provider(pprov){};

    rnp_ctx_t(const rnp_ctx_t &) = delete;
    rnp_ctx_t(rnp_ctx_t &&) = delete;

    rnp_ctx_t &operator=(const rnp_ctx_t &) = delete;
    rnp_ctx_t &operator=(rnp_ctx_t &&) = delete;

    rnp_result_t add_encryption_password(const std::string &password,
                                         pgp_hash_alg_t     halg,
                                         pgp_symm_alg_t     ealg,
                                         size_t             iterations = 0);

#if defined(ENABLE_CRYPTO_REFRESH)
    bool pkeskv6_capable();
#endif
} rnp_ctx_t;

#endif
