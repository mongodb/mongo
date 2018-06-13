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
import com.sun.jna.Memory;
import com.sun.jna.NativeLong;
import com.sun.jna.Pointer;
import com.sun.jna.ptr.NativeLongByReference;
import com.sun.jna.ptr.PointerByReference;

import java.nio.ByteBuffer;

class MongoEmbeddedClientImpl implements MongoEmbeddedClient {
    private final CAPI.mongo_embedded_v1_status status;
    private final CAPI.mongo_embedded_v1_client client;

    MongoEmbeddedClientImpl(final CAPI.mongo_embedded_v1_instance instance) {
        status = CAPIHelper.createStatusPointer();

        try {
            client = CAPI.mongo_embedded_v1_client_create(instance, status);
        } catch (Throwable t) {
            throw CAPIHelper.createError("instance_create", t);
        }

        if (client == null) {
            CAPIHelper.createErrorFromStatus(status);
        }
    }


    @Override
    public void close() {
        try {
            CAPIHelper.validateErrorCode(status,
                    CAPI.mongo_embedded_v1_client_destroy(client, status));
        } catch (Throwable t) {
            throw CAPIHelper.createError("instance_destroy", t);
        }
    }

    @Override
    public ByteBuffer write(final ByteBuffer buffer) {
        PointerByReference outputBufferReference = new PointerByReference();
        NativeLongByReference outputSize = new NativeLongByReference();

        byte[] message = new byte[buffer.remaining()];
        buffer.get(message, 0, buffer.remaining());
        Pointer messagePointer = new Memory(message.length);
        messagePointer.write(0, message, 0, message.length);

        try {
            CAPIHelper.validateErrorCode(status,
                    CAPI.mongo_embedded_v1_client_invoke(client, messagePointer, new NativeLong(message.length), outputBufferReference,
                            outputSize, status));
        } catch (Throwable t) {
            throw CAPIHelper.createError("client_invoke", t);
        }
        return outputBufferReference.getValue().getByteBuffer(0, outputSize.getValue().longValue());
    }
}
