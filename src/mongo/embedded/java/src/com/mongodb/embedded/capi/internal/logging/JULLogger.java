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
