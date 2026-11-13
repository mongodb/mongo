// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/ce/field_stats/field_stats_gen.h"
#include "mongo/db/query/compiler/ce/field_stats/hyperloglog.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mongo::ce {

/** Builds NDV sketches from one document's scalar field values at a time. */
class NdvSketchBuilder {
public:
    // 16 KiB of registers per sketch, with standard error approximately 0.81%.
    static constexpr std::int32_t kPrecision = 14;

    explicit NdvSketchBuilder(size_t numFields);

    /**
     * Consumes borrowed values in a consistent field order without retaining or modifying them.
     * Missing values must be represented as Nothing, distinct from null.
     */
    void accumulate(std::span<const sbe::value::TagValueView> values);

    /**
     * Returns an owned snapshot: one strict sketch for a single field; otherwise the strict
     * tuple, one missing-to-null folding variant per field, and the all-match variant.
     */
    NdvStats getStats() const;

private:
    size_t _numFields;
    std::vector<HyperLogLog> _sketches;
};

}  // namespace mongo::ce
