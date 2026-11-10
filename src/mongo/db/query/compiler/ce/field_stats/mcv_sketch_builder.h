// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/ce/field_stats/field_stats_gen.h"
#include "mongo/db/query/compiler/stats/space_saving.h"

#include <cstddef>
#include <deque>
#include <span>

namespace mongo::ce {

/**
 * Computes independent per-field MCVs in tuple order, excluding null, undefined, and missing
 * values.
 */
class McvSketchBuilder {
public:
    explicit McvSketchBuilder(size_t numFields);

    /**
     * Values must follow the same field order on every call to match the corresponding sketches.
     * Values with an estimated memory size above 1 KiB are skipped.
     * Retained values are copied so they can outlive the caller's document.
     */
    void accumulate(std::span<const sbe::value::TagValueView> values);

    /**
     * Each retained value's true frequency lies in [estFreq - err, estFreq].
     */
    McvStats getStats() const;

private:
    // Deque constructs nonmovable sketches in place without relocating existing elements.
    std::deque<stats::SpaceSavingSketch> _sketches;
};

}  // namespace mongo::ce
