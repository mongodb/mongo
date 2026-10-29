// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <span>

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

/**
 * Hashes a tuple of non-array BSON values for composite NDV estimation by concatenating their
 * KeyString encodings, in the order given, and hashing the result once. Tuples hash equal iff
 * they are pairwise woCompare-equal (modulo 64-bit collisions); KeyString encodings are
 * self-delimiting, so values cannot bleed across tuple positions. Per-element rules match
 * hashValueForNdv(), and a one-element tuple hashes identically to it.
 */
uint64_t hashValuesForNdv(std::span<const BSONElement> values);

}  // namespace mongo::ce
