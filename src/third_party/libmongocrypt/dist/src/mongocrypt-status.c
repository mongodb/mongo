/*
 * Copyright 2018-present MongoDB, Inc.
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

#include <bson/bson.h>

#include "mongocrypt-status-private.h"

struct _mongocrypt_status_t {
    mongocrypt_status_type_t type;
    uint32_t code;
    char *message;
    uint32_t len;
};

mongocrypt_status_t *mongocrypt_status_new(void) {
    return bson_malloc0(sizeof(mongocrypt_status_t));
}

void mongocrypt_status_set(mongocrypt_status_t *status,
                           mongocrypt_status_type_t type,
                           uint32_t code,
                           const char *message,
                           int32_t message_len) {
    if (!status) {
        return;
    }

    if (message_len < 0) {
        message_len = (int32_t)strlen(message) + 1;
    } else if (message_len == 0) {
        /* This is really an error, since message_len should be one more than the
         * string length. But interpret as the empty string */
        message_len = 1;
    }

    bson_free(status->message);
    status->message = bson_malloc((size_t)message_len);
    BSON_ASSERT(status->message);
    status->message[message_len - 1] = '\0';
    memcpy(status->message, message, (size_t)message_len - 1);
    status->len = (uint32_t)message_len - 1;
    status->type = type;
    status->code = code;
}

const char *mongocrypt_status_message(mongocrypt_status_t *status, uint32_t *len) {
    BSON_ASSERT_PARAM(status);

    if (mongocrypt_status_ok(status)) {
        return NULL;
    }
    if (len) {
        *len = status->len;
    }
    return status->message;
}

uint32_t mongocrypt_status_code(mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(status);

    return status->code;
}

mongocrypt_status_type_t mongocrypt_status_type(mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(status);

    return status->type;
}

bool mongocrypt_status_ok(mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(status);

    return (status->type == MONGOCRYPT_STATUS_OK);
}

void _mongocrypt_status_copy_to(mongocrypt_status_t *src, mongocrypt_status_t *dst) {
    BSON_ASSERT_PARAM(dst);
    BSON_ASSERT_PARAM(src);

    if (dst == src) {
        return;
    }

    dst->type = src->type;
    dst->code = src->code;
    dst->len = src->len;
    if (dst->message) {
        bson_free(dst->message);
        dst->message = NULL;
    }

    if (src->message) {
        dst->message = bson_strdup(src->message);
    }
}

void _mongocrypt_status_reset(mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(status);

    status->type = MONGOCRYPT_STATUS_OK;
    status->code = 0;
    status->len = 0;
    bson_free(status->message);
    status->message = NULL;
}

void mongocrypt_status_destroy(mongocrypt_status_t *status) {
    if (!status) {
        return;
    }

    bson_free(status->message);
    bson_free(status);
}

void _mongocrypt_status_append(mongocrypt_status_t *status, mongocrypt_status_t *to_append) {
    char *orig;

    BSON_ASSERT_PARAM(status);
    BSON_ASSERT_PARAM(to_append);

    orig = status->message;

    if (mongocrypt_status_ok(to_append)) {
        return;
    }
    status->message = bson_strdup_printf("%s: %s", orig, to_append->message);
    bson_free(orig);
}
