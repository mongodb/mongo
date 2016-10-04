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

#pragma once

#include "mongo/db/exec/working_set.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/util/string_map.h"

namespace mongo {

class CollatorInterface;

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
        META_IX_KEY,
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

    ProjectionExec(const BSONObj& spec,
                   const MatchExpression* queryExpression,
                   const CollatorInterface* collator,
                   const ExtensionsCallback& extensionsCallback);

    ~ProjectionExec();

    /**
     * Apply this projection to the 'member'.  Changes the type to OWNED_OBJ.
     */
    Status transform(WorkingSetMember* member) const;

private:
    //
    // Initialization
    //

    ProjectionExec();

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
    Status transform(const BSONObj& in,
                     BSONObjBuilder* bob,
                     const MatchDetails* details = NULL) const;

    /**
     * See transform(...) above.
     */
    bool transformRequiresDetails() const {
        return ARRAY_OP_POSITIONAL == _arrayOpType;
    }

    /**
     * Appends the element 'e' to the builder 'bob', possibly descending into sub-fields of 'e'
     * if needed.
     */
    Status append(BSONObjBuilder* bob,
                  const BSONElement& elt,
                  const MatchDetails* details = NULL,
                  const ArrayOpType arrayOpType = ARRAY_OP_NORMAL) const;

    /**
     * Like append, but for arrays.
     * Deals with slice and calls appendArray to preserve the array-ness.
     */
    void appendArray(BSONObjBuilder* bob, const BSONObj& array, bool nested = false) const;

    // True if default at this level is to include.
    bool _include;

    // True if this level can't be skipped or included without recursing.
    bool _special;

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
    bool _includeID;

    // Arguments from the $slice operator.
    int _skip;
    int _limit;

    // Used for $elemMatch and positional operator ($)
    Matchers _matchers;

    // The matchers above point into BSONObjs and this is where those objs live.
    std::vector<BSONObj> _elemMatchObjs;

    ArrayOpType _arrayOpType;

    // The full query expression.  Used when we need MatchDetails.
    const MatchExpression* _queryExpression;

    // Projections that aren't sourced from the document or index keys.
    MetaMap _meta;

    // Do we have a returnKey projection?  If so we *only* output the index key metadata, and
    // possibly the sort key for mongos to use.  If it's not found we output nothing.
    bool _hasReturnKey;

    // The field names associated with any sortKey meta-projection(s). Empty if there is no sortKey
    // meta-projection.
    std::vector<StringData> _sortKeyMetaFields;

    // The collator this projection should use to compare strings. Needed for projection operators
    // that perform matching (e.g. elemMatch projection). If null, the collation is a simple binary
    // compare.
    const CollatorInterface* _collator = nullptr;
};

}  // namespace mongo
