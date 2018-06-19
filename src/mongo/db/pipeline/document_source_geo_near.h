/**
 * Copyright (C) 2016 MongoDB Inc.
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
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/field_path.h"

namespace mongo {

class DocumentSourceGeoNear : public DocumentSource, public NeedsMergerDocumentSource {
public:
    static constexpr StringData kKeyFieldName = "key"_sd;
    static constexpr auto kStageName = "$geoNear";

    /**
     * Only exposed for testing.
     */
    static boost::intrusive_ptr<DocumentSourceGeoNear> create(
        const boost::intrusive_ptr<ExpressionContext>&);

    const char* getSourceName() const final {
        return kStageName;
    }

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        return {StreamType::kStreaming,
                PositionRequirement::kFirst,
                HostTypeRequirement::kAnyShard,
                DiskUseRequirement::kNoDiskUse,
                FacetRequirement::kNotAllowed,
                TransactionRequirement::kAllowed};
    }

    /**
     * DocumentSourceGeoNear should always be replaced by a DocumentSourceGeoNearCursor before
     * executing a pipeline, so this method should never be called.
     */
    GetNextResult getNext() final {
        MONGO_UNREACHABLE;
    }

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pCtx);

    /**
     * A query predicate to apply to the documents in addition to the "near" predicate.
     */
    BSONObj getQuery() const {
        return query;
    };

    /**
     * The field in which the computed distance will be stored.
     */
    FieldPath getDistanceField() const {
        return *distanceField;
    }

    /**
     * The field in which the matching point will be stored, if requested.
     */
    boost::optional<FieldPath> getLocationField() const {
        return includeLocs;
    }

    /**
     * The field over which to apply the "near" predicate, if specified.
     */
    boost::optional<FieldPath> getKeyField() const {
        return keyFieldPath;
    }

    /**
     * A scaling factor to apply to the distance, if specified by the user.
     */
    boost::optional<double> getDistanceMultiplier() const {
        return distanceMultiplier;
    }

    GetDepsReturn getDependencies(DepsTracker* deps) const final;

    /**
     * Returns true if the $geoNear specification requires the geoNear point metadata.
     */
    bool needsGeoNearPoint() const;

    /**
     * Converts this $geoNear aggregation stage into an equivalent $near or $nearSphere query on
     * 'nearFieldName'.
     */
    BSONObj asNearQuery(StringData nearFieldName) const;

    /**
     * This document source is sent as-is to the shards.
     */
    boost::intrusive_ptr<DocumentSource> getShardSource() final {
        return this;
    }

    /**
     * In a sharded cluster, this becomes a merge sort by distance, from nearest to furthest.
     */
    std::list<boost::intrusive_ptr<DocumentSource>> getMergeSources() final;

private:
    explicit DocumentSourceGeoNear(const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    /**
     * Parses the fields in the object 'options', throwing if an error occurs.
     */
    void parseOptions(BSONObj options);

    // These fields describe the command to run.
    // 'coords' and 'distanceField' are required; the rest are optional.
    BSONObj coords;  // "near" option, but near is a reserved keyword on windows
    bool coordsIsArray;
    std::unique_ptr<FieldPath> distanceField;  // Using unique_ptr because FieldPath can't be empty
    BSONObj query;
    bool spherical;
    boost::optional<double> maxDistance;
    boost::optional<double> minDistance;
    boost::optional<double> distanceMultiplier;
    boost::optional<FieldPath> includeLocs;
    boost::optional<FieldPath> keyFieldPath;
};
}  // namespace mongo
