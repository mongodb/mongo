// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <span>

namespace mongo::ce {

/**
 * Hashes a BSON-representable SBE value for NDV estimation using uncollated KeyString encoding.
 * Values hash equal iff their encodings match (modulo 64-bit collisions). Top-level field names
 * and BSON array element field names are ignored.
 * Deprecated Undefined and Nothing (a missing field) have identical KeyString encodings and
 * stay distinct from null.
 *
 * Fixed-seed and non-cryptographic: estimation only, no resistance to crafted collisions.
 */
uint64_t hashValueForNdv(sbe::value::TagValueView value);

/**
 * Hashes a tuple of SBE values for composite NDV estimation by concatenating their
 * KeyString encodings, in the order given, and hashing the result once. Tuples hash equal iff
 * their component encodings match (modulo 64-bit collisions); KeyString encodings are
 * self-delimiting, so values cannot bleed across tuple positions. Per-element rules match
 * hashValueForNdv(), and a one-element tuple hashes identically to it.
 */
uint64_t hashValuesForNdv(std::span<const sbe::value::TagValueView> values);

}  // namespace mongo::ce
