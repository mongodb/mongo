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
#ifndef TYPES_H_
#define TYPES_H_

#include <stdint.h>
#include <string>
#include <vector>
#include <array>
#include <cstring>
#include <stdexcept>
#include <type_traits>

#include <rnp/rnp_def.h>
#include "crypto/common.h"
#include "sec_profile.hpp"

/* SHA1 Hash Size */
#define PGP_SHA1_HASH_SIZE 20

/* Maximum length of the packet header */
#define PGP_MAX_HEADER_SIZE 6

/* Maximum supported userid length */
#define MAX_ID_LENGTH 128

/* Maximum supported password length */
#define MAX_PASSWORD_LENGTH 256

class id_str_pair {
  public:
    int         id;
    const char *str;

    /**
     * @brief Lookup constant pair array for the specified id or string value.
     *        Note: array must be finished with NULL string to stop the lookup.
     *
     * @param pair pointer to the const array with pairs.
     * @param id identifier to search for
     * @param notfound value to return if identifier is not found.
     * @return string, representing the identifier.
     */
    static const char *lookup(const id_str_pair pair[],
                              int               id,
                              const char *      notfound = "unknown");
    static int         lookup(const id_str_pair pair[], const char *str, int notfound = 0);
    static int         lookup(const id_str_pair           pair[],
                              const std::vector<uint8_t> &bytes,
                              int                         notfound = 0);
};

namespace pgp {
using SigID = std::array<uint8_t, PGP_SHA1_HASH_SIZE>;
using SigIDs = std::vector<SigID>;
} // namespace pgp

namespace std {
template <> struct hash<pgp::SigID> {
    std::size_t
    operator()(pgp::SigID const &sigid) const noexcept
    {
        /* since signature id value is hash itself, we may use its low bytes */
        size_t res = 0;
        static_assert(std::tuple_size<pgp::SigID>::value >= sizeof(res),
                      "pgp::SigID size mismatch");
        std::memcpy(&res, sigid.data(), sizeof(res));
        return res;
    }
};
}; // namespace std

namespace rnp {
class rnp_exception : public std::exception {
    rnp_result_t code_;

  public:
    rnp_exception(rnp_result_t code = RNP_ERROR_GENERIC) : code_(code){};
    virtual const char *
    what() const throw()
    {
        return "rnp_exception";
    };
    rnp_result_t
    code() const
    {
        return code_;
    };
};
} // namespace rnp

/* validity information for the signature/key/userid */
typedef struct pgp_validity_t {
    bool validated{}; /* item was validated */
    bool valid{};     /* item is valid by signature/key checks and calculations.
                         Still may be revoked or expired. */
    bool expired{};   /* item is expired */

    void mark_valid();
    void reset();
} pgp_validity_t;

typedef struct pgp_s2k_t {
    pgp_s2k_usage_t usage{};

    /* below fields may not all be valid, depending on the usage field above */
    pgp_s2k_specifier_t specifier{};
    pgp_hash_alg_t      hash_alg{};
    uint8_t             salt[PGP_SALT_SIZE];
    unsigned            iterations{};
    /* GnuPG custom s2k data */
    pgp_s2k_gpg_extension_t gpg_ext_num{};
    uint8_t                 gpg_serial_len{};
    uint8_t                 gpg_serial[16];
    /* Experimental s2k data */
    std::vector<uint8_t> experimental{};
} pgp_s2k_t;

typedef struct pgp_key_protection_t {
    pgp_s2k_t         s2k{};         /* string-to-key kdf params */
    pgp_symm_alg_t    symm_alg{};    /* symmetric alg */
    pgp_cipher_mode_t cipher_mode{}; /* block cipher mode */
    uint8_t           iv[PGP_MAX_BLOCK_SIZE];
} pgp_key_protection_t;

namespace pgp {
namespace pkt {
class Signature;
}
} // namespace pgp

typedef enum {
    /* first octet */
    PGP_KEY_SERVER_NO_MODIFY = 0x80
} pgp_key_server_prefs_t;

typedef struct pgp_literal_hdr_t {
    uint8_t  format{};
    char     fname[256]{};
    uint8_t  fname_len{};
    uint32_t timestamp{};
} pgp_literal_hdr_t;

typedef struct pgp_aead_hdr_t {
    int            version{};                  /* version of the AEAD packet */
    pgp_symm_alg_t ealg;                       /* underlying symmetric algorithm */
    pgp_aead_alg_t aalg;                       /* AEAD algorithm, i.e. EAX, OCB, etc */
    int            csize{};                    /* chunk size bits */
    uint8_t        iv[PGP_AEAD_MAX_NONCE_LEN]; /* initial vector for the message */
    size_t         ivlen{};                    /* iv length */

    pgp_aead_hdr_t() : ealg(PGP_SA_UNKNOWN), aalg(PGP_AEAD_NONE)
    {
    }
} pgp_aead_hdr_t;

#ifdef ENABLE_CRYPTO_REFRESH
typedef struct pgp_seipdv2_hdr_t {
    pgp_seipd_version_t version;                    /* version of the SEIPD packet */
    pgp_symm_alg_t      cipher_alg;                 /* underlying symmetric algorithm */
    pgp_aead_alg_t      aead_alg;                   /* AEAD algorithm, i.e. EAX, OCB, etc */
    uint8_t             chunk_size_octet;           /* chunk size octet */
    uint8_t             salt[PGP_SEIPDV2_SALT_LEN]; /* SEIPDv2 salt value */
} pgp_seipdv2_hdr_t;
#endif

/** litdata_type_t */
typedef enum {
    PGP_LDT_BINARY = 'b',
    PGP_LDT_TEXT = 't',
    PGP_LDT_UTF8 = 'u',
    PGP_LDT_LOCAL = 'l',
    PGP_LDT_LOCAL2 = '1'
} pgp_litdata_enum;

typedef struct rnp_key_protection_params_t {
    pgp_symm_alg_t    symm_alg;
    pgp_cipher_mode_t cipher_mode;
    unsigned          iterations;
    pgp_hash_alg_t    hash_alg;
} rnp_key_protection_params_t;

#endif /* TYPES_H_ */
