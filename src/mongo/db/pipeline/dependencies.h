/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <boost/optional.hpp>
#include <set>
#include <string>

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/variables.h"

namespace mongo {

/**
 * Custom comparator that orders fieldpath strings by path prefix first, then by field.
 * This ensures that a parent field is ordered directly before its children.
 */
struct PathComparator {
    /* Returns true if the lhs value should sort before the rhs, false otherwise. */
    bool operator()(const std::string& lhs, const std::string& rhs) const;
};

/**
 * Set of field paths strings.  When iterated over, a parent path is seen directly before its
 * children (or descendants, more generally).  Eg., "a", "a.a", "a.b", "a-plus", "b".
 */
typedef std::set<std::string, PathComparator> OrderedPathSet;

/**
 * This struct allows components in an agg pipeline to report what they need from their input.
 */
struct DepsTracker {
    /**
     * Used by aggregation stages to report whether or not dependency resolution is complete, or
     * must continue to the next stage.
     */
    enum State {
        // The full object and all metadata may be required.
        NOT_SUPPORTED = 0x0,

        // Later stages could need either fields or metadata. For example, a $limit stage will pass
        // through all fields, and they may or may not be needed by future stages.
        SEE_NEXT = 0x1,

        // Later stages won't need more fields from input. For example, an inclusion projection like
        // {_id: 1, a: 1} will only output two fields, so future stages cannot possibly depend on
        // any other fields.
        EXHAUSTIVE_FIELDS = 0x2,

        // Later stages won't need more metadata from input. For example, a $group stage will group
        // documents together, discarding their text score and sort keys.
        EXHAUSTIVE_META = 0x4,

        // Later stages won't need either fields or metadata.
        EXHAUSTIVE_ALL = EXHAUSTIVE_FIELDS | EXHAUSTIVE_META,
    };

    /**
     * Represents a state where all geo metadata is available.
     */
    static constexpr auto kAllGeoNearData = QueryMetadataBitSet(
        (1 << DocumentMetadataFields::kGeoNearDist) | (1 << DocumentMetadataFields::kGeoNearPoint));

    /**
     * Represents a state where all metadata is available.
     */
    static constexpr auto kAllMetadata =
        QueryMetadataBitSet(~(1 << DocumentMetadataFields::kNumFields));

    /**
     * Represents a state where only text score metadata is available.
     */
    static constexpr auto kOnlyTextScore =
        QueryMetadataBitSet(1 << DocumentMetadataFields::kTextScore);

    /**
     * By default, certain metadata is unavailable to the pipeline, unless explicitly specified
     * that it is available. This state represents all metadata which is not available by default.
     */
    static constexpr auto kDefaultUnavailableMetadata = QueryMetadataBitSet(
        (1 << DocumentMetadataFields::kTextScore) | (1 << DocumentMetadataFields::kGeoNearDist) |
        (1 << DocumentMetadataFields::kGeoNearPoint));

    /**
     * Represents a state where no metadata is available.
     */
    static constexpr auto kNoMetadata = QueryMetadataBitSet();

    DepsTracker(const QueryMetadataBitSet& unavailableMetadata = kNoMetadata)
        : _unavailableMetadata{unavailableMetadata} {}

    enum class TruncateToRootLevel : bool { no, yes };

    /**
     * Return the set of dependencies with descendant paths removed.
     * For example ["a.b", "a.b.f", "c"] --> ["a.b", "c"].
     *
     * TruncateToRootLevel::yes requires all dependencies to be top-level.
     * The example above would return ["a", "c"]
     */
    static OrderedPathSet simplifyDependencies(OrderedPathSet dependencies,
                                               TruncateToRootLevel truncation);

    /**
     * Returns a projection object covering the non-metadata dependencies tracked by this class,
     * or empty BSONObj if the entire document is required. By default, the resulting project
     * will include the full, dotted field names of the dependencies. If 'truncationBehavior' is
     * set to TruncateToRootLevel::yes, the project will contain only the root-level field
     * names.
     */
    BSONObj toProjectionWithoutMetadata(
        TruncateToRootLevel truncationBehavior = TruncateToRootLevel::no) const;

    /**
     * Returns 'true' if there is no dependency on the input documents or metadata.
     *
     * Note: this method does not say anything about dependencies on variables, or on a random
     * generator.
     */
    bool hasNoRequirements() const {
        return fields.empty() && !needWholeDocument && !_metadataDeps.any();
    }

    /**
     * Returns 'true' if any of the DepsTracker's variables appear in the passed 'ids' set.
     */
    bool hasVariableReferenceTo(const std::set<Variables::Id>& ids) const {
        std::vector<Variables::Id> match;
        std::set_intersection(
            vars.begin(), vars.end(), ids.begin(), ids.end(), std::back_inserter(match));
        return !match.empty();
    }

    /**
     * Returns a value with bits set indicating the types of metadata not available to the
     * pipeline.
     */
    QueryMetadataBitSet getUnavailableMetadata() const {
        return _unavailableMetadata;
    }

    /**
     * Sets whether or not metadata 'type' is required. Throws if 'required' is true but that
     * metadata is not available to the pipeline.
     *
     * Except for MetadataType::SORT_KEY, once 'type' is required, it cannot be unset.
     */
    void setNeedsMetadata(DocumentMetadataFields::MetaType type, bool required);

    /**
     * Returns true if the DepsTracker requires that metadata of type 'type' is present.
     */
    bool getNeedsMetadata(DocumentMetadataFields::MetaType type) const {
        return _metadataDeps[type];
    }

    /**
     * Returns true if there exists a type of metadata required by the DepsTracker.
     */
    bool getNeedsAnyMetadata() const {
        return _metadataDeps.any();
    }

    /**
     * Return all of the metadata dependencies.
     */
    QueryMetadataBitSet& metadataDeps() {
        return _metadataDeps;
    }
    const QueryMetadataBitSet& metadataDeps() const {
        return _metadataDeps;
    }

    /**
     * Request that all metadata in the given QueryMetadataBitSet be added as dependencies. Throws a
     * UserException if any of the requested metadata fields have been marked as unavailable.
     */
    void requestMetadata(const QueryMetadataBitSet& metadata) {
        for (size_t i = 1; i < DocumentMetadataFields::kNumFields; ++i) {
            if (metadata[i]) {
                setNeedsMetadata(static_cast<DocumentMetadataFields::MetaType>(i), true);
            }
        }
    }

    /**
     * Return names of needed fields in dotted notation.  A custom comparator orders the fields
     * such that a parent is immediately before its children.
     */
    OrderedPathSet fields;

    std::set<Variables::Id> vars;    // IDs of referenced variables.
    bool needWholeDocument = false;  // If true, ignore 'fields'; the whole document is needed.

    // The output of some operators (such as $sample and $rand) depends on a source of fresh random
    // numbers. During execution this dependency is implicit, but during optimize() we need to know
    // about this dependency to decide whether it's ok to cache or reevaluate an operator.
    bool needRandomGenerator = false;

private:
    // Represents all metadata not available to the pipeline.
    QueryMetadataBitSet _unavailableMetadata;

    // Represents which metadata is used by the pipeline. This is populated while performing
    // dependency analysis.
    QueryMetadataBitSet _metadataDeps;
};

}  // namespace mongo
