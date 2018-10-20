
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

package com.mongodb.embedded.capi.internal.logging;

import java.util.logging.Level;

import static java.util.logging.Level.FINE;
import static java.util.logging.Level.FINER;
import static java.util.logging.Level.INFO;
import static java.util.logging.Level.SEVERE;
import static java.util.logging.Level.WARNING;

class JULLogger implements Logger {

    private final java.util.logging.Logger delegate;

    JULLogger(final String name) {
        this.delegate = java.util.logging.Logger.getLogger(name);
    }

    @Override
    public String getName() {
        return delegate.getName();
    }

    @Override
    public boolean isTraceEnabled() {
        return isEnabled(FINER);
    }

    @Override
    public void trace(final String msg) {
        log(FINER, msg);
    }

    @Override
    public void trace(final String msg, final Throwable t) {
        log(FINER, msg, t);
    }

    @Override
    public boolean isDebugEnabled() {
        return isEnabled(FINE);
    }

    @Override
    public void debug(final String msg) {
        log(FINE, msg);
    }

    @Override
    public void debug(final String msg, final Throwable t) {
        log(FINE, msg, t);
    }

    @Override
    public boolean isInfoEnabled() {
        return delegate.isLoggable(INFO);
    }

    @Override
    public void info(final String msg) {
        log(INFO, msg);
    }

    @Override
    public void info(final String msg, final Throwable t) {
        log(INFO, msg, t);
    }

    @Override
    public boolean isWarnEnabled() {
        return delegate.isLoggable(WARNING);
    }

    @Override
    public void warn(final String msg) {
        log(WARNING, msg);
    }

    @Override
    public void warn(final String msg, final Throwable t) {
        log(WARNING, msg, t);
    }


    @Override
    public boolean isErrorEnabled() {
        return delegate.isLoggable(SEVERE);
    }

    @Override
    public void error(final String msg) {
        log(SEVERE, msg);
    }

    @Override
    public void error(final String msg, final Throwable t) {
        log(SEVERE, msg, t);
    }


    private boolean isEnabled(final Level level) {
        return delegate.isLoggable(level);
    }

    private void log(final Level level, final String msg) {
        delegate.log(level, msg);
    }

    public void log(final Level level, final String msg, final Throwable t) {
        delegate.log(level, msg, t);
    }
}
