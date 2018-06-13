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

/**
 * LogLevel represents the supported logging levels for the embedded mongod
 */
public enum LogLevel {
    /**
     * Turn off logging
     */
    NONE(0),

    /**
     * Log to stdout
     */
    STDOUT(1),

    /**
     * Log to stderr
     */
    STDERR(2),

    /**
     * Log via the {@code org.mongodb.embedded.capi} logger
     */
    LOGGER(4);

    private final int level;

    /**
     * @return the logging level
     */
    public int getLevel(){
        return level;
    }

    LogLevel(final int level){
        this.level = level;
    }
}
