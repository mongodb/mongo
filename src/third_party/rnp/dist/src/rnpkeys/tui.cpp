/*
 * Copyright (c) 2017-2020, [Ribose Inc](https://www.ribose.com).
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

#include <algorithm>
#ifdef _MSC_VER
#include "uniwin.h"
#else
#include <unistd.h>
#endif
#include <errno.h>
#include <iterator>
#include "config.h"
#include "rnp/rnpcfg.h"
#include "rnpkeys.h"
#include "defaults.h"
#include "file-utils.h"
#include "logging.h"

#define MAX_INPUT_ATTEMPTS 5

/* -----------------------------------------------------------------------------
 * @brief   Reads input from file pointer and converts it securelly to ints
 *          Partially based on ERR34-C from SEI CERT C Coding Standard
 *
 * @param   fp          pointer to opened pipe
 * @param   result[out] result read from file pointer and converted to int
 *
 * @returns true and value in result if integer was parsed correctly,
 *          otherwise false
 *
-------------------------------------------------------------------------------- */
static bool
rnp_secure_get_long_from_fd(FILE *fp, long &result, bool allow_empty = true)
{
    char buff[BUFSIZ];
    if (!fgets(buff, sizeof(buff), fp)) {
        RNP_LOG("EOF or read error");
        return false;
    }

    errno = 0;
    char *end_ptr = NULL;
    long  num_long = strtol(buff, &end_ptr, 10);
    if (ERANGE == errno) {
        RNP_LOG("Number out of range");
        return false;
    }
    if (end_ptr == buff) {
        return allow_empty;
    }
    if ('\n' != *end_ptr && '\0' != *end_ptr) {
        RNP_LOG("Unexpected end of line");
        return false;
    }

    result = num_long;
    return true;
}

static bool
check_attempts(int &attempts)
{
    if (attempts >= MAX_INPUT_ATTEMPTS) {
        printf("Too many attempts. Aborting.\n");
        return false;
    }
    attempts++;
    return true;
}

static bool
is_rsa_keysize_supported(uint32_t keysize)
{
    return ((keysize >= 1024) && (keysize <= 4096) && !(keysize % 8));
}

static const char *
ask_curve_name(FILE *input_fp)
{
    std::vector<const char *> curves;
    static const char *const  known_curves[] = {
      "NIST P-256",
      "NIST P-384",
      "NIST P-521",
      "brainpoolP256r1",
      "brainpoolP384r1",
      "brainpoolP512r1",
      "secp256k1",
    };
    const size_t curvenum = sizeof(known_curves) / sizeof(*known_curves);

    try {
        std::copy_if(known_curves,
                     known_curves + curvenum,
                     std::back_inserter(curves),
                     [](const char *curve) {
                         bool supported = false;
                         return !rnp_supports_feature(RNP_FEATURE_CURVE, curve, &supported) &&
                                supported;
                     });
    } catch (const std::exception &e) {
        RNP_LOG("%s", e.what());
        return NULL;
    }
    const size_t ccount = curves.size();
    if (!ccount) {
        return NULL;
    }
    bool        ok = false;
    const char *result = NULL;
    int         attempts = 0;
    do {
        if (!check_attempts(attempts)) {
            return NULL;
        }
        printf("Please select which elliptic curve you want:\n");
        for (size_t i = 0; i < ccount; i++) {
            printf("\t(%zu) %s\n", i + 1, curves[i]);
        }
        printf("(default %s)> ", DEFAULT_CURVE);
        long val = 0;
        ok = rnp_secure_get_long_from_fd(input_fp, val) && (val > 0) && (val <= (long) ccount);
        if (ok) {
            result = curves[val - 1];
        }
    } while (!ok);

    return result;
}

