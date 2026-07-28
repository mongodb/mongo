// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/util/modules.h"

#include <cstdint>

namespace mongo::ce {

/**
 * Hashes a non-array BSON value for NDV estimation, mimicking woCompare() with no collator:
 * values hash equal iff they compare equal (modulo 64-bit collisions). Field names are ignored.
 * EOO (a missing field) hashes like Undefined, which it compares equal to, and stays distinct
 * from null. Arrays fail a tassert; NDV rejects them upstream.
 *
 * Fixed-seed and non-cryptographic: estimation only, no resistance to crafted collisions.
 */
uint64_t hashValueForNdv(const BSONElement& value);

}  // namespace mongo::ce
