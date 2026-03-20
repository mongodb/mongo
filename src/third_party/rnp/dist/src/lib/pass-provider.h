/*
 * Copyright (c) 2017 - 2019, [Ribose Inc](https://www.ribose.com).
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
#ifndef RNP_PASS_PROVIDER_H
#define RNP_PASS_PROVIDER_H

#include <cstddef>
#include <cstdint>

namespace rnp {
class Key;
}

typedef struct pgp_password_ctx_t {
    uint8_t         op;
    const rnp::Key *key;

    pgp_password_ctx_t(uint8_t anop, const rnp::Key *akey = NULL) : op(anop), key(akey){};
} pgp_password_ctx_t;

typedef bool pgp_password_callback_t(const pgp_password_ctx_t *ctx,
                                     char *                    password,
                                     size_t                    password_size,
                                     void *                    userdata);

typedef struct pgp_password_provider_t {
    pgp_password_callback_t *callback;
    void *                   userdata;
    pgp_password_provider_t(pgp_password_callback_t *cb = NULL, void *ud = NULL)
        : callback(cb), userdata(ud){};
} pgp_password_provider_t;

bool pgp_request_password(const pgp_password_provider_t *provider,
                          const pgp_password_ctx_t *     ctx,
                          char *                         password,
                          size_t                         password_size);
bool rnp_password_provider_string(const pgp_password_ctx_t *ctx,
                                  char *                    password,
                                  size_t                    password_size,
                                  void *                    userdata);
#endif
