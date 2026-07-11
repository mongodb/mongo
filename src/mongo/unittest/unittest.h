// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/*
 * A C++ unit testing framework.
 *
 * For examples of basic usage, see mongo/unittest/unittest_test.cpp.
 *
 * It is inspired by Googletest, and shares some macro names with it, but be
 * advised that they are not the same thing, and macros like ASSERT have a
 * different meaning in this framework.
 *
 * For simplicity and for ease of maintenance, it is recommended that most tests
 * include this umbrella header rather than the specific parts of this
 * framework.
 */

#pragma once

#include "mongo/unittest/assert.h"       // IWYU pragma: export
#include "mongo/unittest/framework.h"    // IWYU pragma: export
#include "mongo/unittest/log_capture.h"  // IWYU pragma: export
#include "mongo/unittest/matcher.h"      // IWYU pragma: export
