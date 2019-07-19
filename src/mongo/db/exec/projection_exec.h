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

#include "boost/optional.hpp"

#include "mongo/db/exec/working_set.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/util/string_map.h"

namespace mongo {

class CollatorInterface;

/**
 * A fully-featured executor for find projection.
 */
class ProjectionExec {
public:
    /**
     * A .find() projection can have an array operation, either an elemMatch or positional (or
     * neither).
     */
    enum ArrayOpType { ARRAY_OP_NORMAL = 0, ARRAY_OP_ELEM_MATCH, ARRAY_OP_POSITIONAL };

    /**
     * Projections based on data computed while answering a query, or other metadata about a
     * document / query.
     */
    enum MetaProjection {
        META_GEONEAR_DIST,
        META_GEONEAR_POINT,
        META_RECORDID,
        META_SORT_KEY,
        META_TEXT_SCORE,
    };

    /**
     * TODO: document why we like StringMap so much here
     */
    typedef StringMap<ProjectionExec*> FieldMap;
    typedef StringMap<MatchExpression*> Matchers;
    typedef StringMap<MetaProjection> MetaMap;

    ProjectionExec(OperationContext* opCtx,
                   const BSONObj& spec,
                   const MatchExpression* queryExpression,
                   const CollatorInterface* collator);

    ~ProjectionExec();

    /**
     * Indicates whether this is a returnKey projection which should be performed via
     * 'computeReturnKeyProjection()'.
     */
    bool returnKey() const {
        return _hasReturnKey;
    }

    /**
     * Indicates whether 'sortKey' must be provided for 'computeReturnKeyProjection()' or
     * 'project()'.
     */
    bool needsSortKey() const {
        return _needsSortKey;
    }

    /**
     * Indicates whether 'geoDistance' must be provided for 'project()'.
     */
    bool needsGeoNearDistance() const {
        return _needsGeoNearDistance;
    }

    /**
     * Indicates whether 'geoNearPoint' must be provided for 'project()'.
     */
    bool needsGeoNearPoint() const {
        return _needsGeoNearPoint;
    }

    /**
     * Indicates whether 'textScore' is going to be used in 'project()'.
     */
    bool needsTextScore() const {
        return _needsTextScore;
    }

    /**
     * Returns false if there are no meta fields to project.
     */
    bool hasMetaFields() const {
        return !_meta.empty();
    }

    /**
     * Performs a returnKey projection and provides index keys rather than projection results.
     */
    StatusWith<BSONObj> computeReturnKeyProjection(const BSONObj& indexKey,
                                                   const BSONObj& sortKey) const;

    /**
     * Performs a projection given a BSONObj source. Meta fields must be provided if necessary.
     * Their necessity can be queried via the 'needs*' functions.
     */
    StatusWith<BSONObj> project(const BSONObj& in,
                                const boost::optional<const double> geoDistance = boost::none,
                                Value geoNearPoint = Value{},
                                const BSONObj& sortKey = BSONObj(),
                                const boost::optional<const double> textScore = boost::none,
                                const int64_t recordId = 0) const;

    /**
     * Performs a projection given index 'KeyData' to directly retrieve results. This function
     * handles projections which do not qualify for the ProjectionNodeCovered fast-path but are
     * still covered by indices.
     */
    StatusWith<BSONObj> projectCovered(
        const std::vector<IndexKeyDatum>& keyData,
        const boost::optional<const double> geoDistance = boost::none,
        Value geoNearPoint = Value{},
        const BSONObj& sortKey = BSONObj(),
        const boost::optional<const double> textScore = boost::none,
        const int64_t recordId = 0) const;

    /**
     * Determines if calls to the project method require that this object was created with the full
     * query expression. We may need it for MatchDetails.
     */
    bool projectRequiresQueryExpression() const {
        return ARRAY_OP_POSITIONAL == _arrayOpType;
    }


private:
    /**
     * Adds meta fields to the end of a projection.
     */
    BSONObj addMeta(BSONObjBuilder bob,
                    const boost::optional<const double> geoDistance,
                    Value geoNearPoint,
                    const BSONObj& sortKey,
                    const boost::optional<const double> textScore,
                    const int64_t recordId) const;

    //
    // Initialization
    //

    ProjectionExec() = default;

    /**
     * Add 'field' as a field name that is included or excluded as part of the projection.
     */
    void add(const std::string& field, bool include);

    /**
     * Add 'field' as a field name that is sliced as part of the projection.
     */
    void add(const std::string& field, int skip, int limit);

    //
    // Execution
    //

    /**
     * Apply the projection that 'this' represents to the object 'in'.  'details' is the result
     * of a match evaluation of the full query on the object 'in'.  This is only required
     * if the projection is positional.
     *
     * If the projection is successfully computed, returns Status::OK() and stuff the result in
     * 'bob'.
     * Otherwise, returns error.
     */
    Status projectHelper(const BSONObj& in,
                         BSONObjBuilder* bob,
                         const MatchDetails* details = nullptr) const;

    /**
     * Appends the element 'e' to the builder 'bob', possibly descending into sub-fields of 'e'
     * if needed.
     */
    Status append(BSONObjBuilder* bob,
                  const BSONElement& elt,
                  const MatchDetails* details = nullptr,
                  const ArrayOpType arrayOpType = ARRAY_OP_NORMAL) const;

    /**
     * Like append, but for arrays.
     * Deals with slice and calls appendArray to preserve the array-ness.
     */
    void appendArray(BSONObjBuilder* bob, const BSONObj& array, bool nested = false) const;

    // True if default at this level is to include.
    bool _include = true;

    // True if this level can't be skipped or included without recursing.
    bool _special = false;

    // We must group projections with common prefixes together.
    // TODO: benchmark std::vector<pair> vs map
    //
    // Projection is a rooted tree.  If we have {a.b: 1, a.c: 1} we don't want to
    // double-traverse the document when we're projecting it.  Instead, we have an entry in
    // _fields for 'a' with two sub projections: b:1 and c:1.
    FieldMap _fields;

    // The raw projection spec. that is passed into init(...)
    BSONObj _source;

    // Should we include the _id field?
    bool _includeID = true;

    // Arguments from the $slice operator.
    int _skip = 0;
    int _limit = -1;

    // Used for $elemMatch and positional operator ($)
    Matchers _matchers;

    // The matchers above point into BSONObjs and this is where those objs live.
    std::vector<BSONObj> _elemMatchObjs;

    ArrayOpType _arrayOpType = ARRAY_OP_NORMAL;

    // The full query expression. Used when we need MatchDetails.
    const MatchExpression* _queryExpression = nullptr;

    // Projections that aren't sourced from the document or index keys.
    MetaMap _meta;

    // Do we have a returnKey projection?  If so we *only* output the index key metadata, and
    // possibly the sort key for mongos to use.  If it's not found we output nothing.
    bool _hasReturnKey = false;

    // After parsing in the constructor, these fields will indicate the neccesity of metadata
    // for $meta projection.
    bool _needsSortKey = false;
    bool _needsGeoNearDistance = false;
    bool _needsGeoNearPoint = false;
    bool _needsTextScore = false;

    // The field names associated with any sortKey meta-projection(s). Empty if there is no sortKey
    // meta-projection.
    std::vector<StringData> _sortKeyMetaFields;

    // The collator this projection should use to compare strings. Needed for projection operators
    // that perform matching (e.g. elemMatch projection). If null, the collation is a simple binary
    // compare.
    const CollatorInterface* _collator = nullptr;
};

}  // namespace mongo
