/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2012-2022, Magnus Edenhill
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * Hand-crafted config header file for Win32 builds.
 */
#ifndef _RD_WIN32_CONFIG_H_
#define _RD_WIN32_CONFIG_H_

#ifndef WITHOUT_WIN32_CONFIG
#define WITH_SSL              1
#define WITH_ZLIB             1
#define WITH_SNAPPY           1
#define WITH_ZSTD             1
#define WITH_CURL             1
#define WITH_OAUTHBEARER_OIDC 1
/* zstd is linked dynamically on Windows, but the dynamic library provides
 * the experimental/advanced API, just as the static builds on *nix */
#define WITH_ZSTD_STATIC      1
#define WITH_SASL_SCRAM       1
#define WITH_SASL_OAUTHBEARER 1
#define ENABLE_DEVEL          0
#define WITH_PLUGINS          1
#define WITH_HDRHISTOGRAM     1
#endif
#define SOLIB_EXT ".dll"

/* Notice: Keep up to date */
#define BUILT_WITH                                                             \
        "SSL ZLIB SNAPPY ZSTD CURL SASL_SCRAM SASL_OAUTHBEARER PLUGINS "       \
        "HDRHISTOGRAM"

#endif /* _RD_WIN32_CONFIG_H_ */
