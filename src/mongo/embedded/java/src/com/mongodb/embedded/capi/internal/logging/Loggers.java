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

package com.mongodb.embedded.capi.internal.logging;

/**
 * This class is not part of the public API.
 */
public final class Loggers {
    private static final String NAME = "org.mongodb.driver.embedded.capi";

    private static final boolean USE_SLF4J = shouldUseSLF4J();

    /**
     * @return the logger
     */
    public static Logger getLogger() {
        if (USE_SLF4J) {
            return new SLF4JLogger(NAME);
        } else {
            return new JULLogger(NAME);
        }
    }

    private Loggers() {
    }

    private static boolean shouldUseSLF4J() {
        try {
            Class.forName("org.slf4j.Logger");
            return true;
        } catch (ClassNotFoundException e) {
            return false;
        }
    }
}
