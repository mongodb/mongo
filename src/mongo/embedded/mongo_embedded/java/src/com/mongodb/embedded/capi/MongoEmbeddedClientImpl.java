
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
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
