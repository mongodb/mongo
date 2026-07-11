// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/init.h"  // IWYU pragma: keep

namespace mongo {
namespace {

/*
 * A noop initializer that allows defining dependencies for the code-segment pinning initializer,
 * which only runs on Linux. An example for such dependency is loading Shared Object libraries (i.e.
 * SO files), which must happen before we start pinning code segments.
 */
MONGO_INITIALIZER(PinCodeSegments)(InitializerContext*) {}

}  // namespace
}  // namespace mongo
