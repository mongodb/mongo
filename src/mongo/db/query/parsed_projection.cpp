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

#include "mongo/db/query/parsed_projection.h"

#include "mongo/db/query/lite_parsed_query.h"

namespace mongo {

using std::unique_ptr;
using std::string;

/**
 * Parses the projection 'spec' and checks its validity with respect to the query 'query'.
 * Puts covering information into 'out'.
 *
 * Does not take ownership of 'query'.
 *
 * Returns Status::OK() if it's a valid spec.
 * Returns a Status indicating how it's invalid otherwise.
 */
// static
Status ParsedProjection::make(const BSONObj& spec,
                              const MatchExpression* const query,
                              ParsedProjection** out,
                              const ExtensionsCallback& extensionsCallback) {
    // Whether we're including or excluding fields.
    enum class IncludeExclude { kUninitialized, kInclude, kExclude };
    IncludeExclude includeExclude = IncludeExclude::kUninitialized;

    bool requiresDocument = false;

    bool includeID = true;

    bool hasIndexKeyProjection = false;

    bool wantGeoNearPoint = false;
    bool wantGeoNearDistance = false;
    bool wantSortKey = false;

    // Until we see a positional or elemMatch operator we're normal.
    ArrayOpType arrayOpType = ARRAY_OP_NORMAL;

    BSONObjIterator it(spec);
    while (it.more()) {
        BSONElement e = it.next();

        if (Object == e.type()) {
            BSONObj obj = e.embeddedObject();
            if (1 != obj.nFields()) {
                return Status(ErrorCodes::BadValue, ">1 field in obj: " + obj.toString());
            }

            BSONElement e2 = obj.firstElement();
            if (mongoutils::str::equals(e2.fieldName(), "$slice")) {
                if (e2.isNumber()) {
                    // This is A-OK.
                } else if (e2.type() == Array) {
                    BSONObj arr = e2.embeddedObject();
                    if (2 != arr.nFields()) {
                        return Status(ErrorCodes::BadValue, "$slice array wrong size");
                    }

                    BSONObjIterator it(arr);
                    // Skip over 'skip'.
                    it.next();
                    int limit = it.next().numberInt();
                    if (limit <= 0) {
                        return Status(ErrorCodes::BadValue, "$slice limit must be positive");
                    }
                } else {
                    return Status(ErrorCodes::BadValue,
                                  "$slice only supports numbers and [skip, limit] arrays");
                }

                // Projections with $slice aren't covered.
                requiresDocument = true;
            } else if (mongoutils::str::equals(e2.fieldName(), "$elemMatch")) {
                // Validate $elemMatch arguments and dependencies.
                if (Object != e2.type()) {
                    return Status(ErrorCodes::BadValue,
                                  "elemMatch: Invalid argument, object required.");
                }

                if (ARRAY_OP_POSITIONAL == arrayOpType) {
                    return Status(ErrorCodes::BadValue,
                                  "Cannot specify positional operator and $elemMatch.");
                }

                if (mongoutils::str::contains(e.fieldName(), '.')) {
                    return Status(ErrorCodes::BadValue,
                                  "Cannot use $elemMatch projection on a nested field.");
                }

                arrayOpType = ARRAY_OP_ELEM_MATCH;

                // Create a MatchExpression for the elemMatch.
                BSONObj elemMatchObj = e.wrap();
                verify(elemMatchObj.isOwned());

                // TODO: Is there a faster way of validating the elemMatchObj?
                StatusWithMatchExpression statusWithMatcher =
                    MatchExpressionParser::parse(elemMatchObj, extensionsCallback);
                if (!statusWithMatcher.isOK()) {
                    return statusWithMatcher.getStatus();
                }

                // Projections with $elemMatch aren't covered.
                requiresDocument = true;
            } else if (mongoutils::str::equals(e2.fieldName(), "$meta")) {
                // Field for meta must be top level.  We can relax this at some point.
                if (mongoutils::str::contains(e.fieldName(), '.')) {
                    return Status(ErrorCodes::BadValue, "field for $meta cannot be nested");
                }

                // Make sure the argument to $meta is something we recognize.
                // e.g. {x: {$meta: "textScore"}}
                if (String != e2.type()) {
                    return Status(ErrorCodes::BadValue, "unexpected argument to $meta in proj");
                }

                if (e2.valuestr() != LiteParsedQuery::metaTextScore &&
                    e2.valuestr() != LiteParsedQuery::metaRecordId &&
                    e2.valuestr() != LiteParsedQuery::metaIndexKey &&
                    e2.valuestr() != LiteParsedQuery::metaGeoNearDistance &&
                    e2.valuestr() != LiteParsedQuery::metaGeoNearPoint &&
                    e2.valuestr() != LiteParsedQuery::metaSortKey) {
                    return Status(ErrorCodes::BadValue, "unsupported $meta operator: " + e2.str());
                }

                // This clobbers everything else.
                if (e2.valuestr() == LiteParsedQuery::metaIndexKey) {
                    hasIndexKeyProjection = true;
                } else if (e2.valuestr() == LiteParsedQuery::metaGeoNearDistance) {
                    wantGeoNearDistance = true;
                } else if (e2.valuestr() == LiteParsedQuery::metaGeoNearPoint) {
                    wantGeoNearPoint = true;
                } else if (e2.valuestr() == LiteParsedQuery::metaSortKey) {
                    wantSortKey = true;
                }

                // Of the $meta projections, only sortKey can be covered.
                if (e2.valuestr() != LiteParsedQuery::metaSortKey) {
                    requiresDocument = true;
                }
            } else {
                return Status(ErrorCodes::BadValue,
                              string("Unsupported projection option: ") + e.toString());
            }
        } else if (mongoutils::str::equals(e.fieldName(), "_id") && !e.trueValue()) {
            includeID = false;
        } else {
            // Projections of dotted fields aren't covered.
            if (mongoutils::str::contains(e.fieldName(), '.')) {
                requiresDocument = true;
            }

            // If we haven't specified an include/exclude, initialize includeExclude. We expect
            // further include/excludes to match it.
            if (includeExclude == IncludeExclude::kUninitialized) {
                includeExclude =
                    e.trueValue() ? IncludeExclude::kInclude : IncludeExclude::kExclude;
            } else if ((includeExclude == IncludeExclude::kInclude && !e.trueValue()) ||
                       (includeExclude == IncludeExclude::kExclude && e.trueValue())) {
                return Status(ErrorCodes::BadValue,
                              "Projection cannot have a mix of inclusion and exclusion.");
            }
        }

        if (_isPositionalOperator(e.fieldName())) {
            // Validate the positional op.
            if (!e.trueValue()) {
                return Status(ErrorCodes::BadValue,
                              "Cannot exclude array elements with the positional operator.");
            }

            if (ARRAY_OP_POSITIONAL == arrayOpType) {
                return Status(ErrorCodes::BadValue,
                              "Cannot specify more than one positional proj. per query.");
            }

            if (ARRAY_OP_ELEM_MATCH == arrayOpType) {
                return Status(ErrorCodes::BadValue,
                              "Cannot specify positional operator and $elemMatch.");
            }

            std::string after = mongoutils::str::after(e.fieldName(), ".$");
            if (mongoutils::str::contains(after, ".$")) {
                mongoutils::str::stream ss;
                ss << "Positional projection '" << e.fieldName() << "' contains "
                   << "the positional operator more than once.";
                return Status(ErrorCodes::BadValue, ss);
            }

            std::string matchfield = mongoutils::str::before(e.fieldName(), '.');
            if (!_hasPositionalOperatorMatch(query, matchfield)) {
                mongoutils::str::stream ss;
                ss << "Positional projection '" << e.fieldName() << "' does not "
                   << "match the query document.";
                return Status(ErrorCodes::BadValue, ss);
            }

            arrayOpType = ARRAY_OP_POSITIONAL;
        }
    }

    // If includeExclude is uninitialized or set to exclude fields, then we can't use an index
    // because we don't know what fields we're missing.
    if (includeExclude == IncludeExclude::kUninitialized ||
        includeExclude == IncludeExclude::kExclude) {
        requiresDocument = true;
    }

    // Fill out the returned obj.
    unique_ptr<ParsedProjection> pp(new ParsedProjection());

    // The positional operator uses the MatchDetails from the query
    // expression to know which array element was matched.
    pp->_requiresMatchDetails = arrayOpType == ARRAY_OP_POSITIONAL;

    // Save the raw spec.  It should be owned by the LiteParsedQuery.
    verify(spec.isOwned());
    pp->_source = spec;
    pp->_returnKey = hasIndexKeyProjection;
    pp->_requiresDocument = requiresDocument;

    // Add meta-projections.
    pp->_wantGeoNearPoint = wantGeoNearPoint;
    pp->_wantGeoNearDistance = wantGeoNearDistance;
    pp->_wantSortKey = wantSortKey;

    // If it's possible to compute the projection in a covered fashion, populate _requiredFields
    // so the planner can perform projection analysis.
    if (!pp->_requiresDocument) {
        if (includeID) {
            pp->_requiredFields.push_back("_id");
        }

        // The only way we could be here is if spec is only simple non-dotted-field inclusions or
        // the $meta sortKey projection. Therefore we can iterate over spec to get the fields
        // required.
        BSONObjIterator srcIt(spec);
        while (srcIt.more()) {
            BSONElement elt = srcIt.next();
            // We've already handled the _id field before entering this loop.
            if (includeID && mongoutils::str::equals(elt.fieldName(), "_id")) {
                continue;
            }
            // $meta sortKey should not be checked as a part of _requiredFields, since it can
            // potentially produce a covered projection as long as the sort key is covered.
            if (BSONType::Object == elt.type()) {
                dassert(elt.Obj() == BSON("$meta"
                                          << "sortKey"));
                continue;
            }
            if (elt.trueValue()) {
                pp->_requiredFields.push_back(elt.fieldName());
            }
        }
    }

    // returnKey clobbers everything except for sortKey meta-projection.
    if (hasIndexKeyProjection && !wantSortKey) {
        pp->_requiresDocument = false;
    }

    *out = pp.release();
    return Status::OK();
}

// static
bool ParsedProjection::_isPositionalOperator(const char* fieldName) {
    return mongoutils::str::contains(fieldName, ".$") &&
        !mongoutils::str::contains(fieldName, ".$ref") &&
        !mongoutils::str::contains(fieldName, ".$id") &&
        !mongoutils::str::contains(fieldName, ".$db");
}

// static
bool ParsedProjection::_hasPositionalOperatorMatch(const MatchExpression* const query,
                                                   const std::string& matchfield) {
    if (query->isLogical()) {
        for (unsigned int i = 0; i < query->numChildren(); ++i) {
            if (_hasPositionalOperatorMatch(query->getChild(i), matchfield)) {
                return true;
            }
        }
    } else {
        StringData queryPath = query->path();
        const char* pathRawData = queryPath.rawData();
        // We have to make a distinction between match expressions that are
        // initialized with an empty field/path name "" and match expressions
        // for which the path is not meaningful (eg. $where and the internal
        // expression type ALWAYS_FALSE).
        if (!pathRawData) {
            return false;
        }
        std::string pathPrefix = mongoutils::str::before(pathRawData, '.');
        return pathPrefix == matchfield;
    }
    return false;
}

}  // namespace mongo
