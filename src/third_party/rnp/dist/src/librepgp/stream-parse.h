/*
 * Copyright (c) 2017, [Ribose Inc](https://www.ribose.com).
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

#ifndef STREAM_PARSE_H_
#define STREAM_PARSE_H_

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include "rnp.h"
#include "stream-common.h"
#include "stream-ctx.h"
#include "stream-packet.h"
#include "signature.hpp"

typedef struct pgp_parse_handler_t pgp_parse_handler_t;
typedef bool                       pgp_destination_func_t(pgp_parse_handler_t *    handler,
                                                          pgp_dest_t **            dst,
                                                          bool *                   closedst,
                                                          const pgp_literal_hdr_t *lithdr);
typedef bool pgp_source_func_t(pgp_parse_handler_t *handler, pgp_source_t *src);
typedef void pgp_signatures_func_t(const std::vector<rnp::SignatureInfo> &sigs, void *param);

typedef void pgp_on_recipients_func_t(const std::vector<pgp_pk_sesskey_t> &recipients,
                                      const std::vector<pgp_sk_sesskey_t> &passwords,
                                      void *                               param);
typedef void pgp_decryption_start_func_t(pgp_pk_sesskey_t *pubenc,
                                         pgp_sk_sesskey_t *symenc,
                                         void *            param);
typedef void pgp_decryption_info_func_t(bool           mdc,
                                        pgp_aead_alg_t aead,
                                        pgp_symm_alg_t salg,
                                        void *         param);
typedef void pgp_decryption_done_func_t(bool validated, void *param);

/* handler used to return needed information during pgp source processing */
typedef struct pgp_parse_handler_t {
    pgp_password_provider_t *password_provider; /* if NULL then default will be used */
    rnp::KeyProvider *       key_provider; /* must be set when key is required, i.e. during
                                                signing/verification/public key encryption and
                                                decryption */
    pgp_destination_func_t *dest_provider; /* called when destination stream is required */
    pgp_source_func_t *     src_provider;  /* required to provider source during the detached
                                              signature verification */
    pgp_on_recipients_func_t *   on_recipients;       /* called before decryption start */
    pgp_decryption_start_func_t *on_decryption_start; /* called when decryption key obtained */
    pgp_decryption_info_func_t * on_decryption_info;  /* called when decryption is started */
    pgp_decryption_done_func_t * on_decryption_done;  /* called when decryption is finished */
    pgp_signatures_func_t *      on_signatures;       /* for signature verification results */

    rnp_ctx_t *ctx;   /* operation context */
    void *     param; /* additional parameters */
} pgp_parse_handler_t;

/* @brief Process the OpenPGP source: file, memory, stdin
 * Function will parse input data, provided by any source conforming to pgp_source_t,
 * autodetecting whether it is armored, cleartext or binary.
 * @param handler handler to respond on stream reader callbacks
 * @param src initialized source with cache
 * @return RNP_SUCCESS on success or error code otherwise
 **/
rnp_result_t process_pgp_source(pgp_parse_handler_t *handler, pgp_source_t &src);

/* @brief Init source with OpenPGP compressed data packet
 * @param src allocated pgp_source_t structure
 * @param readsrc source to read compressed data from
 * @return RNP_SUCCESS on success or error code otherwise
 */
rnp_result_t init_compressed_src(pgp_source_t *src, pgp_source_t *readsrc);

/* @brief Get compression algorithm used in compressed source
 * @param src compressed source, initialized with init_compressed_src
 * @param alg algorithm will be written here. Cannot be NULL.
 * @return true if operation succeeded and alg is populate or false otherwise
 */
bool get_compressed_src_alg(pgp_source_t *src, uint8_t *alg);

/* @brief Init source with OpenPGP literal data packet
 * @param src allocated pgp_source_t structure
 * @param readsrc source to read literal data from
 * @return RNP_SUCCESS on success or error code otherwise
 */
rnp_result_t init_literal_src(pgp_source_t *src, pgp_source_t *readsrc);

/* @brief Get the literal data packet information fields (not the OpenPGP packet header)
 * @param src literal data source, initialized with init_literal_src
 * @return reference to the structure
 */
const pgp_literal_hdr_t &get_literal_src_hdr(pgp_source_t &src);

#if defined(ENABLE_CRYPTO_REFRESH)
/* @brief Get the SEIPDv2 packet information fields (not the OpenPGP packet header)
 * @param src SEIPDv2-encrypted data source (starting from packet data itself, not the header)
 * @param hdr pointer to header structure, where result will be stored
 * @return true on success or false otherwise
 */
bool get_seipdv2_src_hdr(pgp_source_t *src, pgp_seipdv2_hdr_t *hdr);
#endif

/* @brief Get the AEAD-encrypted packet information fields (not the OpenPGP packet header)
 * @param src AEAD-encrypted data source (starting from packet data itself, not the header)
 * @param hdr pointer to header structure, where result will be stored
 * @return true on success or false otherwise
 */
bool get_aead_src_hdr(pgp_source_t *src, pgp_aead_hdr_t *hdr);

#endif
