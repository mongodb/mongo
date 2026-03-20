/*-
 * Copyright (c) 2018 Ribose Inc.
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
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef RNP_CRYPTO_COMMON_H_
#define RNP_CRYPTO_COMMON_H_

/* base */
#include "mpi.hpp"
#include "rng.h"
/* asymmetric crypto */
#include "rsa.h"
#include "dsa.h"
#include "elgamal.h"
#include "ec.h"
#include "ecdh.h"
#include "ecdsa.h"
#include "sm2.h"
#include "eddsa.h"
#if defined(ENABLE_PQC)
#include "kyber_ecdh_composite.h"
#include "dilithium_exdsa_composite.h"
#include "sphincsplus.h"
#endif
#if defined(ENABLE_CRYPTO_REFRESH)
#include "x25519.h"
#include "ed25519.h"
#endif
/* symmetric crypto */
#include "symmetric.h"
/* hash */
#include "hash.hpp"
/* s2k */
#include "s2k.h"
/* backend name and version */
#include "backend_version.h"

#endif // RNP_CRYPTO_COMMON_H_
