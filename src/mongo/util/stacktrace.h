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

/**
 * Tools for working with in-process stack traces.
 */

#pragma once

#include <iosfwd>

#if defined(_WIN32)
#include "mongo/platform/windows_basic.h"  // for CONTEXT
#endif

#include "mongo/base/string_data.h"

namespace mongo {

/** Abstract sink onto which stacktrace is piecewise emitted. */
class StackTraceSink {
public:
    StackTraceSink& operator<<(StringData v) {
        doWrite(v);
        return *this;
    }

    StackTraceSink& operator<<(uint64_t v) {
        doWrite(v);
        return *this;
    }

private:
    virtual void doWrite(StringData v) = 0;
    virtual void doWrite(uint64_t v) = 0;
};

// Print stack trace information to "os", default to the log stream.
void printStackTrace(std::ostream& os);
void printStackTrace();

// Signal-safe variant.
void printStackTraceFromSignal(std::ostream& os);

#if defined(_WIN32)
// Print stack trace (using a specified stack context) to "os", default to the log stream.
void printWindowsStackTrace(CONTEXT& context, std::ostream& os);
void printWindowsStackTrace(CONTEXT& context);
#endif

}  // namespace mongo