#if defined(ENABLE_PQC)
static std::string
ask_sphincsplus_param_name(FILE *input_fp)
{
    std::vector<std::string> params = {"128f", "128s", "192f", "192s", "256f", "256s"};
    std::vector<std::string> add_info = {"ML-KEM-768 + X25519",
                                         "ML-KEM-768 + X25519",
                                         "ML-KEM-1024 + ECDH-NIST-P-384",
                                         "ML-KEM-1024 + ECDH-NIST-P-384",
                                         "ML-KEM-1024 + ECDH-NIST-P-384",
                                         "ML-KEM-1024 + ECDH-NIST-P-384"};

    const size_t pcount = params.size();
    if (!pcount) {
        return NULL;
    }
    bool        ok = false;
    std::string result = "";
    int         attempts = 0;
    do {
        if (!check_attempts(attempts)) {
            return NULL;
        }
        printf("Please select which SLH-DSA parameter set you want. In parenthesis, the "
               "correspondingly chosen encryption subkey is shown:\n");
        for (size_t i = 0; i < pcount; i++) {
            printf("\t(%zu) %s (%s)\n", i + 1, params[i].c_str(), add_info[i].c_str());
        }
        printf("(default %s)> ", params[0].c_str());
        long val = 0;
        ok = rnp_secure_get_long_from_fd(input_fp, val) && (val > 0) && (val <= (long) pcount);
        if (ok) {
            result = params[val - 1];
        }
    } while (!ok);

    return result;
}
#endif

static long
ask_rsa_bitlen(FILE *input_fp)
{
    long result = 0;
    int  attempts = 0;
    do {
        result = DEFAULT_RSA_NUMBITS;
        if (!check_attempts(attempts)) {
            return -1;
        }
        printf("Please provide bit length of the key (between 1024 and 4096):\n(default %d)> ",
               DEFAULT_RSA_NUMBITS);
    } while (!rnp_secure_get_long_from_fd(input_fp, result) ||
             !is_rsa_keysize_supported(result));
    return result;
}

static long
ask_elgamal_bitlen(FILE *input_fp)
{
    int attempts = 0;
    do {
        long result = DEFAULT_ELGAMAL_NUMBITS;
        if (!check_attempts(attempts)) {
            return -1;
        }
        printf(
          "Please provide bit length of the ElGamal key (between %d and %d):\n(default %d) > ",
          ELGAMAL_MIN_P_BITLEN,
          ELGAMAL_MAX_P_BITLEN,
          DEFAULT_ELGAMAL_NUMBITS);
        if (!rnp_secure_get_long_from_fd(input_fp, result)) {
            continue;
        }
        if ((result >= ELGAMAL_MIN_P_BITLEN) && (result <= ELGAMAL_MAX_P_BITLEN)) {
            // round up to multiple of 32
            result = ((result + 31) / 32) * 32;
            printf("Bitlen of the key will be %lu\n", result);
            return result;
        }
    } while (1);
    return -1;
}

static long
ask_dsa_bitlen(FILE *input_fp)
{
    int attempts = 0;
    do {
        if (!check_attempts(attempts)) {
            return -1;
        }
        printf(
          "Please provide bit length of the DSA key (between %d and %d):\n(default %d) > ",
          DSA_MIN_P_BITLEN,
          DSA_MAX_P_BITLEN,
          DSA_DEFAULT_P_BITLEN);
        long result = DSA_DEFAULT_P_BITLEN;
        if (!rnp_secure_get_long_from_fd(input_fp, result)) {
            continue;
        }
        if ((result >= DSA_MIN_P_BITLEN) && (result <= DSA_MAX_P_BITLEN)) {
            // round up to multiple of 64
            result = ((result + 63) / 64) * 64;
            printf("Bitlen of the key will be %lu\n", result);
            return result;
        }
    } while (1);

    return -1;
}

