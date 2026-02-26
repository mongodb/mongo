/*-
 * Copyright (c) 2017-2021 Ribose Inc.
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

#ifndef RNP_LOGGING_H_
#define RNP_LOGGING_H_

#include <stdlib.h>
#include <stdint.h>

/* environment variable name */
static const char RNP_LOG_CONSOLE[] = "RNP_LOG_CONSOLE";

bool rnp_log_switch();
void set_rnp_log_switch(int8_t);
void rnp_log_stop();
void rnp_log_continue();

namespace rnp {
class LogStop {
    bool stop_;

  public:
    LogStop(bool stop = true) : stop_(stop)
    {
        if (stop_) {
            rnp_log_stop();
        }
    }
    ~LogStop()
    {
        if (stop_) {
            rnp_log_continue();
        }
    }
};
} // namespace rnp

/* remove "src" */
#ifndef SOURCE_PATH_SIZE
#define SOURCE_PATH_SIZE 0
#endif
#define __SOURCE_PATH_FILE__ (&(__FILE__[SOURCE_PATH_SIZE + 3]))

#define RNP_LOG_FD(fd, ...)                                                              \
    do {                                                                                 \
        if (!rnp_log_switch())                                                           \
            break;                                                                       \
        (void) fprintf((fd), "[%s() %s:%d] ", __func__, __SOURCE_PATH_FILE__, __LINE__); \
        (void) fprintf((fd), __VA_ARGS__);                                               \
        (void) fprintf((fd), "\n");                                                      \
    } while (0)

#define RNP_LOG(...) RNP_LOG_FD(stderr, __VA_ARGS__)

#define RNP_LOG_KEY(msg, key)                                                           \
    do {                                                                                \
        if (!(key)) {                                                                   \
            RNP_LOG(msg, "(null)");                                                     \
            break;                                                                      \
        }                                                                               \
        auto keyid = (key)->keyid();                                                    \
        auto idhex = bin_to_hex(keyid.data(), keyid.size(), rnp::HexFormat::Lowercase); \
        RNP_LOG(msg, idhex.c_str());                                                    \
    } while (0)

#if defined(ENABLE_PQC_DBG_LOG)

#define RNP_LOG_FD_NO_POS_INFO(fd, ...)    \
    do {                                   \
        if (!rnp_log_switch())             \
            break;                         \
        (void) fprintf((fd), "[LOG] ");    \
        (void) fprintf((fd), __VA_ARGS__); \
        (void) fprintf((fd), "\n");        \
    } while (0)

#define RNP_LOG_NO_POS_INFO(...) RNP_LOG_FD_NO_POS_INFO(stderr, __VA_ARGS__)

#define RNP_LOG_U8VEC(msg, vec)                                             \
    do {                                                                    \
        if (vec.empty() {                                   \
            RNP_LOG(msg, "(empty)");                        \
            break;                                          \
        }                                                   \
        auto _tmp_hex_vec = rnp::hex_to_bin(vec, rnp::HexFormat::Lowercase) \
        RNP_LOG_NO_POS_INFO(msg, _tmp_hex_vec.c_str());                     \
    } while (0)
#endif

#endif
