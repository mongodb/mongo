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

#include "mongo/unittest/death_test.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if __has_feature(thread_sanitizer)
#include <sanitizer/common_interface_defs.h>
#endif

#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#if __has_feature(thread_sanitizer)
#define TSAN_ENABLED_
#endif
#if __has_feature(address_sanitizer)
#define ASAN_ENABLED_
#endif
#if __has_feature(memory_sanitizer)
#define MSAN_ENABLED_
#endif

namespace mongo::unittest {

void DeathTestBase::TestBody() {
#if defined(ASAN_ENABLED_) || defined(MSAN_ENABLED_)
    LOGV2(5306900, "Skipping death test in sanitizer build");
#elif defined(_WIN32)
    LOGV2(24133, "Skipping death test on Windows");
#elif defined(__APPLE__) && (TARGET_OS_TV || TARGET_OS_WATCH)
    LOGV2(24134, "Skipping death test on tvOS/watchOS");
#else
    SCOPED_TRACE(fmt::format("Death test @ {}:{}", _getFile(), _getLine()));
    _executeDeathTest();
#endif
}

}  // namespace mongo::unittest
