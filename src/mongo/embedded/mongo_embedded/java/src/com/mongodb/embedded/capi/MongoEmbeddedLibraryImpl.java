
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
import com.mongodb.embedded.capi.internal.logging.Logger;
import com.mongodb.embedded.capi.internal.logging.Loggers;
import com.sun.jna.Pointer;

import static java.lang.String.format;

import java.util.Locale;

class MongoEmbeddedLibraryImpl implements MongoEmbeddedLibrary {
    private static final Logger LOGGER = Loggers.getLogger();
    private static final LogCallback LOG_CALLBACK = new LogCallback();

    private final CAPI.mongo_embedded_v1_status status;
    private final CAPI.mongo_embedded_v1_lib lib;

    MongoEmbeddedLibraryImpl(final String yamlConfig, final LogLevel logLevel) {
        status = CAPIHelper.createStatusPointer();
        CAPI.mongo_embedded_v1_init_params.ByReference initParams = new CAPI.mongo_embedded_v1_init_params.ByReference();
        initParams.yaml_config = new CAPI.cstring(yamlConfig != null ? yamlConfig : "");
        initParams.log_flags = logLevel != null ? logLevel.getLevel() : LogLevel.LOGGER.getLevel();
        if (logLevel == LogLevel.LOGGER) {
            initParams.log_callback = LOG_CALLBACK;
        }

        lib =  CAPI.mongo_embedded_v1_lib_init(initParams, status);
        if (lib == null) {
            CAPIHelper.createErrorFromStatus(status);
        }
    }

    @Override
    public MongoEmbeddedInstance createInstance(final String yamlConfig) {
        return new MongoEmbeddedInstanceImpl(lib, yamlConfig);
    }

    @Override
    public void close() {
        try {
            CAPIHelper.validateErrorCode(status, CAPI.mongo_embedded_v1_lib_fini(lib, status));
        } catch (Throwable t) {
            throw CAPIHelper.createError("fini", t);
        }
        CAPIHelper.destroyStatusPointer(status);
    }

    static class LogCallback implements CAPI.mongo_embedded_v1_log_callback {

        // CHECKSTYLE:OFF
        @Override
        public void log(final Pointer user_data, final CAPI.cstring message, final CAPI.cstring component, final CAPI.cstring context,
                        final int severity) {
            String logMessage = format("%-9s [%s] %s", component.toString().toUpperCase(Locale.US), context, message).trim();

            if (severity < -2) {
                LOGGER.error(logMessage);   // Severe/Fatal & Error messages
            } else if (severity == -2) {
                LOGGER.warn(logMessage);    // Warning messages
            } else if (severity < 1) {
                LOGGER.info(logMessage);    // Info / Log messages
            } else {
                LOGGER.debug(logMessage);   // Debug messages
            }
        }
        // CHECKSTYLE:ON
    }
}
