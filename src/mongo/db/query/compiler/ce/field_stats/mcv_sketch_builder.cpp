// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/field_stats/mcv_sketch_builder.h"

#include "mongo/db/basic_types.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/value_size.h"
#include "mongo/db/query/compiler/stats/value_utils.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace mongo::ce {
namespace {

constexpr size_t kMaxMcvCountersPerField = 100;
// Bound retained memory and BSON output even when input values are large.
constexpr int kMaxMcvValueSizeBytes = 1024;

}  // namespace

McvSketchBuilder::McvSketchBuilder(size_t numFields) {
    for (size_t i = 0; i < numFields; ++i) {
        _sketches.emplace_back(kMaxMcvCountersPerField, /*collator=*/nullptr);
    }
}

void McvSketchBuilder::accumulate(std::span<const sbe::value::TagValueView> values) {
    tassert(13794504,
            "MCV input must match the configured field count",
            values.size() == _sketches.size());
    for (size_t i = 0; i < _sketches.size(); ++i) {
        const auto value = values[i];
        if (sbe::value::isNullish(value.tag) ||
            sbe::value::getApproximateSize(value.tag, value.value) > kMaxMcvValueSizeBytes) {
            continue;
        }
        _sketches[i].increment(stats::SBEValue(value.copy().releaseToRaw()));
    }
}

McvStats McvSketchBuilder::getStats() const {
    std::vector<McvSketch> sketches;
    sketches.reserve(_sketches.size());

    for (const auto& fieldSketch : _sketches) {
        auto counters = fieldSketch.getTopK();
        std::ranges::sort(counters, [](const auto& lhs, const auto& rhs) {
            return stats::compareValues(lhs.value.getTag(),
                                        lhs.value.getValue(),
                                        rhs.value.getTag(),
                                        rhs.value.getValue()) < 0;
        });

        std::vector<McvEntry> entries;
        entries.reserve(fieldSketch.numCounters());

        for (const auto& counter : counters) {
            BSONObjBuilder valueBuilder;
            sbe::bson::appendValueToBsonObj(
                valueBuilder, "value", counter.value.getTag(), counter.value.getValue());
            const auto valueBson = valueBuilder.obj();

            auto& entry = entries.emplace_back();
            entry.setValue(IDLAnyTypeOwned(valueBson.firstElement(), valueBson));
            entry.setEstFreq(static_cast<std::int64_t>(counter.estFreq));
            entry.setErr(static_cast<std::int64_t>(counter.err));
        }

        sketches.emplace_back(std::move(entries));
    }

    return McvStats(std::move(sketches));
}

}  // namespace mongo::ce
