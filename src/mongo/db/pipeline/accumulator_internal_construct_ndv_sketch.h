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
 * The output is an array of sketch documents, one per null/missing folding variant, in the
 * positional convention shared with the read path (see NdvStats in field_stats.idl): a single
 * field emits exactly one strict ($expr semantics) sketch, while n >= 2 canonically sorted
 * fields emit n+1, the all-strict tuple sketch first and then, per field, the variant that
 * folds that field's missing values into null (regular $eq semantics).
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
    // One sketch per folding variant: 1 for a single field, _fieldPaths.size() + 1 otherwise.
    std::vector<ce::HyperLogLog> _sketches;
};

}  // namespace mongo
