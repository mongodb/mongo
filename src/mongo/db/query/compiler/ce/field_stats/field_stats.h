// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/query/compiler/ce/field_stats/field_stats_gen.h"
#include "mongo/db/query/compiler/stats/value_utils.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/inlined_vector.h>
#include <boost/container/flat_map.hpp>

namespace mongo::ce {

inline constexpr std::int32_t kFieldStatsSchemaVersion = 1;

// Maximum number of fields a composite field statistic may cover. Enforced on user input by the
// field statistics builder.
inline constexpr size_t kMaxFieldStatsFields = 3;

/**
 * Builds the '_id' string for a field-stats document, e.g. "1|<collection uuid>|<field path>".
 * 'fieldPaths' must not be empty.
 *
 * A string key deliberately avoids BSON object equality, which is field-order sensitive and
 * therefore fragile as a lookup key. Shared by the write path (analyze command) and the read
 * path so both produce identical keys. The paths are sorted canonically, so the key is
 * independent of the order the caller lists them in, and '|' and '\' inside path names are
 * escaped so a path cannot fake a separator.
 */
std::string makeFieldStatsId(const UUID& collectionUuid, std::vector<std::string> fieldPaths);

void validateFieldStatsPath(std::string_view path);

/**
 * One monitored value of an MCV list: its estimated frequency and the error term bounding the
 * overestimation. The true frequency of the value is guaranteed to lie in [estFreq - err,
 * estFreq].
 */
struct McvEntryStats {
    size_t estFreq = 0;
    size_t err = 0;
};

/**
 * BSON-order comparison for SBEValue keys: an McvMap iterates in value order, and a lookup
 * matches the way an equality predicate compares values (numeric values compare numerically
 * across type tags). Lookups take a non-owning sbe::value::TagValueView, so a lookup miss never
 * frees a value borrowed from an equality predicate.
 */
struct SBEValueOrdering {
    using is_transparent = void;

    // Accepts any mix of SBEValue and TagValueView; any other type fails to compile at view().
    template <typename L, typename R>
    bool operator()(const L& lhs, const R& rhs) const {
        const auto [lTag, lVal] = view(lhs);
        const auto [rTag, rVal] = view(rhs);
        return stats::compareValues(lTag, lVal, rTag, rVal) < 0;
    }

private:
    static sbe::value::TagValueView view(const stats::SBEValue& v) {
        return {v.getTag(), v.getValue()};
    }
    static sbe::value::TagValueView view(const sbe::value::TagValueView& v) {
        return v;
    }
};

/**
 * An MCV list as a sorted vector keyed by the value itself: O(log n) binary-search lookup over
 * contiguous storage, iterated in value order. The list is built once at load time and then only
 * read, so the O(n) cost of an insert is not paid on the estimation path.
 */
using McvMap = boost::container::flat_map<stats::SBEValue, McvEntryStats, SBEValueOrdering>;

/**
 * The statistics for a single field, loaded from its persisted field-stats document.
 */
struct SingleFieldStats {
    /**
     * Returns the MCV entry whose value equals {tag, val}, or nullptr if the
     * value is not monitored.
     */
    const McvEntryStats* findMcv(sbe::value::TypeTags tag, sbe::value::Value val) const {
        const auto it = mcv.find(sbe::value::TagValueView{tag, val});
        return it == mcv.end() ? nullptr : &it->second;
    }

    /**
     * Validity checks.
     */
    Status validate() const;

    /**
     * Returns the number of documents not covered by the specific counters, never negative.
     */
    double residualDocsNum() const;

    /**
     * Returns the number of distinct values not covered by the MCV entries, null, missing, or
     * undefined. Never negative.
     * Missing and undefined hash equally in the NDV sketch (see hashValueForNdv()), so together
     * they take one distinct value. Documents with an array or object are not added to the NDV
     * sketch, so nothing is subtracted for them.
     */
    double residualNdv() const;

    /** Most common values for this field, keyed by the value itself. */
    McvMap mcv;

    /** NDV of the field, where null and missing count as distinct values. */
    double ndv = 0.0;

    /** Documents whose value for this field is an array or object. */
    /* TODO SERVER-139086 split nonScalarDocsNum into separate objectsNum and arraysNum counters */
    size_t nonScalarDocsNum = 0;
    /** Documents with an explicit null for this field. */
    size_t nullDocsNum = 0;
    /** Documents missing the field entirely. */
    size_t missingDocsNum = 0;
    /** Documents with BSON Undefined for this field. */
    size_t undefinedDocsNum = 0;
    /** Collection size at stats collection time. */
    size_t totalDocsNum = 0;
    /** Sum of the MCV entries guaranteed lower bounds (estFreq - err). */
    size_t headDocsNum = 0;
};

/**
 * The statistics for a set of n >= 2 fields, loaded from its persisted field-stats document.
 */
struct CompoundFieldStats {
    /**
     * Checks that 'ndvs' holds n + 2 entries for 2 <= n <= kMaxFieldStatsFields and that no NDV is
     * negative.
     */
    Status validate() const;

    /**
     * Caps each NDV at 'totalDocsNum'.
     */
    void capNdvs();

    /**
     * The NDVs of the tuple, one per persisted NDV sketch, in sketch order (see
     * NdvSketchBuilder::accumulate()), n = number of fields:
     * - [0]: every field counts null and missing as distinct values (strict);
     * - [1 .. n]: entry i counts a missing value of the field at position i - 1 of the sorted
     *   field set as null, and the other fields count null and missing as distinct values;
     * - [n + 1]: a missing value counts as null in every field.
     */
    absl::InlinedVector<double, kMaxFieldStatsFields + 2> ndvs;

    /** Collection size at stats collection time. */
    size_t totalDocsNum = 0;
};

/**
 * Converts a single-field statistics document into SingleFieldStats.
 *
 * Returns an error for a document without exactly one NDV and one MCV sketch, an MCV entry whose
 * err exceeds its estFreq, duplicate MCV values, or statistics that fail
 * SingleFieldStats::validate().
 */
StatusWith<SingleFieldStats> makeSingleFieldStatsFromDoc(const FieldStatsDoc& doc);

/**
 * Converts a statistics document over 'numFields' >= 2 fields into CompoundFieldStats: the NDVs of
 * all n + 2 sketches and 'totalDocsNum'.
 *
 * Returns an error for 'numFields' outside [2, kMaxFieldStatsFields], a document whose NDV sketches
 * do not match 'numFields', or statistics that fail CompoundFieldStats::validate().
 */
StatusWith<CompoundFieldStats> makeCompoundFieldStatsFromDoc(const FieldStatsDoc& doc,
                                                             size_t numFields);

}  // namespace mongo::ce
