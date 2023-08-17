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

#include "mongocrypt-config.h"
#include "mongocrypt-log-private.h"
#include "mongocrypt-opts-private.h"

#include <bson/bson.h>

void _mongocrypt_log_init(_mongocrypt_log_t *log) {
    BSON_ASSERT_PARAM(log);

    _mongocrypt_mutex_init(&log->mutex);
    /* Initially, no log function is set. */
    _mongocrypt_log_set_fn(log, NULL, NULL);
#ifdef MONGOCRYPT_ENABLE_TRACE
    log->trace_enabled = (getenv("MONGOCRYPT_TRACE") != NULL);
#endif
}

void _mongocrypt_log_cleanup(_mongocrypt_log_t *log) {
    if (!log) {
        return;
    }

    _mongocrypt_mutex_cleanup(&log->mutex);
    memset(log, 0, sizeof(*log));
}

void _mongocrypt_stdout_log_fn(mongocrypt_log_level_t level, const char *message, uint32_t message_len, void *ctx) {
    BSON_ASSERT_PARAM(message);

    switch (level) {
    case MONGOCRYPT_LOG_LEVEL_FATAL: printf("FATAL"); break;
    case MONGOCRYPT_LOG_LEVEL_ERROR: printf("ERROR"); break;
    case MONGOCRYPT_LOG_LEVEL_WARNING: printf("WARNING"); break;
    case MONGOCRYPT_LOG_LEVEL_INFO: printf("INFO"); break;
    case MONGOCRYPT_LOG_LEVEL_TRACE: printf("TRACE"); break;
    default: printf("UNKNOWN"); break;
    }
    printf(" %s\n", message);
}

void _mongocrypt_log_set_fn(_mongocrypt_log_t *log, mongocrypt_log_fn_t fn, void *ctx) {
    BSON_ASSERT_PARAM(log);

    _mongocrypt_mutex_lock(&log->mutex);
    log->fn = fn;
    log->ctx = ctx;
    _mongocrypt_mutex_unlock(&log->mutex);
}

void _mongocrypt_log(_mongocrypt_log_t *log, mongocrypt_log_level_t level, const char *format, ...) {
    va_list args;
    char *message;

    BSON_ASSERT_PARAM(log);
    BSON_ASSERT_PARAM(format);

    if (level == MONGOCRYPT_LOG_LEVEL_TRACE && !log->trace_enabled) {
        return;
    }

    va_start(args, format);
    message = bson_strdupv_printf(format, args);
    va_end(args);

    BSON_ASSERT(message);

    _mongocrypt_mutex_lock(&log->mutex);
    if (log->fn) {
        log->fn(level, message, (uint32_t)strlen(message), log->ctx);
    }
    _mongocrypt_mutex_unlock(&log->mutex);
    bson_free(message);
}
