// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/compiler.h"

// Ignore data races in certain functions when running with TSAN.
#if __has_feature(thread_sanitizer)
#define MONGO_TSAN_IGNORE __attribute__((no_sanitize("thread")))
#else
#define MONGO_TSAN_IGNORE
#endif
