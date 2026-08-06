// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/compiler/ce/ndv/hyperloglog.h"
#include "mongo/db/query/compiler/ce/ndv/ndv_sketch_gen.h"

#include <vector>

namespace mongo {

/**
 * Builds HyperLogLog sketches over field values during analyze({mode: "ndv"}), from which the
 * number of distinct values (NDV) is estimated. Constant memory. Internal-only; no merging.
 *
 * Mirrors the $_internalConstructStats syntax:
 *     {$_internalConstructNdvSketch: {val: "$$ROOT", fields: ["a"]}}
 * 'val' evaluates to the (typically $project-narrowed) document; the accumulator extracts each
 * path in 'fields' itself, so absent fields stay distinguishable from null ones. Over the
 * documents {a: 5}, {a: 5} and {} the example above hashes 5, 5 and "missing" into the sketch
 * and emits
 *     sketches: [{ndv: 2, registers: BinData(0, "..."), precision: 14}]
 * where each element is an NdvSketch (ndv_sketch.idl).
 *
 * The output is an array of sketch documents, one per null/missing folding variant. A single
 * field needs exactly one variant ($expr semantics).
 * TODO SERVER-131763: composite NDV emits the other variants as further array elements.
 */
class AccumulatorInternalConstructNdvSketch final : public AccumulatorState {
public:
    static constexpr auto kName = "$_internalConstructNdvSketch"sv;

    const char* getOpName() const final {
        return kName.data();
    }

    AccumulatorInternalConstructNdvSketch(ExpressionContext* expCtx,
                                          const InternalConstructNdvSketchAccumulatorParams&);

    void processInternal(const Value& input, bool merging) final;
    Value getValue(bool toBeMerged) final;
    void reset() final;

    bool isCommutative() const final {
        return true;
    }

private:
    void _setMemoryUsage();

    std::vector<FieldPath> _fieldPaths;
    ce::HyperLogLog _hll;
};

}  // namespace mongo
