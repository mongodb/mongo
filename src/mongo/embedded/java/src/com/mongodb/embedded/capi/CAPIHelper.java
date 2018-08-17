/*
 * Copyright 2008-present MongoDB, Inc.
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

package com.mongodb.embedded.capi;

import com.mongodb.embedded.capi.internal.CAPI;

import static java.lang.String.format;

final class CAPIHelper {

    static CAPI.mongo_embedded_v1_status createStatusPointer() {
        try {
            return CAPI.mongo_embedded_v1_status_create();
        } catch (Throwable t) {
            throw createError("status_create", t);
        }
    }

    static MongoEmbeddedCAPIException createError(final String methodName, final Throwable t) {
        if (t instanceof MongoEmbeddedCAPIException) {
            return (MongoEmbeddedCAPIException) t;
        }
        return new MongoEmbeddedCAPIException(format("Error from embedded server when calling '%s': %s", methodName, t.getMessage()), t);
    }

    static void createErrorFromStatus(final CAPI.mongo_embedded_v1_status statusPointer) {
        createErrorFromStatus(statusPointer, CAPI.mongo_embedded_v1_status_get_error(statusPointer));
    }

    static void createErrorFromStatus(final CAPI.mongo_embedded_v1_status statusPointer,
                                       final int errorCode) {
        throw new MongoEmbeddedCAPIException(errorCode,
                CAPI.mongo_embedded_v1_status_get_code(statusPointer),
                CAPI.mongo_embedded_v1_status_get_explanation(statusPointer).toString());
    }

    static void destroyStatusPointer(final CAPI.mongo_embedded_v1_status statusPointer) {
        try {
            CAPI.mongo_embedded_v1_status_destroy(statusPointer);
        } catch (Throwable t) {
            throw createError("status_destroy", t);
        }
    }

    static void validateErrorCode(final CAPI.mongo_embedded_v1_status statusPointer, final int errorCode) {
        if (errorCode != 0) {
            createErrorFromStatus(statusPointer, errorCode);
        }
    }

    private CAPIHelper() {
    }
}
