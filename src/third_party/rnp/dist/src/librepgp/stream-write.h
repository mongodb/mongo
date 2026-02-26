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

#ifndef STREAM_WRITE_H_
#define STREAM_WRITE_H_

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include "rnp.h"
#include "stream-common.h"
#include "stream-ctx.h"

/** @brief sign the input data, producing attached, detached or cleartext signature.
 *         Type of the signature is controlled by clearsign and detached fields of the
 *         rnp_ctx_t structure
 *  @param handler handler to respond on stream processor callbacks, and additional processing
 *         parameters, including rnp_ctx_t
 *  @param src input source: file, stdin, memory, whatever else conforming to pgp_source_t
 *  @param dst output destination: file, stdout, memory, whatever else conforming to pgp_dest_t
 **/
rnp_result_t rnp_sign_src(rnp_ctx_t &ctx, pgp_source_t &src, pgp_dest_t &dst);

/** @brief encrypt and sign the input data. Signatures will be encrypted together with data.
 *  @param handler handler handler to respond on stream processor callbacks, and additional
 *         processing parameters, including rnp_ctx_t
 *  @param src input source: file, stdin, memory, whatever else conforming to pgp_source_t
 *  @param dst output destination: file, stdout, memory, whatever else conforming to pgp_dest_t
 **/
rnp_result_t rnp_encrypt_sign_src(rnp_ctx_t &ctx, pgp_source_t &src, pgp_dest_t &dst);

/* Following functions are used only in tests currently. Later could be used in CLI for debug
 * commands like --wrap-literal, --encrypt-raw, --compress-raw, etc. */

rnp_result_t rnp_compress_src(pgp_source_t &         src,
                              pgp_dest_t &           dst,
                              pgp_compression_type_t zalg,
                              int                    zlevel);

rnp_result_t rnp_wrap_src(pgp_source_t &     src,
                          pgp_dest_t &       dst,
                          const std::string &filename,
                          uint32_t           modtime);

rnp_result_t rnp_raw_encrypt_src(pgp_source_t &        src,
                                 pgp_dest_t &          dst,
                                 const std::string &   password,
                                 rnp::SecurityContext &secctx);

#endif