static bool
rnpkeys_ask_generate_params(rnp_cfg &cfg, FILE *input_fp)
{
    long option = 0;
    int  attempts = 0;
    do {
        if (!check_attempts(attempts)) {
            return false;
        }
        printf(
          "Please select what kind of key you want:\n"
          "\t(1)  RSA (Encrypt or Sign)\n"
          "\t(16) DSA + ElGamal\n"
          "\t(17) DSA + RSA\n" // TODO: See #584
          "\t(19) ECDSA + ECDH\n"
#if defined(ENABLE_CRYPTO_REFRESH)
          "\t(21) EDDSA + ECDH (v6 key) \n"
          "\t(22) EDDSA + ECDH (v4 key) \n"
#else
          "\t(22) EDDSA + ECDH\n"
#endif
#if defined(ENABLE_CRYPTO_REFRESH)
          "\t(23) ED25519 + X25519 (v6 key) \n"
#endif
#if defined(ENABLE_PQC)
          "\t(24) Ed25519Legacy + Curve25519Legacy + (ML-KEM-768 + X25519)\n"
#if defined(ENABLE_CRYPTO_REFRESH) // PQC primary keys only for v6
          "\t(25) (ML-DSA-65 + Ed25519) + (ML-KEM-768 + X25519)\n"
          "\t(27) (ML-DSA-65 + ECDSA-NIST-P-256) + (ML-KEM-768 + ECDH-NIST-P-256)\n"
          "\t(28) (ML-DSA-87 + ECDSA-NIST-P-384) + (ML-KEM-1024 + ECDH-NIST-P-384)\n"
          "\t(29) (ML-DSA-65 + ECDSA-brainpoolP256r1) + (ML-KEM-768 + ECDH-brainpoolP256r1)\n"
          "\t(30) (ML-DSA-87 + ECDSA-brainpoolP384r1) + (ML-KEM-1024 + ECDH-brainpoolP384r1)\n"
          "\t(31) SLH-DSA-SHA2 + MLKEM-ECDH Composite\n"
          "\t(32) SLH-DSA-SHAKE + MLKEM-ECDH Composite\n"
#endif
#endif
          "\t(99) SM2\n"
          "> ");
        if (!rnp_secure_get_long_from_fd(input_fp, option, false)) {
            option = 0;
            continue;
        }
        switch (option) {
        case 1: {
            int bits = ask_rsa_bitlen(input_fp);
            if (bits < 0) {
                return false;
            }
            cfg.set_str(CFG_KG_PRIMARY_ALG, RNP_ALGNAME_RSA);
            cfg.set_str(CFG_KG_SUBKEY_ALG, RNP_ALGNAME_RSA);
            cfg.set_int(CFG_KG_PRIMARY_BITS, bits);
            cfg.set_int(CFG_KG_SUBKEY_BITS, bits);
            break;
        }
        case 16: {
            int bits = ask_dsa_bitlen(input_fp);
            if (bits < 0) {
                return false;
            }
            cfg.set_str(CFG_KG_PRIMARY_ALG, RNP_ALGNAME_DSA);
            cfg.set_str(CFG_KG_SUBKEY_ALG, RNP_ALGNAME_ELGAMAL);
            cfg.set_int(CFG_KG_PRIMARY_BITS, bits);
            cfg.set_int(CFG_KG_SUBKEY_BITS, bits);
            break;
        }
        case 17: {
            int bits = ask_dsa_bitlen(input_fp);
            if (bits < 0) {
                return false;
            }
            cfg.set_str(CFG_KG_PRIMARY_ALG, RNP_ALGNAME_DSA);
            cfg.set_str(CFG_KG_SUBKEY_ALG, RNP_ALGNAME_RSA);
            cfg.set_int(CFG_KG_PRIMARY_BITS, bits);
            cfg.set_int(CFG_KG_SUBKEY_BITS, bits);
            break;
        }
        case 19: {
            const char *curve = ask_curve_name(input_fp);
            if (!curve) {
                return false;
            }
            cfg.set_str(CFG_KG_PRIMARY_ALG, RNP_ALGNAME_ECDSA);
            cfg.set_str(CFG_KG_SUBKEY_ALG, RNP_ALGNAME_ECDH);
            cfg.set_str(CFG_KG_PRIMARY_CURVE, curve);
            cfg.set_str(CFG_KG_SUBKEY_CURVE, curve);
            break;
        }
#if defined(ENABLE_CRYPTO_REFRESH)
        case 21: {
            cfg.set_str(CFG_KG_V6_KEY, "true");
            FALLTHROUGH_STATEMENT;
        }
#endif
        case 22: {
            cfg.set_str(CFG_KG_PRIMARY_ALG, RNP_ALGNAME_EDDSA);
            cfg.set_str(CFG_KG_SUBKEY_ALG, RNP_ALGNAME_ECDH);
            cfg.set_str(CFG_KG_SUBKEY_CURVE, "Curve25519");
            break;
        }
#if defined(ENABLE_CRYPTO_REFRESH)
        case 23: {
            cfg.set_str(CFG_KG_PRIMARY_ALG, RNP_ALGNAME_ED25519);
            cfg.set_str(CFG_KG_SUBKEY_ALG, RNP_ALGNAME_X25519);
            cfg.set_str(CFG_KG_V6_KEY, "true");
            break;
        }
#endif
#if defined(ENABLE_PQC)
        case 24:
            cfg.set_str(CFG_KG_PRIMARY_ALG, RNP_ALGNAME_EDDSA);
            cfg.set_str(CFG_KG_SUBKEY_ALG, RNP_ALGNAME_ECDH);
            cfg.set_str(CFG_KG_SUBKEY_CURVE, "Curve25519");
            cfg.set_str(CFG_KG_SUBKEY_2_ALG, RNP_ALGNAME_KYBER768_X25519);
            break;
#if defined(ENABLE_CRYPTO_REFRESH) // PQC primary keys only for v6
        case 25:
            cfg.set_str(CFG_KG_PRIMARY_ALG, RNP_ALGNAME_DILITHIUM3_ED25519);
            cfg.set_str(CFG_KG_HASH, RNP_ALGNAME_SHA3_256);
            cfg.set_str(CFG_KG_SUBKEY_ALG, RNP_ALGNAME_KYBER768_X25519);
            cfg.set_str(CFG_KG_V6_KEY, "true");
            break;
        case 27:
            cfg.set_str(CFG_KG_PRIMARY_ALG, RNP_ALGNAME_DILITHIUM3_P256);
            cfg.set_str(CFG_KG_HASH, RNP_ALGNAME_SHA3_256);
            cfg.set_str(CFG_KG_SUBKEY_ALG, RNP_ALGNAME_KYBER768_P256);
            cfg.set_str(CFG_KG_V6_KEY, "true");
            break;
        case 28:
            cfg.set_str(CFG_KG_PRIMARY_ALG, RNP_ALGNAME_DILITHIUM5_P384);
            cfg.set_str(CFG_KG_HASH, RNP_ALGNAME_SHA3_256);
            cfg.set_str(CFG_KG_SUBKEY_ALG, RNP_ALGNAME_KYBER1024_P384);
            cfg.set_str(CFG_KG_V6_KEY, "true");
            break;
        case 29:
            cfg.set_str(CFG_KG_PRIMARY_ALG, RNP_ALGNAME_DILITHIUM3_BP256);
            cfg.set_str(CFG_KG_HASH, RNP_ALGNAME_SHA3_256);
            cfg.set_str(CFG_KG_SUBKEY_ALG, RNP_ALGNAME_KYBER768_BP256);
            cfg.set_str(CFG_KG_V6_KEY, "true");
            break;
        case 30:
            cfg.set_str(CFG_KG_PRIMARY_ALG, RNP_ALGNAME_DILITHIUM5_BP384);
            cfg.set_str(CFG_KG_HASH, RNP_ALGNAME_SHA3_256);
            cfg.set_str(CFG_KG_SUBKEY_ALG, RNP_ALGNAME_KYBER1024_BP384);
            cfg.set_str(CFG_KG_V6_KEY, "true");
            break;
        case 31: {
            std::string param = ask_sphincsplus_param_name(input_fp);
            if (param == "") {
                return false;
            }
            if (param == "128f" || param == "128s") {
                cfg.set_str(CFG_KG_SUBKEY_ALG, RNP_ALGNAME_KYBER768_X25519);
                cfg.set_str(CFG_KG_HASH, RNP_ALGNAME_SHA256);
            } else {
                cfg.set_str(CFG_KG_SUBKEY_ALG, RNP_ALGNAME_KYBER1024_P384);
                cfg.set_str(CFG_KG_HASH, RNP_ALGNAME_SHA512);
            }
            cfg.set_str(CFG_KG_PRIMARY_ALG, RNP_ALGNAME_SPHINCSPLUS_SHA2);
            cfg.set_str(CFG_KG_PRIMARY_SPHINCSPLUS_PARAM, param);
            cfg.set_str(CFG_KG_V6_KEY, "true");
            break;
        }
        case 32: {
            std::string param = ask_sphincsplus_param_name(input_fp);
            if (param == "") {
                return false;
            }
            if (param == "128f" || param == "128s") {
                cfg.set_str(CFG_KG_SUBKEY_ALG, RNP_ALGNAME_KYBER768_X25519);
                cfg.set_str(CFG_KG_HASH, RNP_ALGNAME_SHA3_256);
            } else {
                cfg.set_str(CFG_KG_SUBKEY_ALG, RNP_ALGNAME_KYBER1024_P384);
                cfg.set_str(CFG_KG_HASH, RNP_ALGNAME_SHA3_512);
            }
            cfg.set_str(CFG_KG_PRIMARY_ALG, RNP_ALGNAME_SPHINCSPLUS_SHAKE);
            cfg.set_str(CFG_KG_PRIMARY_SPHINCSPLUS_PARAM, param);
            cfg.set_str(CFG_KG_V6_KEY, "true");
            break;
        }
#endif
#endif
        case 99: {
            cfg.set_str(CFG_KG_PRIMARY_ALG, RNP_ALGNAME_SM2);
            cfg.set_str(CFG_KG_SUBKEY_ALG, RNP_ALGNAME_SM2);
            if (!cfg.has(CFG_KG_HASH)) {
                cfg.set_str(CFG_KG_HASH, RNP_ALGNAME_SM3);
            }
            break;
        }
        default:
            option = 0;
            break;
        }
    } while (!option);

    return true;
}

