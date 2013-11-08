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

#include "mongo/db/query/lite_projection.h"

#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    // static
    Status LiteProjection::make(const BSONObj& query,
                                const BSONObj& projObj,
                                LiteProjection** out) {

        auto_ptr<LiteProjection> proj(new LiteProjection());

        Status initStatus = proj->init(projObj, query);
        if (!initStatus.isOK()) {
            return initStatus;
        }

        *out = proj.release();
        return Status::OK();
    }

    LiteProjection::LiteProjection()
        : _include(true),
          _special(false),
          _includeID(true),
          _skip(0),
          _limit(-1),
          _arrayOpType(ARRAY_OP_NORMAL),
          _hasNonSimple(false),
          _hasDottedField(false) { }

    LiteProjection::~LiteProjection() {
        for (FieldMap::const_iterator it = _fields.begin(); it != _fields.end(); ++it) {
            delete it->second;
        }

        for (Matchers::const_iterator it = _matchers.begin(); it != _matchers.end(); ++it) {
            delete it->second;
        }
    }

    Status LiteProjection::init(const BSONObj& spec, const BSONObj& query) {
        verify(_source.isEmpty());
        // Save the raw obj.
        _source = spec;
        verify(_source.isOwned());

        // Are we including or excluding fields?
        // -1 when we haven't initialized it.
        // 1 when we're including
        // 0 when we're excluding.
        int include_exclude = -1;

        BSONObjIterator it(_source);
        while (it.more()) {
            BSONElement e = it.next();

            if (!e.isNumber()) { _hasNonSimple = true; }

            if (Object == e.type()) {
                BSONObj obj = e.embeddedObject();
                BSONElement e2 = obj.firstElement();
                if (mongoutils::str::equals(e2.fieldName(), "$slice")) {
                    if (e2.isNumber()) {
                        int i = e2.numberInt();
                        if (i < 0) {
                            add(e.fieldName(), i, -i); // limit is now positive
                        }
                        else {
                            add(e.fieldName(), 0, i);
                        }
                    }
                    else if (e2.type() == Array) {
                        BSONObj arr = e2.embeddedObject();
                        if (2 != arr.nFields()) {
                            return Status(ErrorCodes::BadValue, "$slice array wrong size");
                        }

                        BSONObjIterator it(arr);
                        int skip = it.next().numberInt();
                        int limit = it.next().numberInt();
                        if (limit <= 0) {
                            return Status(ErrorCodes::BadValue, "$slice limit must be positive");
                        }

                        add(e.fieldName(), skip, limit);
                    }
                    else {
                        return Status(ErrorCodes::BadValue,
                                      "$slice only supports numbers and [skip, limit] arrays");
                    }
                }
                else if (mongoutils::str::equals(e2.fieldName(), "$elemMatch")) {
                    // Validate $elemMatch arguments and dependencies.
                    if (Object != e2.type()) {
                        return Status(ErrorCodes::BadValue,
                                      "elemMatch: Invalid argument, object required.");
                    }

                    if (ARRAY_OP_POSITIONAL == _arrayOpType) {
                        return Status(ErrorCodes::BadValue,
                                      "Cannot specify positional operator and $elemMatch.");
                    }

                    if (mongoutils::str::contains(e.fieldName(), '.')) {
                        return Status(ErrorCodes::BadValue,
                                      "Cannot use $elemMatch projection on a nested field.");
                    }

                    _arrayOpType = ARRAY_OP_ELEM_MATCH;

                    // Create a MatchExpression for the elemMatch.
                    BSONObj elemMatchObj = e.wrap();
                    verify(elemMatchObj.isOwned());
                    _elemMatchObjs.push_back(elemMatchObj);
                    StatusWithMatchExpression swme = MatchExpressionParser::parse(elemMatchObj);
                    if (!swme.isOK()) {
                        return swme.getStatus();
                    }
                    // And store it in _matchers.
                    _matchers[mongoutils::str::before(e.fieldName(), '.').c_str()]
                        = swme.getValue();

                    add(e.fieldName(), true);
                }
                else if (mongoutils::str::equals(e2.fieldName(), "$textScore")) {
                    // TODO: Do we want to check this for :0 or :1 or just assume presence implies
                    // projection?
                    _textScoreFieldName = e.fieldName();
                }
                else {
                    return Status(ErrorCodes::BadValue,
                                  string("Unsupported projection option: ") + e.toString());
                }
            }
            else if (mongoutils::str::equals(e.fieldName(), "_id") && !e.trueValue()) {
                _includeID = false;
            }
            else {
                add(e.fieldName(), e.trueValue());

                // Projections of dotted fields aren't covered.
                if (mongoutils::str::contains(e.fieldName(), '.')) {
                    _hasDottedField = true;
                }

                // Validate input.
                if (include_exclude == -1) {
                    // If we haven't specified an include/exclude, initialize include_exclude.
                    // We expect further include/excludes to match it.
                    include_exclude = e.trueValue();
                    _include = !e.trueValue();
                }
                else if (static_cast<bool>(include_exclude) != e.trueValue()) {
                    // Make sure that the incl./excl. matches the previous.
                    return Status(ErrorCodes::BadValue,
                                  "Projection cannot have a mix of inclusion and exclusion.");
                }
            }

            if (mongoutils::str::contains(e.fieldName(), ".$")) {
                // Validate the positional op.
                if (!e.trueValue()) {
                    return Status(ErrorCodes::BadValue,
                                  "Cannot exclude array elements with the positional operator.");
                }

                if (ARRAY_OP_POSITIONAL == _arrayOpType) {
                    return Status(ErrorCodes::BadValue,
                                  "Cannot specify more than one positional proj. per query.");
                }

                if (ARRAY_OP_ELEM_MATCH == _arrayOpType) {
                    return Status(ErrorCodes::BadValue,
                                  "Cannot specify positional operator and $elemMatch.");
                }

                _arrayOpType = ARRAY_OP_POSITIONAL;
            }
        }

        if (ARRAY_OP_POSITIONAL != _arrayOpType) {
            return Status::OK();
        }

        // Validates positional operator ($) projections.

        // XXX: This is copied from how it was validated before.  It should probably walk the
        // expression tree...but we maintain this for now.  TODO: Remove this and/or make better.

        BSONObjIterator querySpecIter(query);
        while (querySpecIter.more()) {
            BSONElement queryElement = querySpecIter.next();
            if (mongoutils::str::equals(queryElement.fieldName(), "$and")) {
                // don't check $and to avoid deep comparison of the arguments.
                // TODO: can be replaced with Matcher::FieldSink when complete (SERVER-4644)
                return Status::OK();
            }

            BSONObjIterator projectionSpecIter(_source);
            while ( projectionSpecIter.more() ) {
                // for each projection element
                BSONElement projectionElement = projectionSpecIter.next();
                if ( mongoutils::str::contains( projectionElement.fieldName(), ".$" ) &&
                        mongoutils::str::before( queryElement.fieldName(), '.' ) ==
                        mongoutils::str::before( projectionElement.fieldName(), "." ) ) {
                    return Status::OK();
                }
            }
        }

        return Status(ErrorCodes::BadValue,
                      "Positional operator does not match the query specifier.");
    }

    // TODO: stringdata
    void LiteProjection::getRequiredFields(vector<string>* fields) const {
        if (_includeID) {
            fields->push_back("_id");
        }

        // The only way we could be here is if _source is only simple non-dotted-field projections.
        // Therefore we can iterate over _source to get the fields required.
        BSONObjIterator srcIt(_source);
        while (srcIt.more()) {
            BSONElement elt = srcIt.next();
            if (elt.trueValue()) {
                fields->push_back(elt.fieldName());
            }
        }
    }

    void LiteProjection::add(const string& field, bool include) {
        if (field.empty()) { // this is the field the user referred to
            _include = include;
        }
        else {
            // XXX document
            _include = !include;

            const size_t dot = field.find('.');
            const string subfield = field.substr(0,dot);
            const string rest = (dot == string::npos ? "" : field.substr(dot + 1, string::npos));

            LiteProjection*& fm = _fields[subfield.c_str()];

            if (NULL == fm) {
                fm = new LiteProjection();
            }

            fm->add(rest, include);
        }
    }

    void LiteProjection::add(const string& field, int skip, int limit) {
        _special = true; // can't include or exclude whole object

        if (field.empty()) { // this is the field the user referred to
            _skip = skip;
            _limit = limit;
        }
        else {
            const size_t dot = field.find('.');
            const string subfield = field.substr(0,dot);
            const string rest = (dot == string::npos ? "" : field.substr(dot + 1, string::npos));

            LiteProjection*& fm = _fields[subfield.c_str()];

            if (NULL == fm) {
                fm = new LiteProjection();
            }

            fm->add(rest, skip, limit);
        }
    }

    //
    // Execution
    //

    Status LiteProjection::transform(const BSONObj& in,
                                     BSONObjBuilder* bob,
                                     const MatchDetails* details) const {

        const ArrayOpType& arrayOpType = _arrayOpType;

        BSONObjIterator it(in);
        while (it.more()) {
            BSONElement elt = it.next();

            // Case 1: _id
            if (mongoutils::str::equals("_id", elt.fieldName())) {
                if (_includeID) {
                    bob->append(elt);
                }
                continue;
            }

            // Case 2: no array projection for this field.
            Matchers::const_iterator matcher = _matchers.find(elt.fieldName());
            if (_matchers.end() == matcher) {
                append(bob, elt, details, arrayOpType);
                continue;
            }

            // Case 3: field has array projection with $elemMatch specified.
            if (ARRAY_OP_ELEM_MATCH != arrayOpType) {
                return Status(ErrorCodes::BadValue,
                             "Matchers are only supported for $elemMatch");
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

    void LiteProjection::appendArray(BSONObjBuilder* bob, const BSONObj& array, bool nested) const {
        int skip  = nested ?  0 : _skip;
        int limit = nested ? -1 : _limit;

        if (skip < 0) {
            skip = max(0, skip + array.nFields());
        }

        int index = 0;
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

            switch(elt.type()) {
            case Array: {
                BSONObjBuilder subBob;
                appendArray(&subBob, elt.embeddedObject(), true);
                bob->appendArray(bob->numStr(index++), subBob.obj());
                break;
            }
            case Object: {
                BSONObjBuilder subBob;
                BSONObjIterator jt(elt.embeddedObject());
                while (jt.more()) {
                    append(&subBob, jt.next());
                }
                bob->append(bob->numStr(index++), subBob.obj());
                break;
            }
            default:
                if (_include) {
                    bob->appendAs(elt, bob->numStr(index++));
                }
            }
        }
    }

    Status LiteProjection::append(BSONObjBuilder* bob,
                                  const BSONElement& elt,
                                  const MatchDetails* details,
                                  const ArrayOpType arrayOpType) const {

        FieldMap::const_iterator field = _fields.find(elt.fieldName());
        if (field == _fields.end()) {
            if (_include) {
                bob->append(elt);
            }
            return Status::OK();
        }

        LiteProjection& subfm = *field->second;
        if ((subfm._fields.empty() && !subfm._special)
            || !(elt.type() == Object || elt.type() == Array)) {
            // field map empty, or element is not an array/object
            if (subfm._include) {
                bob->append(elt);
            }
        }
        else if (elt.type() == Object) {
            BSONObjBuilder subBob;
            BSONObjIterator it(elt.embeddedObject());
            while (it.more()) {
                subfm.append(&subBob, it.next(), details, arrayOpType);
            }
            bob->append(elt.fieldName(), subBob.obj());
        }
        else {
            // Array
            BSONObjBuilder matchedBuilder;
            if (details && arrayOpType == ARRAY_OP_POSITIONAL) {
                // $ positional operator specified
                if (!details->hasElemMatchKey()) {
                    stringstream error;
                    error << "positional operator (" << elt.fieldName()
                          << ".$) requires corresponding field"
                          << " in query specifier";
                    return Status(ErrorCodes::BadValue, error.str());
                }

                if (elt.embeddedObject()[details->elemMatchKey()].eoo()) {
                    return Status(ErrorCodes::BadValue,
                                  "positional operator element mismatch");
                }

                // append as the first and only element in the projected array
                matchedBuilder.appendAs( elt.embeddedObject()[details->elemMatchKey()], "0" );
            }
            else {
                // append exact array; no subarray matcher specified
                subfm.appendArray(&matchedBuilder, elt.embeddedObject());
            }
            bob->appendArray(elt.fieldName(), matchedBuilder.obj());
        }

        return Status::OK();
    }

}  // namespace mongo
