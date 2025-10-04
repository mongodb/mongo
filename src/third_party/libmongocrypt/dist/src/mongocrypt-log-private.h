/*
 * Copyright 2019-present MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef MONGOCRYPT_LOG_PRIVATE_H
#define MONGOCRYPT_LOG_PRIVATE_H

#include "mongocrypt-mutex-private.h"
#include "mongocrypt.h"

typedef struct {
    mongocrypt_mutex_t mutex; /* protects fn and ctx. */
    mongocrypt_log_fn_t fn;
    void *ctx;
    bool trace_enabled;
} _mongocrypt_log_t;

void _mongocrypt_stdout_log_fn(mongocrypt_log_level_t level, const char *message, uint32_t message_len, void *ctx);

void _mongocrypt_log(_mongocrypt_log_t *log, mongocrypt_log_level_t level, const char *message, ...)
    BSON_GNUC_PRINTF(3, 4);

void _mongocrypt_log_init(_mongocrypt_log_t *log);

void _mongocrypt_log_cleanup(_mongocrypt_log_t *log);

void _mongocrypt_log_set_fn(_mongocrypt_log_t *log, mongocrypt_log_fn_t fn, void *ctx);

#ifdef MONGOCRYPT_ENABLE_TRACE

#define CRYPT_TRACEF(log, fmt, ...)                                                                                    \
    _mongocrypt_log(log, MONGOCRYPT_LOG_LEVEL_TRACE, "(%s:%d) " fmt, BSON_FUNC, __LINE__, __VA_ARGS__)

#define CRYPT_TRACE(log, msg) CRYPT_TRACEF(crypt, "%s", msg)

#define CRYPT_ENTRY(log) _mongocrypt_log(crypt, MONGOCRYPT_LOG_LEVEL_TRACE, "entry (%s:%d)", BSON_FUNC, __LINE__)

#define CRYPT_EXIT(log)                                                                                                \
    do {                                                                                                               \
        _mongocrypt_log(crypt, MONGOCRYPT_LOG_LEVEL_TRACE, "exit (%s:%d)", BSON_FUNC, __LINE__);                       \
        return;                                                                                                        \
    } while (0)

#define CRYPT_RETURN(log, x)                                                                                           \
    do {                                                                                                               \
        _mongocrypt_log(log, MONGOCRYPT_LOG_LEVEL_TRACE, "return (%s:%d)", BSON_FUNC, __LINE__);                       \
        return (x);                                                                                                    \
    } while (0)

#define CRYPT_GOTO(log, x)                                                                                             \
    do {                                                                                                               \
        _mongocrypt_log(log, MONGOCRYPT_LOG_LEVEL_TRACE, "goto (%s:%d)", BSON_FUNC, __LINE__);                         \
        goto x;                                                                                                        \
    } while (0)

#else

#define CRYPT_TRACEF(log, fmt, ...)
#define CRYPT_TRACE(log, msg)
#define CRYPT_ENTRY(log)
#define CRYPT_EXIT(log)
#define CRYPT_RETURN(log, x) return (x);
#define CRYPT_GOTO(log, x) goto x;

#endif /* MONGOCRYPT_ENABLE_TRACE */

#endif /* MONGOCRYPT_LOG_PRIVATE_H */
