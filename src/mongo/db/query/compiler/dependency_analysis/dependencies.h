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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/pipeline/variables.h"

#include <compare>
#include <cstddef>
#include <set>
#include <string>

#include <boost/optional.hpp>

namespace mongo {

/**
 * Custom comparator that orders fieldpath strings by path prefix first, then by field.
 * This ensures that a parent field is ordered directly before its children.
 */
struct PathComparator {
    using is_transparent = void;

    /* Returns true if the lhs value should sort before the rhs, false otherwise. */
    bool operator()(StringData lhs, StringData rhs) const;
};

/**
 * Three way (<=>) version of PathComparator.
 * See PathComparator for sorting semantics.
 */
struct ThreeWayPathComparator {
    using is_transparent = void;

    /* Returns strong_ordering::less, equal, greater indicating the relation lhs <=> rhs */
    std::strong_ordering operator()(StringData lhs, StringData rhs) const;
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
    struct NoMetadataValidation {
        // Nothing.
    };

    /**
     * std::variant type used for tracking metadata dependency validation (i.e., if a metadata field
     * is requested, validate that it will provided to the pipeline), used for _availableMetadata.
     */
    using MetadataDependencyValidation = std::variant<NoMetadataValidation, QueryMetadataBitSet>;

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
    static constexpr auto kAllGeoNearData =
        QueryMetadataBitSet((1 << DocumentMetadataFields::MetaType::kGeoNearDist) |
                            (1 << DocumentMetadataFields::MetaType::kGeoNearPoint));

    /**
     * Represents a state where all metadata is available.
     */
    static constexpr auto kAllMetadata =
        QueryMetadataBitSet(~(1 << DocumentMetadataFields::MetaType::kNumFields));

    /**
     * Represents a state where only text score metadata is available.
     */
    static constexpr auto kOnlyTextScore =
        QueryMetadataBitSet((1 << DocumentMetadataFields::MetaType::kTextScore) |
                            (1 << DocumentMetadataFields::MetaType::kScore));

    /**
     * Represents a state where no metadata is available.
     */
    static constexpr auto kNoMetadata = QueryMetadataBitSet();

    /**
     * If left unspecified, default to NoMetadataValidation(), which means we don't want to
     * track available metadata and will skip dependency validation.
     */
    explicit DepsTracker(MetadataDependencyValidation availableMetadata = NoMetadataValidation())
        : _availableMetadata{availableMetadata} {}

    enum class TruncateToRootLevel : bool { no, yes };

    /**
     * Return the set of dependencies with descendant paths removed.
     * For example ["a.b", "a.b.f", "c"] --> ["a.b", "c"].
     *
     * TruncateToRootLevel::yes requires all dependencies to be top-level.
     * The example above would return ["a", "c"]
     */
    static OrderedPathSet simplifyDependencies(const OrderedPathSet& dependencies,
                                               TruncateToRootLevel truncation);

    /**
     * Helper function to determine whether the query has a dependency on the $text score metadata.
     *
     * TODO SERVER-100676 Consider finding a better home for this helper.
     */
    static bool needsTextScoreMetadata(const QueryMetadataBitSet& metadataDeps);

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
     * Returns a value with bits set indicating the types of metadata available to the pipeline. If
     * available metadata isn't being tracked, returns kAllMetadata.
     */
    QueryMetadataBitSet getAvailableMetadata() const {
        return std::visit(
            OverloadedVisitor{
                [](NoMetadataValidation) { return kAllMetadata; },
                [](QueryMetadataBitSet availableMetadata) { return availableMetadata; },
            },
            _availableMetadata);
    }

    /**
     * Marks that the given metadata type (or set of metadata types) will be generated and is
     * available for the rest of the pipeline to access.
     *
     * _availableMetadata is typically initialized in the DepsTracker constructor with metadata
     * fields populated by the first stage of the pipeline. This method is then needed to signal
     * that a stage (like $score) anywhere else in the pipeline will generate metadata that's only
     * available to the downstream stages. For example, this makes sure we reject the pipeline
     * [{$project "score" meta field}, {$score}].
     */
    void setMetadataAvailable(DocumentMetadataFields::MetaType type);
    void setMetadataAvailable(const QueryMetadataBitSet& metadata);

    /**
     * Clears the set of available metadata. This is used for tracking and validating metadata
     * dependencies. When walking a pipeline, we may encounter a stage that destroys per-document
     * metadata, so we must mark that downstream stages do not have access to any
     * previously-existing metadata.
     *
     * TODO SERVER-100902 Split $meta validation logic out of DepsTracker.
     */
    void clearMetadataAvailable();

    /**
     * Marks that the given metadata type (or set of metadata types) is required by this pipeline,
     * because the pipeline wants to read that metadata type for some purpose.
     *
     * If we are validating metadata requests and the type is marked as one that should be
     * validated, throws a UserException if the type has not been marked as available.
     */
    void setNeedsMetadata(DocumentMetadataFields::MetaType type);
    void setNeedsMetadata(const QueryMetadataBitSet& metadata);

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
     * Return names of needed fields in dotted notation.  A custom comparator orders the fields
     * such that a parent is immediately before its children.
     */
    OrderedPathSet fields;

    bool needWholeDocument = false;  // If true, ignore 'fields'; the whole document is needed.

    // The output of some operators (such as $sample and $rand) depends on a source of fresh random
    // numbers. During execution this dependency is implicit, but during optimize() we need to know
    // about this dependency to decide whether it's ok to cache or reevaluate an operator.
    bool needRandomGenerator = false;

private:
    // Struct to track metadata dependency validation: either NoMetadataValidation, or a bitset of
    // metadata fields that have been marked available to the pipeline. Used for validating that
    // downstream metadata dependencies are requesting metadata that will actually be available to
    // the pipeline.
    MetadataDependencyValidation _availableMetadata;

    // Represents which metadata is used by the pipeline. This is populated while performing
    // dependency analysis.
    QueryMetadataBitSet _metadataDeps;
};

}  // namespace mongo
