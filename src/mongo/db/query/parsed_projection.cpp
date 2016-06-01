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

#include "mongo/db/query/query_request.h"

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
    bool hasIndexKeyProjection = false;

    bool wantGeoNearPoint = false;
    bool wantGeoNearDistance = false;
    bool wantSortKey = false;

    // Until we see a positional or elemMatch operator we're normal.
    ArrayOpType arrayOpType = ARRAY_OP_NORMAL;

    // Fill out the returned obj.
    unique_ptr<ParsedProjection> pp(new ParsedProjection());
    pp->_hasId = true;

    for (auto&& elem : spec) {
        if (Object == elem.type()) {
            BSONObj obj = elem.embeddedObject();
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
                pp->_arrayFields.push_back(elem.fieldNameStringData());
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

                if (mongoutils::str::contains(elem.fieldName(), '.')) {
                    return Status(ErrorCodes::BadValue,
                                  "Cannot use $elemMatch projection on a nested field.");
                }

                arrayOpType = ARRAY_OP_ELEM_MATCH;

                // Create a MatchExpression for the elemMatch.
                BSONObj elemMatchObj = elem.wrap();
                invariant(elemMatchObj.isOwned());

                // We pass a null pointer instead of threading through the CollatorInterface. This
                // is ok because the parsed MatchExpression is not used after being created. We are
                // only parsing here in order to ensure that the elemMatch projection is valid.
                //
                // TODO: Is there a faster way of validating the elemMatchObj?
                const CollatorInterface* collator = nullptr;
                StatusWithMatchExpression statusWithMatcher =
                    MatchExpressionParser::parse(elemMatchObj, extensionsCallback, collator);
                if (!statusWithMatcher.isOK()) {
                    return statusWithMatcher.getStatus();
                }

                // Projections with $elemMatch aren't covered.
                requiresDocument = true;
                pp->_arrayFields.push_back(elem.fieldNameStringData());
            } else if (mongoutils::str::equals(e2.fieldName(), "$meta")) {
                // Field for meta must be top level.  We can relax this at some point.
                if (mongoutils::str::contains(elem.fieldName(), '.')) {
                    return Status(ErrorCodes::BadValue, "field for $meta cannot be nested");
                }

                // Make sure the argument to $meta is something we recognize.
                // e.g. {x: {$meta: "textScore"}}
                if (String != e2.type()) {
                    return Status(ErrorCodes::BadValue, "unexpected argument to $meta in proj");
                }

                if (e2.valuestr() != QueryRequest::metaTextScore &&
                    e2.valuestr() != QueryRequest::metaRecordId &&
                    e2.valuestr() != QueryRequest::metaIndexKey &&
                    e2.valuestr() != QueryRequest::metaGeoNearDistance &&
                    e2.valuestr() != QueryRequest::metaGeoNearPoint &&
                    e2.valuestr() != QueryRequest::metaSortKey) {
                    return Status(ErrorCodes::BadValue, "unsupported $meta operator: " + e2.str());
                }

                // This clobbers everything else.
                if (e2.valuestr() == QueryRequest::metaIndexKey) {
                    hasIndexKeyProjection = true;
                } else if (e2.valuestr() == QueryRequest::metaGeoNearDistance) {
                    wantGeoNearDistance = true;
                } else if (e2.valuestr() == QueryRequest::metaGeoNearPoint) {
                    wantGeoNearPoint = true;
                } else if (e2.valuestr() == QueryRequest::metaSortKey) {
                    wantSortKey = true;
                }

                // Of the $meta projections, only sortKey can be covered.
                if (e2.valuestr() != QueryRequest::metaSortKey) {
                    requiresDocument = true;
                }
                pp->_metaFields.push_back(elem.fieldNameStringData());
            } else {
                return Status(ErrorCodes::BadValue,
                              string("Unsupported projection option: ") + elem.toString());
            }
        } else if (mongoutils::str::equals(elem.fieldName(), "_id") && !elem.trueValue()) {
            pp->_hasId = false;
        } else {
            // Projections of dotted fields aren't covered.
            if (mongoutils::str::contains(elem.fieldName(), '.')) {
                requiresDocument = true;
            }

            if (elem.trueValue()) {
                pp->_includedFields.push_back(elem.fieldNameStringData());
            } else {
                pp->_excludedFields.push_back(elem.fieldNameStringData());
            }

            // If we haven't specified an include/exclude, initialize includeExclude. We expect
            // further include/excludes to match it.
            if (includeExclude == IncludeExclude::kUninitialized) {
                includeExclude =
                    elem.trueValue() ? IncludeExclude::kInclude : IncludeExclude::kExclude;
            } else if ((includeExclude == IncludeExclude::kInclude && !elem.trueValue()) ||
                       (includeExclude == IncludeExclude::kExclude && elem.trueValue())) {
                return Status(ErrorCodes::BadValue,
                              "Projection cannot have a mix of inclusion and exclusion.");
            }
        }

        if (_isPositionalOperator(elem.fieldName())) {
            // Validate the positional op.
            if (!elem.trueValue()) {
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

            std::string after = mongoutils::str::after(elem.fieldName(), ".$");
            if (mongoutils::str::contains(after, ".$")) {
                mongoutils::str::stream ss;
                ss << "Positional projection '" << elem.fieldName() << "' contains "
                   << "the positional operator more than once.";
                return Status(ErrorCodes::BadValue, ss);
            }

            std::string matchfield = mongoutils::str::before(elem.fieldName(), '.');
            if (!_hasPositionalOperatorMatch(query, matchfield)) {
                mongoutils::str::stream ss;
                ss << "Positional projection '" << elem.fieldName() << "' does not "
                   << "match the query document.";
                return Status(ErrorCodes::BadValue, ss);
            }

            arrayOpType = ARRAY_OP_POSITIONAL;
            pp->_arrayFields.push_back(elem.fieldNameStringData());
        }
    }

    // If includeExclude is uninitialized or set to exclude fields, then we can't use an index
    // because we don't know what fields we're missing.
    if (includeExclude == IncludeExclude::kUninitialized ||
        includeExclude == IncludeExclude::kExclude) {
        requiresDocument = true;
    }

    pp->_isInclusionProjection = (includeExclude == IncludeExclude::kInclude);

    // The positional operator uses the MatchDetails from the query
    // expression to know which array element was matched.
    pp->_requiresMatchDetails = arrayOpType == ARRAY_OP_POSITIONAL;

    // Save the raw spec.  It should be owned by the QueryRequest.
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
        if (pp->_hasId) {
            pp->_requiredFields.push_back("_id");
        }

        // The only way we could be here is if spec is only simple non-dotted-field inclusions or
        // the $meta sortKey projection. Therefore we can iterate over spec to get the fields
        // required.
        BSONObjIterator srcIt(spec);
        while (srcIt.more()) {
            BSONElement elt = srcIt.next();
            // We've already handled the _id field before entering this loop.
            if (pp->_hasId && mongoutils::str::equals(elt.fieldName(), "_id")) {
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

namespace {

bool isPrefixOf(StringData first, StringData second) {
    if (first.size() >= second.size()) {
        return false;
    }

    return second.startsWith(first) && second[first.size()] == '.';
}

}  // namespace

bool ParsedProjection::isFieldRetainedExactly(StringData path) const {
    // If a path, or a parent or child of the path, is contained in _metaFields or in _arrayFields,
    // our output likely does not preserve that field.
    for (auto&& metaField : _metaFields) {
        if (path == metaField || isPrefixOf(path, metaField) || isPrefixOf(metaField, path)) {
            return false;
        }
    }

    for (auto&& arrayField : _arrayFields) {
        if (path == arrayField || isPrefixOf(path, arrayField) || isPrefixOf(arrayField, path)) {
            return false;
        }
    }

    if (path == "_id" || isPrefixOf("_id", path)) {
        return _hasId;
    }

    if (!_isInclusionProjection) {
        // If we are an exclusion projection, and the path, or a parent or child of the path, is
        // contained in _excludedFields, our output likely does not preserve that field.
        for (auto&& excluded : _excludedFields) {
            if (path == excluded || isPrefixOf(excluded, path) || isPrefixOf(path, excluded)) {
                return false;
            }
        }
    } else {
        // If we are an inclusion projection, we may include parents of this path, but we cannot
        // include children.
        bool fieldIsIncluded = false;
        // In a projection with several statements, the last one takes precedence. For example, the
        // projection {a: 1, a.b: 1} preserves 'a.b', but not 'a'.
        // TODO SERVER-6527: Simplify this when projections are no longer order-dependent.
        for (auto&& included : _includedFields) {
            if (path == included || isPrefixOf(included, path)) {
                fieldIsIncluded = true;
            } else if (isPrefixOf(path, included)) {
                fieldIsIncluded = false;
            }
        }

        if (!fieldIsIncluded) {
            return false;
        }
    }


    return true;
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
        // for which the path is not meaningful (eg. $where).
        if (!pathRawData) {
            return false;
        }
        std::string pathPrefix = mongoutils::str::before(pathRawData, '.');
        return pathPrefix == matchfield;
    }
    return false;
}

}  // namespace mongo
