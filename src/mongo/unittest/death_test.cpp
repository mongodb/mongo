// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
