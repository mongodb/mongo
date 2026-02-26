/*
 * Copyright (c) 2018-2021, [Ribose Inc](https://www.ribose.com).
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

#ifndef DEFAULTS_H_
#define DEFAULTS_H_

/* Default hash algorithm as PGP constant */
#define DEFAULT_PGP_HASH_ALG PGP_HASH_SHA256

/* Default symmetric algorithm as PGP constant */
#define DEFAULT_PGP_SYMM_ALG PGP_SA_AES_256

/* Default number of msec to run S2K derivation */
#define DEFAULT_S2K_MSEC 150

/* Default number of msec to run S2K tuning */
#define DEFAULT_S2K_TUNE_MSEC 10

/* Default compression algorithm and level */
#define DEFAULT_Z_ALG "ZIP"
#define DEFAULT_Z_LEVEL 6

/* Default AEAD algorithm */
#define DEFAULT_AEAD_ALG PGP_AEAD_OCB

/* Default AEAD chunk bits, equals to 256K chunks */
#define DEFAULT_AEAD_CHUNK_BITS 12

/* Default cipher mode for secret key encryption */
#define DEFAULT_CIPHER_MODE "CFB"

/* Default cipher mode for secret key encryption */
#define DEFAULT_PGP_CIPHER_MODE PGP_CIPHER_MODE_CFB

/* Default public key algorithm for new key generation */
#define DEFAULT_PK_ALG PGP_PKA_RSA

/* Default RSA key length */
#define DEFAULT_RSA_NUMBITS 3072

/* Default ElGamal key length */
#define DEFAULT_ELGAMAL_NUMBITS 2048
#define ELGAMAL_MIN_P_BITLEN 1024
#define ELGAMAL_MAX_P_BITLEN 4096

/* Default, min and max DSA key length */
#define DSA_MIN_P_BITLEN 1024
#define DSA_MAX_P_BITLEN 3072
#define DSA_DEFAULT_P_BITLEN 2048

/* Default EC curve */
#define DEFAULT_CURVE "NIST P-256"

/* Default maximum password request attempts */
#define MAX_PASSWORD_ATTEMPTS 3

/* Infinite password request attempts */
#define INFINITE_ATTEMPTS -1

/* Default key expiration in seconds, 2 years */
#define DEFAULT_KEY_EXPIRATION (2 * 365 * 24 * 60 * 60)

#endif
