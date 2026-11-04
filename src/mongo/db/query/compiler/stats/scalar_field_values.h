// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/pipeline/field_path.h"

#include <span>

#include <absl/container/inlined_vector.h>

namespace mongo::stats {

/**
 * Extracts borrowed scalar SBE views in path order. The input must outlive the returned views.
 * Missing fields become Nothing; arrays at or along a requested path are rejected.
 */
absl::InlinedVector<sbe::value::TagValueView, 3> extractScalarFieldValues(
    sbe::value::TagValueView doc, std::span<const FieldPath> paths);

}  // namespace mongo::stats
