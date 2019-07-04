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

#include "mongo/db/exec/projection_exec.h"

#include "mongo/bson/mutable/document.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/query_request.h"
#include "mongo/db/update/path_support.h"
#include "mongo/util/decimal_counter.h"
#include "mongo/util/str.h"

namespace mongo {

using std::string;

namespace mmb = mongo::mutablebson;

ProjectionExec::ProjectionExec(OperationContext* opCtx,
                               const BSONObj& spec,
                               const MatchExpression* queryExpression,
                               const CollatorInterface* collator)
    : _source(spec), _queryExpression(queryExpression), _collator(collator) {
    // Whether we're including or excluding fields.
    enum class IncludeExclude { kUninitialized, kInclude, kExclude };
    IncludeExclude includeExclude = IncludeExclude::kUninitialized;

    BSONObjIterator it(_source);
    while (it.more()) {
        BSONElement e = it.next();

        if (Object == e.type()) {
            BSONObj obj = e.embeddedObject();
            invariant(1 == obj.nFields());

            BSONElement e2 = obj.firstElement();
            if (e2.fieldNameStringData() == "$slice") {
                if (e2.isNumber()) {
                    int i = e2.numberInt();
                    if (i < 0) {
                        add(e.fieldName(), i, -i);  // limit is now positive
                    } else {
                        add(e.fieldName(), 0, i);
                    }
                } else {
                    invariant(e2.type() == Array);
                    BSONObj arr = e2.embeddedObject();
                    invariant(2 == arr.nFields());

                    BSONObjIterator it(arr);
                    int skip = it.next().numberInt();
                    int limit = it.next().numberInt();

                    invariant(limit > 0);

                    add(e.fieldName(), skip, limit);
                }
            } else if (e2.fieldNameStringData() == "$elemMatch") {
                _arrayOpType = ARRAY_OP_ELEM_MATCH;

                // Create a MatchExpression for the elemMatch.
                BSONObj elemMatchObj = e.wrap();
                invariant(elemMatchObj.isOwned());
                _elemMatchObjs.push_back(elemMatchObj);
                boost::intrusive_ptr<ExpressionContext> expCtx(
                    new ExpressionContext(opCtx, _collator));
                StatusWithMatchExpression statusWithMatcher =
                    MatchExpressionParser::parse(elemMatchObj, std::move(expCtx));
                invariant(statusWithMatcher.isOK());
                // And store it in _matchers.
                _matchers[str::before(e.fieldNameStringData(), '.')] =
                    statusWithMatcher.getValue().release();

                add(e.fieldName(), true);
            } else if (e2.fieldNameStringData() == "$meta") {
                invariant(String == e2.type());
                if (e2.valuestr() == QueryRequest::metaTextScore) {
                    _meta[e.fieldName()] = META_TEXT_SCORE;
                    _needsTextScore = true;
                } else if (e2.valuestr() == QueryRequest::metaSortKey) {
                    _sortKeyMetaFields.push_back(e.fieldName());
                    _meta[_sortKeyMetaFields.back()] = META_SORT_KEY;
                    _needsSortKey = true;
                } else if (e2.valuestr() == QueryRequest::metaRecordId) {
                    _meta[e.fieldName()] = META_RECORDID;
                } else if (e2.valuestr() == QueryRequest::metaGeoNearPoint) {
                    _meta[e.fieldName()] = META_GEONEAR_POINT;
                    _needsGeoNearPoint = true;
                } else if (e2.valuestr() == QueryRequest::metaGeoNearDistance) {
                    _meta[e.fieldName()] = META_GEONEAR_DIST;
                    _needsGeoNearDistance = true;
                } else if (e2.valuestr() == QueryRequest::metaIndexKey) {
                    _hasReturnKey = true;
                } else {
                    // This shouldn't happen, should be caught by parsing.
                    MONGO_UNREACHABLE;
                }
            } else {
                MONGO_UNREACHABLE;
            }
        } else if ((e.fieldNameStringData() == "_id") && !e.trueValue()) {
            _includeID = false;
        } else {
            add(e.fieldName(), e.trueValue());

            // If we haven't specified an include/exclude, initialize includeExclude.
            if (includeExclude == IncludeExclude::kUninitialized) {
                includeExclude =
                    e.trueValue() ? IncludeExclude::kInclude : IncludeExclude::kExclude;
                _include = !e.trueValue();
            }
        }

        if (str::contains(e.fieldName(), ".$")) {
            _arrayOpType = ARRAY_OP_POSITIONAL;
        }
    }
}

ProjectionExec::~ProjectionExec() {
    for (FieldMap::const_iterator it = _fields.begin(); it != _fields.end(); ++it) {
        delete it->second;
    }

    for (Matchers::const_iterator it = _matchers.begin(); it != _matchers.end(); ++it) {
        delete it->second;
    }
}

void ProjectionExec::add(const string& field, bool include) {
    if (field.empty()) {  // this is the field the user referred to
        _include = include;
    } else {
        _include = !include;

        const size_t dot = field.find('.');
        const string subfield = field.substr(0, dot);
        const string rest = (dot == string::npos ? "" : field.substr(dot + 1, string::npos));

        ProjectionExec*& fm = _fields[subfield.c_str()];

        if (nullptr == fm) {
            fm = new ProjectionExec();
        }

        fm->add(rest, include);
    }
}

void ProjectionExec::add(const string& field, int skip, int limit) {
    _special = true;  // can't include or exclude whole object

    if (field.empty()) {  // this is the field the user referred to
        _skip = skip;
        _limit = limit;
    } else {
        const size_t dot = field.find('.');
        const string subfield = field.substr(0, dot);
        const string rest = (dot == string::npos ? "" : field.substr(dot + 1, string::npos));

        ProjectionExec*& fm = _fields[subfield.c_str()];

        if (nullptr == fm) {
            fm = new ProjectionExec();
        }

        fm->add(rest, skip, limit);
    }
}

//
// Execution
//

StatusWith<BSONObj> ProjectionExec::computeReturnKeyProjection(const BSONObj& indexKey,
                                                               const BSONObj& sortKey) const {
    BSONObjBuilder bob;

    if (!indexKey.isEmpty()) {
        bob.appendElements(indexKey);
    }

    // Must be possible to do both returnKey meta-projection and sortKey meta-projection so that
    // mongos can support returnKey.
    for (auto fieldName : _sortKeyMetaFields)
        bob.append(fieldName, sortKey);

    return bob.obj();
}

StatusWith<BSONObj> ProjectionExec::project(const BSONObj& in,
                                            const boost::optional<const double> geoDistance,
                                            const BSONObj& geoNearPoint,
                                            const BSONObj& sortKey,
                                            const boost::optional<const double> textScore,
                                            const int64_t recordId) const {
    BSONObjBuilder bob;
    MatchDetails matchDetails;

    // If it's a positional projection we need a MatchDetails.
    if (projectRequiresQueryExpression()) {
        matchDetails.requestElemMatchKey();
        invariant(nullptr != _queryExpression);
        invariant(_queryExpression->matchesBSON(in, &matchDetails));
    }

    Status projStatus = projectHelper(in, &bob, &matchDetails);
    if (!projStatus.isOK())
        return projStatus;
    else
        return {addMeta(std::move(bob), geoDistance, geoNearPoint, sortKey, textScore, recordId)};
}

StatusWith<BSONObj> ProjectionExec::projectCovered(const std::vector<IndexKeyDatum>& keyData,
                                                   const boost::optional<const double> geoDistance,
                                                   const BSONObj& geoNearPoint,
                                                   const BSONObj& sortKey,
                                                   const boost::optional<const double> textScore,
                                                   const int64_t recordId) const {
    invariant(!_include);
    BSONObjBuilder bob;
    // Go field by field.
    if (_includeID) {
        boost::optional<BSONElement> elt;
        // Sometimes the _id field doesn't exist...
        if ((elt = IndexKeyDatum::getFieldDotted(keyData, "_id")) && !elt->eoo()) {
            bob.appendAs(elt.get(), "_id");
        }
    }

    mmb::Document projectedDoc;

    for (auto&& specElt : _source) {
        if (specElt.fieldNameStringData() == "_id") {
            continue;
        }

        // $meta sortKey is the only meta-projection which is allowed to operate on index keys
        // rather than the full document.
        auto metaIt = _meta.find(specElt.fieldName());
        if (metaIt != _meta.end()) {
            invariant(metaIt->second == META_SORT_KEY);
            continue;
        }

        // $meta sortKey is also the only element with an Object value in the projection spec
        // that can operate on index keys rather than the full document.
        invariant(BSONType::Object != specElt.type());

        boost::optional<BSONElement> keyElt;
        // We can project a field that doesn't exist.  We just ignore it.
        if ((keyElt = IndexKeyDatum::getFieldDotted(keyData, specElt.fieldName())) &&
            !keyElt->eoo()) {
            FieldRef projectedFieldPath{specElt.fieldNameStringData()};
            auto setElementStatus =
                pathsupport::setElementAtPath(projectedFieldPath, keyElt.get(), &projectedDoc);
            if (!setElementStatus.isOK()) {
                return setElementStatus;
            }
        }
    }

    bob.appendElements(projectedDoc.getObject());
    return {addMeta(std::move(bob), geoDistance, geoNearPoint, sortKey, textScore, recordId)};
}

BSONObj ProjectionExec::addMeta(BSONObjBuilder bob,
                                const boost::optional<const double> geoDistance,
                                const BSONObj& geoNearPoint,
                                const BSONObj& sortKey,
                                const boost::optional<const double> textScore,
                                const int64_t recordId) const {
    for (MetaMap::const_iterator it = _meta.begin(); it != _meta.end(); ++it) {
        switch (it->second) {
            case META_GEONEAR_DIST:
                invariant(geoDistance);
                bob.append(it->first, geoDistance.get());
                break;
            case META_GEONEAR_POINT: {
                invariant(!geoNearPoint.isEmpty());
                auto& ptObj = geoNearPoint;
                if (ptObj.couldBeArray()) {
                    bob.appendArray(it->first, ptObj);
                } else {
                    bob.append(it->first, ptObj);
                }
                break;
            }
            case META_TEXT_SCORE:
                invariant(textScore);
                bob.append(it->first, textScore.get());
                break;
            case META_SORT_KEY: {
                invariant(!sortKey.isEmpty());
                bob.append(it->first, sortKey);
                break;
            }
            case META_RECORDID:
                invariant(recordId != 0);
                bob.append(it->first, recordId);
        }
    }
    return bob.obj();
}

Status ProjectionExec::projectHelper(const BSONObj& in,
                                     BSONObjBuilder* bob,
                                     const MatchDetails* details) const {
    const ArrayOpType& arrayOpType = _arrayOpType;

    BSONObjIterator it(in);
    while (it.more()) {
        BSONElement elt = it.next();

        // Case 1: _id
        if ("_id" == elt.fieldNameStringData()) {
            if (_includeID) {
                bob->append(elt);
            }
            continue;
        }

        // Case 2: no array projection for this field.
        Matchers::const_iterator matcher = _matchers.find(elt.fieldName());
        if (_matchers.end() == matcher) {
            Status s = append(bob, elt, details, arrayOpType);
            if (!s.isOK()) {
                return s;
            }
            continue;
        }

        // Case 3: field has array projection with $elemMatch specified.
        if (ARRAY_OP_ELEM_MATCH != arrayOpType) {
            return Status(ErrorCodes::BadValue, "Matchers are only supported for $elemMatch");
        }

        MatchDetails arrayDetails;
        arrayDetails.requestElemMatchKey();

        if (matcher->second->matchesBSON(in, &arrayDetails)) {
            FieldMap::const_iterator fieldIt = _fields.find(elt.fieldName());
            if (_fields.end() == fieldIt) {
                return Status(ErrorCodes::BadValue,
                              "$elemMatch specified, but projection field not found.");
            }

            BSONArrayBuilder arrBuilder;
            BSONObjBuilder subBob;

            if (in.getField(elt.fieldName()).eoo()) {
                return Status(ErrorCodes::InternalError,
                              "$elemMatch called on document element with eoo");
            }

            if (in.getField(elt.fieldName()).Obj().getField(arrayDetails.elemMatchKey()).eoo()) {
                return Status(ErrorCodes::InternalError,
                              "$elemMatch called on array element with eoo");
            }

            arrBuilder.append(
                in.getField(elt.fieldName()).Obj().getField(arrayDetails.elemMatchKey()));
            subBob.appendArray(matcher->first, arrBuilder.arr());
            Status status = append(bob, subBob.done().firstElement(), details, arrayOpType);
            if (!status.isOK()) {
                return status;
            }
        }
    }

    return Status::OK();
}

void ProjectionExec::appendArray(BSONObjBuilder* bob, const BSONObj& array, bool nested) const {
    int skip = nested ? 0 : _skip;
    int limit = nested ? -1 : _limit;

    if (skip < 0) {
        skip = std::max(0, skip + array.nFields());
    }

    DecimalCounter<size_t> index;
    BSONObjIterator it(array);
    while (it.more()) {
        BSONElement elt = it.next();

        if (skip) {
            skip--;
            continue;
        }

        if (limit != -1 && (limit-- == 0)) {
            break;
        }

        switch (elt.type()) {
            case Array: {
                BSONObjBuilder subBob;
                appendArray(&subBob, elt.embeddedObject(), true);
                bob->appendArray(StringData{index}, subBob.obj());
                ++index;
                break;
            }
            case Object: {
                BSONObjBuilder subBob;
                BSONObjIterator jt(elt.embeddedObject());
                while (jt.more()) {
                    append(&subBob, jt.next()).transitional_ignore();
                }
                bob->append(StringData{index}, subBob.obj());
                ++index;
                break;
            }
            default:
                if (_include) {
                    bob->appendAs(elt, StringData{index});
                    ++index;
                }
        }
    }
}

Status ProjectionExec::append(BSONObjBuilder* bob,
                              const BSONElement& elt,
                              const MatchDetails* details,
                              const ArrayOpType arrayOpType) const {
    // Skip if the field name matches a computed $meta field.
    // $meta projection fields can exist at the top level of
    // the result document and the field names cannot be dotted.
    if (_meta.find(elt.fieldName()) != _meta.end()) {
        return Status::OK();
    }

    FieldMap::const_iterator field = _fields.find(elt.fieldName());
    if (field == _fields.end()) {
        if (_include) {
            bob->append(elt);
        }
        return Status::OK();
    }

    ProjectionExec& subfm = *field->second;
    if ((subfm._fields.empty() && !subfm._special) ||
        !(elt.type() == Object || elt.type() == Array)) {
        // field map empty, or element is not an array/object
        if (subfm._include) {
            bob->append(elt);
        }
    } else if (elt.type() == Object) {
        BSONObjBuilder subBob;
        BSONObjIterator it(elt.embeddedObject());
        while (it.more()) {
            subfm.append(&subBob, it.next(), details, arrayOpType).transitional_ignore();
        }
        bob->append(elt.fieldName(), subBob.obj());
    } else {
        // Array
        BSONObjBuilder matchedBuilder;
        if (details && arrayOpType == ARRAY_OP_POSITIONAL) {
            // $ positional operator specified
            if (!details->hasElemMatchKey()) {
                str::stream error;
                error << "positional operator (" << elt.fieldName()
                      << ".$) requires corresponding field"
                      << " in query specifier";
                return Status(ErrorCodes::BadValue, error);
            }

            if (elt.embeddedObject()[details->elemMatchKey()].eoo()) {
                return Status(ErrorCodes::BadValue, "positional operator element mismatch");
            }

            // append as the first and only element in the projected array
            matchedBuilder.appendAs(elt.embeddedObject()[details->elemMatchKey()], "0");
        } else {
            // append exact array; no subarray matcher specified
            subfm.appendArray(&matchedBuilder, elt.embeddedObject());
        }
        bob->appendArray(elt.fieldName(), matchedBuilder.obj());
    }

    return Status::OK();
}

}  // namespace mongo
