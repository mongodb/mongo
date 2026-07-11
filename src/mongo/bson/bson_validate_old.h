// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsontypes.h"

#include <cstdint>

namespace mongo {
class BSONObj;
class Status;

/**
 * Older version of validateBSON to be used as "oracle" for the fuzzer to check the
 * current version against.
 */
namespace fuzzerOnly {
Status validateBSON(const char* buf, uint64_t maxLength);
}  // namespace fuzzerOnly
}  // namespace mongo
