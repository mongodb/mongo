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

#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/util/string_map.h"

namespace mongo {

    class LiteProjection {
    public:
        typedef StringMap<LiteProjection*> FieldMap;
        typedef StringMap<MatchExpression*> Matchers;

        enum ArrayOpType {
            ARRAY_OP_NORMAL = 0,
            ARRAY_OP_ELEM_MATCH,
            ARRAY_OP_POSITIONAL
        };

        /**
         * Use this to create a LiteProjection.
         *
         * 'query' is the user's query, used to ensure that the query is consistent with any
         * positional operators in the projection.
         *
         * 'projObj' is the projection.
         *
         * If 'projObj' is valid (with respect to 'query'), returns Status::OK() and populates *out.
         * Caller owns the pointer.
         *
         * Otherwise, returns an error.
         */
        static Status make(const BSONObj& query, const BSONObj& projObj, LiteProjection** out);

        ~LiteProjection();

        /**
         * Is the full document required to compute this projection?
         */
        bool requiresDocument() const {
            return _include || _hasNonSimple || _hasDottedField;
        }

        /**
         * Return the fields required to compute the projection in 'fields'.
         *
         * Assumes that requiresDocument() is false and may produce bogus results if
         * requiresDocument() is true.
         */
        void getRequiredFields(vector<string>* fields) const;

        /**
         * Return a StringData for which field we're projecting the text score into.
         */
        StringData getTextScoreName() const { return _textScoreFieldName; }

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
         * Used for debugging.
         */
        const BSONObj& getProjectionSpec() const { return _source; }

        StringData getTextScoreFieldName() const { return _textScoreFieldName; }

    private:
        friend class ProjectionStage;

        //
        // Initialization
        //

        /**
         * We keep this private so that one must call ::make to create a new instance.
         */
        LiteProjection();

        /**
         * Initialize the projection from the provided BSONObj.
         * Returns Status::OK() if the provided projection spec is valid.
         * Otherwise, returns an error.
         */
        Status init(const BSONObj& spec, const BSONObj& query);

        /**
         * Add 'field' to the set of things to be projected.
         * 'include' specifies whether or not 'field' is to be included or dropped.
         * 'skip' and 'limit' are for $slice.
         */
        void add(const string& field, bool include);
        void add(const string& field, int skip, int limit);

        //
        // Execution
        // TODO: Move into exec/projection.cpp or elsewhere.
        //

        /**
         * Appends the element 'e' to the builder 'bob', possibly descending into sub-fields of 'e'
         * if needed.
         */
        Status append(BSONObjBuilder* bob,
                      const BSONElement& elt,
                      const MatchDetails* details = NULL,
                      const ArrayOpType arrayOpType = ARRAY_OP_NORMAL) const;

        // XXX document
        void appendArray(BSONObjBuilder* bob, const BSONObj& array, bool nested = false) const;

        // True if default at this level is to include.
        bool _include;

        // True if this level can't be skipped or included without recursing.
        bool _special; 

        // TODO: benchmark vector<pair> vs map
        // XXX: document
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
        vector<BSONObj> _elemMatchObjs;

        ArrayOpType _arrayOpType;

        // Is there an elemMatch or positional operator?
        bool _hasNonSimple;

        // Is there a projection over a dotted field?
        bool _hasDottedField;

        // The field name for a $textScore projection
        StringData _textScoreFieldName;
    };

}  // namespace mongo
