// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/ndv/ndv_sketch_builder.h"

#include "mongo/db/query/compiler/ce/ndv/field_stats.h"
#include "mongo/db/query/compiler/ce/ndv/ndv_hashing.h"
#include "mongo/util/assert_util.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

#include <absl/container/inlined_vector.h>

namespace mongo::ce {
namespace {

using sbe::value::TagValueOwned;
using sbe::value::TagValueView;
using sbe::value::TypeTags;

struct NormalizedTuple {
    absl::InlinedVector<TagValueView, kNdvMaxFields> values;
    // Keep normalized DBRef storage alive while the views are consumed.
    absl::InlinedVector<TagValueOwned, kNdvMaxFields> ownedDbRefs;
};

NormalizedTuple normalizeTuple(std::span<const TagValueView> values) {
    NormalizedTuple tuple;

    tuple.values.assign(values.begin(), values.end());
    for (auto& value : tuple.values) {
        if (value.tag != TypeTags::bsonDBPointer) {
            continue;
        }

        const auto dbRef = sbe::value::getBsonDBPointerView(value.value);
        const auto nul = dbRef.ns.find('\0');
        if (nul == std::string_view::npos) {
            continue;
        }

        // Preserve the old Value conversion's namespace truncation in persisted hashes.
        tuple.ownedDbRefs.push_back(TagValueOwned::fromRaw(
            sbe::value::makeNewBsonDBPointer(dbRef.ns.substr(0, nul), dbRef.id)));
        value = tuple.ownedDbRefs.back().view();
    }

    return tuple;
}

long long roundNdvEstimate(double estimate) {
    // LLONG_MAX rounds up to 2^63 as a double, which is outside llround()'s range.
    constexpr auto kMaxNdv = std::numeric_limits<long long>::max();
    if (estimate >= static_cast<double>(kMaxNdv)) {
        return kMaxNdv;
    }
    return std::llround(estimate);
}

}  // namespace

NdvSketchBuilder::NdvSketchBuilder(size_t numFields) : _numFields(numFields) {
    tassert(13794500,
            "NDV sketch construction requires between 1 and 3 fields",
            numFields > 0 && numFields <= kNdvMaxFields);
    const auto numSketches = numFields == 1 ? 1 : numFields + 2;
    _sketches.assign(numSketches, uassertStatusOK(HyperLogLog::create(kPrecision)));
}

void NdvSketchBuilder::accumulateTuple(std::span<const TagValueView> values) {
    tassert(
        13794501, "NDV input must match the configured field count", values.size() == _numFields);

    auto normalized = normalizeTuple(values);
    auto& tuple = normalized.values;

    const uint64_t strictHash = hashValuesForNdv(tuple);
    _sketches.front().addHash(strictHash);
    if (_numFields == 1) {
        return;
    }

    size_t numMissing = 0;
    uint64_t allMatchHash = strictHash;
    for (size_t i = 0; i < tuple.size(); ++i) {
        auto& fieldSketch = _sketches[i + 1];
        if (tuple[i].tag != TypeTags::Nothing) {
            fieldSketch.addHash(strictHash);
            continue;
        }

        ++numMissing;
        tuple[i] = TagValueView::null();
        const auto foldedHash = hashValuesForNdv(tuple);
        tuple[i] = TagValueView::nothing();

        fieldSketch.addHash(foldedHash);
        allMatchHash = foldedHash;
    }

    // With at most one missing field, all-match reuses a hash computed above.
    if (numMissing <= 1) {
        _sketches.back().addHash(allMatchHash);
        return;
    }

    for (auto& value : tuple) {
        if (value.tag == TypeTags::Nothing) {
            value = TagValueView::null();
        }
    }

    _sketches.back().addHash(hashValuesForNdv(tuple));
}

std::vector<NdvSketch> NdvSketchBuilder::getSketches() const {
    std::vector<NdvSketch> sketches;
    sketches.reserve(_sketches.size());

    for (const auto& hll : _sketches) {
        const auto registers = hll.registers();

        NdvSketch sketch;
        sketch.setNdv(roundNdvEstimate(hll.estimate()));
        sketch.setRegisters(std::vector<std::uint8_t>(registers.begin(), registers.end()));
        sketch.setPrecision(static_cast<int>(hll.precision()));

        sketches.push_back(std::move(sketch));
    }

    return sketches;
}

}  // namespace mongo::ce