static bool
rnpkeys_ask_generate_params_subkey(rnp_cfg &cfg, FILE *input_fp)
{
    long option = 0;
    int  attempts = 0;
    do {
        if (!check_attempts(attempts)) {
            return false;
        }
        printf("Please select subkey algorithm you want:\n"
               "\t(1)  RSA\n"
               "\t(16) ElGamal\n"
               "\t(17) DSA\n"
               "\t(18) ECDH\n"
               "\t(19) ECDSA\n"
               "\t(22) EDDSA\n"
#if defined(ENABLE_PQC)
               "\t(25) ML-KEN-768 + X25519\n"
               "\t(26) ML-KEM-1024 + X448\n"
               "\t(27) ML-KEM-768 + ECDH-NIST-P-256\n"
               "\t(28) ML-KEM-1024 + ECDH-NIST-P-384\n"
               "\t(29) ML-KEM-768 + ECDH-brainpoolP256r1\n"
               "\t(30) ML-KEM-1024 + ECDH-brainpoolP384r1\n"
#endif
               "\t(99) SM2"
               "> ");
        if (!rnp_secure_get_long_from_fd(input_fp, option, false)) {
            option = 0;
            continue;
        }
        switch (option) {
        case 1: {
            int bits = ask_rsa_bitlen(input_fp);
            if (bits < 0) {
                return false;
            }
            cfg.set_str(CFG_KG_SUBKEY_ALG, RNP_ALGNAME_RSA);
            cfg.set_int(CFG_KG_SUBKEY_BITS, bits);
            break;
        }
        case 16: {
            int bits = ask_elgamal_bitlen(input_fp);
            if (bits < 0) {
                return false;
            }
            cfg.set_str(CFG_KG_SUBKEY_ALG, RNP_ALGNAME_ELGAMAL);
            cfg.set_int(CFG_KG_SUBKEY_BITS, bits);
            break;
        }
        case 17: {
            int bits = ask_dsa_bitlen(input_fp);
            if (bits < 0) {
                return false;
            }
            cfg.set_str(CFG_KG_SUBKEY_ALG, RNP_ALGNAME_DSA);
            cfg.set_int(CFG_KG_SUBKEY_BITS, bits);
            break;
        }
        case 18: {
            const char *curve = ask_curve_name(input_fp);
            if (!curve) {
                return false;
            }
            cfg.set_str(CFG_KG_SUBKEY_ALG, RNP_ALGNAME_ECDH);
            cfg.set_str(CFG_KG_SUBKEY_CURVE, curve);
            break;
        }
        case 19: {
            const char *curve = ask_curve_name(input_fp);
            if (!curve) {
                return false;
            }
            cfg.set_str(CFG_KG_SUBKEY_ALG, RNP_ALGNAME_ECDSA);
            cfg.set_str(CFG_KG_SUBKEY_CURVE, curve);
            break;
        }
        case 22: {
            cfg.set_str(CFG_KG_SUBKEY_ALG, RNP_ALGNAME_EDDSA);
            break;
        }
#if defined(ENABLE_PQC)
        case 25:
            cfg.set_str(CFG_KG_SUBKEY_ALG, RNP_ALGNAME_KYBER768_X25519);
            break;
        case 26:
            cfg.set_str(CFG_KG_SUBKEY_ALG, RNP_ALGNAME_KYBER1024_X448);
            break;
        case 27:
            cfg.set_str(CFG_KG_SUBKEY_ALG, RNP_ALGNAME_KYBER768_P256);
            break;
        case 28:
            cfg.set_str(CFG_KG_SUBKEY_ALG, RNP_ALGNAME_KYBER1024_P384);
            break;
        case 29:
            cfg.set_str(CFG_KG_SUBKEY_ALG, RNP_ALGNAME_KYBER768_BP256);
            break;
        case 30:
            cfg.set_str(CFG_KG_SUBKEY_ALG, RNP_ALGNAME_KYBER1024_BP384);
            break;
#endif
        case 99: {
            cfg.set_str(CFG_KG_SUBKEY_ALG, RNP_ALGNAME_SM2);
            if (!cfg.has(CFG_KG_HASH)) {
                cfg.set_str(CFG_KG_HASH, RNP_ALGNAME_SM3);
            }
            break;
        }
        default:
            option = 0;
            break;
        }
    } while (!option);

    return true;
}

