// @file mongo/util/timer-generic-inl.h

/*    Copyright 2010 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/**
 * Inline function implementations for the "generic" implementation of the
 * Timer class.  This implementation often has pretty poor resolution, but is available
 * on all supported platforms.
 *
 * This file should only be included through timer-inl.h, which selects the
 * particular implementation based on target platform.
 */

#pragma once

#define MONGO_TIMER_IMPL_GENERIC

#include "mongo/util/time_support.h"

namespace mongo {

    unsigned long long Timer::now() const { return curTimeMicros64(); }

}  // namespace mongo
