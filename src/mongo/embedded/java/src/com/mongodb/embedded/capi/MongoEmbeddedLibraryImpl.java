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
