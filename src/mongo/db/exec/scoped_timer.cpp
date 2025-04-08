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

#include "mongo/db/exec/scoped_timer.h"

#include "mongo/platform/compiler.h"

#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {
ScopedTimer::ScopedTimer(Nanoseconds* counter, TickSource* ts)
    : _counter(counter), _tickSource(ts), _clockSource(nullptr), _startTS(ts->getTicks()) {}

ScopedTimer::ScopedTimer(Nanoseconds* counter, ClockSource* cs)
    : _counter(counter), _tickSource(nullptr), _clockSource(cs), _startCS(cs->now()) {}

ScopedTimer::~ScopedTimer() {
    if (MONGO_likely(_clockSource)) {
        *_counter += Nanoseconds{
            (durationCount<Milliseconds>(_clockSource->now() - _startCS) * 1000 * 1000)};
        return;
    }
    if (_tickSource) {
        *_counter += _tickSource->ticksTo<Nanoseconds>(_tickSource->getTicks() - _startTS);
    }
}

TimeElapsedBuilderScopedTimer::TimeElapsedBuilderScopedTimer(ClockSource* clockSource,
                                                             StringData description,
                                                             BSONObjBuilder* builder)
    : _clockSource(clockSource),
      _description(description),
      _beginTime(clockSource->now()),
      _builder(builder) {}

TimeElapsedBuilderScopedTimer::~TimeElapsedBuilderScopedTimer() {
    mongo::Milliseconds elapsedTime = _clockSource->now() - _beginTime;
    _builder->append(_description, elapsedTime.toString());
}

boost::optional<TimeElapsedBuilderScopedTimer> createTimeElapsedBuilderScopedTimer(
    ClockSource* clockSource, StringData description, BSONObjBuilder* builder) {
    if (builder == nullptr) {
        return boost::none;
    }
    return boost::optional<TimeElapsedBuilderScopedTimer>(
        boost::in_place_init, clockSource, description, builder);
}
}  // namespace mongo
