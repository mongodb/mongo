// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#if defined(_WIN32)

#include "mongo/platform/windows_basic.h"  // for CONTEXT
#include "mongo/util/stacktrace.h"

#include <iosfwd>

namespace mongo {

// Print a stack trace (using a specified stack context) to a sink.
// If sink is unspecified, it defaults to the `LogComponent::kControl` stream.
void printWindowsStackTrace(CONTEXT& context, StackTraceSink& sink);
void printWindowsStackTrace(CONTEXT& context, std::ostream& os);
void printWindowsStackTrace(CONTEXT& context);

}  // namespace mongo

#endif
