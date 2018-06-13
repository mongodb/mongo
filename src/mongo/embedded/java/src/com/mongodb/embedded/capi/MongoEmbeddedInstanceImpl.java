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

class MongoEmbeddedInstanceImpl implements MongoEmbeddedInstance {
    private final CAPI.mongo_embedded_v1_status status;
    private final CAPI.mongo_embedded_v1_instance instance;

    MongoEmbeddedInstanceImpl(final CAPI.mongo_embedded_v1_lib libraryPointer, final String yamlConfig) {
        status = CAPIHelper.createStatusPointer();

        try {
            instance = CAPI.mongo_embedded_v1_instance_create(libraryPointer,
                    new CAPI.cstring(yamlConfig != null ? yamlConfig : ""), status);
        } catch (Throwable t) {
            throw CAPIHelper.createError("instance_create", t);
        }

        if (instance == null) {
            CAPIHelper.createErrorFromStatus(status);
        }
    }

    @Override
    public MongoEmbeddedClient createClient() {
        return new MongoEmbeddedClientImpl(instance);
    }

    @Override
    public void close() {
        try {
            CAPIHelper.validateErrorCode(status,
                    CAPI.mongo_embedded_v1_instance_destroy(instance, status));
        } catch (Throwable t) {
            throw CAPIHelper.createError("instance_destroy", t);
        }
    }
}
