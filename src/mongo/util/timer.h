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

#pragma once

#include "mongo/util/time_support.h"

namespace mongo {

class TickSource;

/**
 * Time tracking object.
 */
class Timer {
public:
    /**
     * Creates a timer with the system default tick source. Should not be created before global
     * initialization completes.
     */
    Timer();

    /**
     * Creates a timer using the specified tick source. Caller retains ownership of TickSource and
     * must keep it in scope until Timer goes out of scope.
     */
    explicit Timer(TickSource* tickSource);

    long long micros() const {
        return static_cast<long long>((now() - _old) * _microsPerCount);
    }
    int millis() const {
        return static_cast<int>(micros() / 1000);
    }
    int seconds() const {
        return static_cast<int>(micros() / 1000000);
    }
    int minutes() const {
        return seconds() / 60;
    }

    Microseconds elapsed() const {
        return Microseconds{micros()};
    }

    void reset() {
        _old = now();
    }

private:
    TickSource* const _tickSource;

    // Derived value from _countsPerSecond. This represents the conversion ratio
    // from clock ticks to microseconds.
    const double _microsPerCount;

    long long now() const;

    long long _old;
};

}  // namespace mongo
