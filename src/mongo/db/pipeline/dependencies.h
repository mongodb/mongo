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
     * Represents a state where no metadata is available.
     */
    static constexpr auto kNoMetadata = QueryMetadataBitSet();


    DepsTracker(QueryMetadataBitSet metadataAvailable = kNoMetadata)
        : _metadataAvailable(metadataAvailable) {}

    /**
     * Returns a projection object covering the non-metadata dependencies tracked by this class, or
     * empty BSONObj if the entire document is required.
     */
    BSONObj toProjectionWithoutMetadata() const;

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
     * Returns a value with bits set indicating the types of metadata available.
     */
    QueryMetadataBitSet getMetadataAvailable() const {
        return _metadataAvailable;
    }

    /**
     * Returns true if the DepsTracker the metadata 'type' is available to the pipeline. It is
     * illegal to call this with MetadataType::SORT_KEY, since the sort key will always be available
     * if needed.
     */
    bool isMetadataAvailable(DocumentMetadataFields::MetaType type) const {
        return _metadataAvailable[type];
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
     * Request that all metadata in the given QueryMetadataBitSet be added as dependencies.
     */
    void requestMetadata(const QueryMetadataBitSet& metadata) {
        _metadataDeps |= metadata;
    }

    std::set<std::string> fields;    // Names of needed fields in dotted notation.
    std::set<Variables::Id> vars;    // IDs of referenced variables.
    bool needWholeDocument = false;  // If true, ignore 'fields'; the whole document is needed.

private:
    // Represents all metadata available to the pipeline.
    QueryMetadataBitSet _metadataAvailable;

    // Represents which metadata is used by the pipeline. This is populated while performing
    // dependency analysis.
    QueryMetadataBitSet _metadataDeps;
};
}  // namespace mongo
