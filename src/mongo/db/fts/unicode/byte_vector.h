// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#define MONGO_HAVE_FAST_BYTE_VECTOR

// TODO replace this with #if BOOST_HW_SIMD_X86 >= BOOST_HW_SIMD_X86_SSE2_VERSION in boost 1.60
#if defined(_M_AMD64) || defined(__amd64__)
#include "mongo/db/fts/unicode/byte_vector_sse2.h"  // IWYU pragma: export
#elif defined(__powerpc64__)
#include "mongo/db/fts/unicode/byte_vector_altivec.h"  // IWYU pragma: export
#elif defined(__aarch64__)
#include "mongo/db/fts/unicode/byte_vector_neon.h"  // IWYU pragma: export
#else                                               // Other platforms go above here.
#undef MONGO_HAVE_FAST_BYTE_VECTOR
#endif
