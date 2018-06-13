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

import java.nio.ByteBuffer;

/**
 * The embedded client
 */
public interface MongoEmbeddedClient {

    /**
     * Writes the input to the embedded mongodb client
     *
     * @param input the input to write to the client
     * @return the response from the embedded mongodb
     */
    ByteBuffer write(ByteBuffer input);

    /**
     * Shutsdown the client
     */
    void close();
}
