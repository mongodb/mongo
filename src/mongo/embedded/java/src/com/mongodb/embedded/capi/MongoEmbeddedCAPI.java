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
import com.sun.jna.NativeLibrary;

import static java.lang.String.format;

/**
 * The embedded mongodb CAPI.
 */
public final class MongoEmbeddedCAPI {
    private static final String NATIVE_LIBRARY_NAME = "mongo_embedded_capi";

    /**
     * Initializes the embedded mongodb library, required before any other call.
     *
     * <p>Cannot be called multiple times without first calling {@link MongoEmbeddedLibrary#close()}.</p>
     *
     * @param yamlConfig the yaml configuration for the embedded mongodb capi library
     * @return the initialized MongoEmbedded.
     */
    public static MongoEmbeddedLibrary create(final String yamlConfig) {
        return create(yamlConfig, LogLevel.LOGGER);
    }

    /**
     * Initializes the embedded mongodb library, required before any other call.
     *
     * <p>Cannot be called multiple times without first calling {@link MongoEmbeddedLibrary#close()}.</p>
     *
     * @param yamlConfig the yaml configuration for the embedded mongodb capi library
     * @param logLevel   the logging level
     * @return the initialized MongoEmbedded.
     */
    public static MongoEmbeddedLibrary create(final String yamlConfig, final LogLevel logLevel) {
        return create(yamlConfig, logLevel, null);
    }

    /**
     * Initializes the embedded mongodb library, required before any other call.
     *
     * <p>Cannot be called multiple times without first calling {@link MongoEmbeddedLibrary#close()}.</p>
     *
     * @param yamlConfig the yaml configuration for the embedded mongodb capi library
     * @param libraryPath the path to the embedded mongodb capi library.
     * @param logLevel   the logging level
     * @return the initialized MongoEmbedded.
     */
    public static  MongoEmbeddedLibrary create(final String yamlConfig, final LogLevel logLevel, final String libraryPath) {
        if (libraryPath != null) {
            NativeLibrary.addSearchPath(NATIVE_LIBRARY_NAME, libraryPath);
        }
        try {
            new CAPI();
        } catch (Throwable t) {
            throw new MongoEmbeddedCAPIException(
                    format("Unable to load the Mongo Embedded Library.%n"
                         + "Please either: Set the libraryPath when calling MongoEmbeddedCAPI.create or %n"
                         + "Ensure the library is set on the jna.library.path or the java.library.path system property."
                    )
            );
        }
        return new MongoEmbeddedLibraryImpl(yamlConfig != null ? yamlConfig : "", logLevel != null ? logLevel : LogLevel.LOGGER);
    }

    private MongoEmbeddedCAPI() {
    }
}
