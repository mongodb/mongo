/*-
 *    Copyright (C) 2018 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
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