bool
cli_rnp_set_generate_params(rnp_cfg &cfg, bool subkey)
{
    bool res = true;
    // hash algorithms for signing and protection
    if (cfg.has(CFG_HASH)) {
        cfg.set_str(CFG_KG_HASH, cfg.get_str(CFG_HASH));
        cfg.set_str(CFG_KG_PROT_HASH, cfg.get_str(CFG_HASH));
    }

    // key and subkey algorithms, bit length/curve
    if (!cfg.get_bool(CFG_EXPERT)) {
        cfg.set_str(CFG_KG_PRIMARY_ALG, RNP_ALGNAME_RSA);
        cfg.set_int(CFG_KG_PRIMARY_BITS, cfg.get_int(CFG_NUMBITS));
        cfg.set_str(CFG_KG_SUBKEY_ALG, RNP_ALGNAME_RSA);
        cfg.set_int(CFG_KG_SUBKEY_BITS, cfg.get_int(CFG_NUMBITS));
    } else {
        FILE *input = stdin;
        if (cfg.has(CFG_USERINPUTFD)) {
            int inputfd = dup(cfg.get_int(CFG_USERINPUTFD));
            if (inputfd != -1) {
                input = rnp_fdopen(inputfd, "r");
                if (!input) {
                    close(inputfd);
                }
            }
        }
        if (!input) {
            return false;
        }
        if (subkey) {
            res = rnpkeys_ask_generate_params_subkey(cfg, input);
        } else {
            res = rnpkeys_ask_generate_params(cfg, input);
        }
        if (input != stdin) {
            fclose(input);
        }
        if (!res) {
            return false;
        }
    }

    // make sure hash algorithms are set
    if (!cfg.has(CFG_KG_HASH)) {
        cfg.set_str(CFG_KG_HASH, DEFAULT_HASH_ALG);
    }
    if (!cfg.has(CFG_KG_PROT_HASH)) {
        cfg.set_str(CFG_KG_PROT_HASH, DEFAULT_HASH_ALG);
    }

    // protection symmetric algorithm
    cfg.set_str(CFG_KG_PROT_ALG,
                cfg.has(CFG_CIPHER) ? cfg.get_str(CFG_CIPHER) : DEFAULT_SYMM_ALG);
    // protection iterations count
    size_t iterations = cfg.get_int(CFG_S2K_ITER);
    if (!iterations) {
        res = res && !rnp_calculate_iterations(cfg.get_str(CFG_KG_PROT_HASH).c_str(),
                                               cfg.get_int(CFG_S2K_MSEC),
                                               &iterations);
    }
    cfg.set_int(CFG_KG_PROT_ITERATIONS, iterations);
    return res;
}
