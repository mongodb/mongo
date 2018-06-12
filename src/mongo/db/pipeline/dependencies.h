/**
 * Copyright (c) 2014 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#pragma once

#include <boost/optional.hpp>
#include <set>
#include <string>

#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/variables.h"

namespace mongo {
class ParsedDeps;

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
     * Represents the type of metadata a pipeline might request.
     */
    enum class MetadataType {
        // The score associated with a text match.
        TEXT_SCORE,

        // The key to use for sorting.
        SORT_KEY,

        // The computed distance for a near query.
        GEO_NEAR_DISTANCE,

        // The point used in the computation of the GEO_NEAR_DISTANCE.
        GEO_NEAR_POINT,
    };

    /**
     * Represents what metadata is available on documents that are input to the pipeline.
     */
    enum MetadataAvailable {
        kNoMetadata = 0,
        kTextScore = 1 << 1,
        kGeoNearDistance = 1 << 2,
        kGeoNearPoint = 1 << 3,
    };

    /**
     * Represents a state where all geo metadata is available.
     */
    static constexpr auto kAllGeoNearDataAvailable =
        MetadataAvailable(MetadataAvailable::kGeoNearDistance | MetadataAvailable::kGeoNearPoint);

    /**
     * Represents a state where all metadata is available.
     */
    static constexpr auto kAllMetadataAvailable =
        MetadataAvailable(kTextScore | kGeoNearDistance | kGeoNearPoint);

    DepsTracker(MetadataAvailable metadataAvailable = kNoMetadata)
        : _metadataAvailable(metadataAvailable) {}

    /**
     * Returns a projection object covering the dependencies tracked by this class.
     */
    BSONObj toProjection() const;

    boost::optional<ParsedDeps> toParsedDeps() const;

    bool hasNoRequirements() const {
        return fields.empty() && !needWholeDocument && !_needTextScore;
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
    MetadataAvailable getMetadataAvailable() const {
        return _metadataAvailable;
    }

    /**
     * Returns true if the DepsTracker the metadata 'type' is available to the pipeline. It is
     * illegal to call this with MetadataType::SORT_KEY, since the sort key will always be available
     * if needed.
     */
    bool isMetadataAvailable(MetadataType type) const;

    /**
     * Sets whether or not metadata 'type' is required. Throws if 'required' is true but that
     * metadata is not available to the pipeline.
     *
     * Except for MetadataType::SORT_KEY, once 'type' is required, it cannot be unset.
     */
    void setNeedsMetadata(MetadataType type, bool required);

    /**
     * Returns true if the DepsTracker requires that metadata of type 'type' is present.
     */
    bool getNeedsMetadata(MetadataType type) const;

    /**
     * Returns true if there exists a type of metadata required by the DepsTracker.
     */
    bool getNeedsAnyMetadata() const {
        return _needTextScore || _needSortKey || _needGeoNearDistance || _needGeoNearPoint;
    }

    /**
     * Returns a vector containing all the types of metadata required by this DepsTracker.
     */
    std::vector<MetadataType> getAllRequiredMetadataTypes() const;

    std::set<std::string> fields;    // Names of needed fields in dotted notation.
    std::set<Variables::Id> vars;    // IDs of referenced variables.
    bool needWholeDocument = false;  // If true, ignore 'fields'; the whole document is needed.

private:
    /**
     * Appends the meta projections for the sort key and/or text score to 'bb' if necessary. Returns
     * true if either type of metadata was needed, and false otherwise.
     */
    bool _appendMetaProjections(BSONObjBuilder* bb) const;

    MetadataAvailable _metadataAvailable;

    // Each member variable influences a different $meta projection.
    bool _needTextScore = false;        // {$meta: "textScore"}
    bool _needSortKey = false;          // {$meta: "sortKey"}
    bool _needGeoNearDistance = false;  // {$meta: "geoNearDistance"}
    bool _needGeoNearPoint = false;     // {$meta: "geoNearPoint"}
};

/**
 * This class is designed to quickly extract the needed fields from a BSONObj into a Document.
 * It should only be created by a call to DepsTracker::ParsedDeps
 */
class ParsedDeps {
public:
    Document extractFields(const BSONObj& input) const;

private:
    friend struct DepsTracker;  // so it can call constructor
    explicit ParsedDeps(Document&& fields) : _fields(std::move(fields)), _nFields(_fields.size()) {}

    Document _fields;
    int _nFields;  // Cache the number of top-level fields needed.
};
}
