/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#include "merizo/platform/basic.h"

#include "merizo/util/fast_clock_source_factory.h"

#include <memory>

#include "merizo/stdx/memory.h"
#include "merizo/util/background_thread_clock_source.h"
#include "merizo/util/system_clock_source.h"

namespace merizo {

std::unique_ptr<ClockSource> FastClockSourceFactory::create(Milliseconds granularity) {
    // TODO: Create the fastest to read wall clock available on the system.
    // For now, assume there is no built-in fast wall clock so instead
    // create a background-thread-based timer.
    return stdx::make_unique<BackgroundThreadClockSource>(stdx::make_unique<SystemClockSource>(),
                                                          granularity);
}

}  // namespace merizo
