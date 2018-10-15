
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
import com.sun.jna.NativeLibrary;

import static java.lang.String.format;

/**
 * The embedded mongodb CAPI.
 */
public final class MongoEmbeddedCAPI {
    private static final String NATIVE_LIBRARY_NAME = "mongo_embedded";

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
                    ), t
            );
        }
        return new MongoEmbeddedLibraryImpl(yamlConfig != null ? yamlConfig : "", logLevel != null ? logLevel : LogLevel.LOGGER);
    }

    private MongoEmbeddedCAPI() {
    }
}
