/*
 * Copyright (c) 2020, [Ribose Inc](https://www.ribose.com).
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

#include <rnp/rnp.h>
#include "../lib/key.hpp"
#include "../librekey/key_store_g10.h"
#include "../librepgp/stream-common.h"
#include "../include/rekey/rnp_key_store.h"
#include "../lib/sec_profile.hpp"

#ifdef RNP_RUN_TESTS
int keyring_g10_LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int
keyring_g10_LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
#else
extern "C" RNP_API int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
#endif
{
    rnp::SecurityContext ctx;
    rnp::KeyStore        ks(ctx);
    pgp_source_t         memsrc = {};

    init_mem_src(&memsrc, data, size, false);
    ks.load_g10(memsrc);
    memsrc.close();

    return 0;
}
