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


import static java.lang.String.format;

/**
 * Top level Exception for all Mongo Embedded CAPI exceptions
 */
public class MongoEmbeddedCAPIException extends RuntimeException {
    private static final long serialVersionUID = -5524416583514807953L;
    private final int code;

    /**
     * @param msg   the message
     */
    public MongoEmbeddedCAPIException(final String msg) {
        super(msg);
        this.code = -1;
    }

    /**
     * @param msg   the message
     * @param cause the cause
     */
    public MongoEmbeddedCAPIException(final String msg, final Throwable cause) {
        super(msg, cause);
        this.code = -1;
    }

    /**
     * @param code the error code
     * @param msg  the message
     */
    public MongoEmbeddedCAPIException(final int code, final String msg) {
        super(msg);
        this.code = code;
    }

    /**
     * Constructs a new instance
     *
     * @param errorCode the error code
     * @param subErrorCode the sub category error code
     * @param reason the reason for the exception
     */
    public MongoEmbeddedCAPIException(final int errorCode, final int subErrorCode, final String reason) {
        this(errorCode, format("%s (%s:%s)", reason, errorCode, subErrorCode));
    }

    /**
     * @return the error code for the exception.
     */
    public int getCode() {
        return code;
    }
}
