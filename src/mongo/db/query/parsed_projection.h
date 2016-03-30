/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

class ParsedProjection {
public:
    // TODO: this is duplicated in here and in the proj exec code.  When we have
    // ProjectionExpression we can remove dups.
    enum ArrayOpType { ARRAY_OP_NORMAL = 0, ARRAY_OP_ELEM_MATCH, ARRAY_OP_POSITIONAL };

    /**
     * Parses the projection 'spec' and checks its validity with respect to the query 'query'.
     * Puts covering information into 'out'.
     *
     * Returns Status::OK() if it's a valid spec.
     * Returns a Status indicating how it's invalid otherwise.
     */
    static Status make(const BSONObj& spec,
                       const MatchExpression* const query,
                       ParsedProjection** out,
                       const ExtensionsCallback& extensionsCallback);

    /**
     * Returns true if the projection requires match details from the query,
     * and false otherwise.
     */
    bool requiresMatchDetails() const {
        return _requiresMatchDetails;
    }

    /**
     * Is the full document required to compute this projection?
     */
    bool requiresDocument() const {
        return _requiresDocument;
    }

    /**
     * If requiresDocument() == false, what fields are required to compute
     * the projection?
     *
     * Returned StringDatas are owned by, and have the lifetime of, the ParsedProjection.
     */
    const std::vector<StringData>& getRequiredFields() const {
        return _requiredFields;
    }

    /**
     * Get the raw BSONObj proj spec obj
     */
    const BSONObj& getProjObj() const {
        return _source;
    }

    /**
     * Does the projection want geoNear metadata?  If so any geoNear stage should include them.
     */
    bool wantGeoNearDistance() const {
        return _wantGeoNearDistance;
    }

    bool wantGeoNearPoint() const {
        return _wantGeoNearPoint;
    }

    bool wantIndexKey() const {
        return _returnKey;
    }

    bool wantSortKey() const {
        return _wantSortKey;
    }

    /**
     * Returns true if the element at 'path' is preserved entirely after this projection is applied,
     * and false otherwise. For example, the projection {a: 1} will preserve the element located at
     * 'a.b', and the projection {'a.b': 0} will not preserve the element located at 'a'.
     */
    bool isFieldRetainedExactly(StringData path) const;

private:
    /**
     * Must go through ::make
     */
    ParsedProjection() = default;

    /**
     * Returns true if field name refers to a positional projection.
     */
    static bool _isPositionalOperator(const char* fieldName);

    /**
     * Returns true if the MatchExpression 'query' queries against
     * the field named by 'matchfield'. This deeply traverses logical
     * nodes in the matchfield and returns true if any of the children
     * have the field (so if 'query' is {$and: [{a: 1}, {b: 1}]} and
     * 'matchfield' is "b", the return value is true).
     *
     * Does not take ownership of 'query'.
     */
    static bool _hasPositionalOperatorMatch(const MatchExpression* const query,
                                            const std::string& matchfield);

    // Track fields needed by the projection so that the query planner can perform projection
    // analysis and possibly give us a covered projection.
    //
    // StringDatas are owned by the ParsedProjection.
    //
    // The order of the fields is the order they were in the projection object.
    std::vector<StringData> _requiredFields;

    // _hasId determines whether the _id field of the input is included in the output.
    bool _hasId = false;

    // Tracks the fields that have been explicitly included and excluded, respectively, in this
    // projection.
    //
    // StringDatas are owned by the ParsedProjection.
    //
    // The ordering of the paths is the order that they appeared within the projection, and should
    // be maintained.
    std::vector<StringData> _includedFields;
    std::vector<StringData> _excludedFields;

    // Tracks fields referenced within the projection that are meta or array projections,
    // respectively.
    //
    // StringDatas are owned by the ParsedProjection.
    //
    // The order of the fields is not significant.
    std::vector<StringData> _metaFields;
    std::vector<StringData> _arrayFields;

    // Tracks whether this projection is an inclusion projection, i.e., {a: 1}, or an exclusion
    // projection, i.e., {a: 0}. The projection {_id: 0} is ambiguous but will result in this field
    // being set to false.
    bool _isInclusionProjection = false;

    bool _requiresMatchDetails = false;

    bool _requiresDocument = true;

    BSONObj _source;

    bool _wantGeoNearDistance = false;

    bool _wantGeoNearPoint = false;

    bool _returnKey = false;

    // Whether this projection includes a sortKey meta-projection.
    bool _wantSortKey = false;
};

}  // namespace mongo
