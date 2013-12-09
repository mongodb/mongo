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

#include "mongo/db/exec/projection_exec.h"

#include "mongo/db/exec/working_set_computed_data.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    ProjectionExec::ProjectionExec()
        : _include(true),
          _special(false),
          _includeID(true),
          _skip(0),
          _limit(-1),
          _arrayOpType(ARRAY_OP_NORMAL),
          _hasNonSimple(false),
          _hasDottedField(false),
          _queryExpression(NULL),
          _hasReturnKey(false) { }


    ProjectionExec::ProjectionExec(const BSONObj& spec, const MatchExpression* queryExpression)
        : _include(true),
          _special(false),
          _source(spec),
          _includeID(true),
          _skip(0),
          _limit(-1),
          _arrayOpType(ARRAY_OP_NORMAL),
          _hasNonSimple(false),
          _hasDottedField(false),
          _queryExpression(queryExpression),
          _hasReturnKey(false) {

        // Are we including or excluding fields?
        // -1 when we haven't initialized it.
        // 1 when we're including
        // 0 when we're excluding.
        int include_exclude = -1;

        BSONObjIterator it(_source);
        while (it.more()) {
            BSONElement e = it.next();

            if (!e.isNumber() && !e.isBoolean()) {
                _hasNonSimple = true;
            }

            if (Object == e.type()) {
                BSONObj obj = e.embeddedObject();
                verify(1 == obj.nFields());

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
                    else {
                        verify(e2.type() == Array);
                        BSONObj arr = e2.embeddedObject();
                        verify(2 == arr.nFields());

                        BSONObjIterator it(arr);
                        int skip = it.next().numberInt();
                        int limit = it.next().numberInt();

                        verify(limit > 0);

                        add(e.fieldName(), skip, limit);
                    }
                }
                else if (mongoutils::str::equals(e2.fieldName(), "$elemMatch")) {
                    _arrayOpType = ARRAY_OP_ELEM_MATCH;

                    // Create a MatchExpression for the elemMatch.
                    BSONObj elemMatchObj = e.wrap();
                    verify(elemMatchObj.isOwned());
                    _elemMatchObjs.push_back(elemMatchObj);
                    StatusWithMatchExpression swme = MatchExpressionParser::parse(elemMatchObj);
                    verify(swme.isOK());
                    // And store it in _matchers.
                    _matchers[mongoutils::str::before(e.fieldName(), '.').c_str()]
                        = swme.getValue();

                    add(e.fieldName(), true);
                }
                else if (mongoutils::str::equals(e2.fieldName(), "$meta")) {
                    verify(String == e2.type());
                    if (mongoutils::str::equals(e2.valuestr(), "text")) {
                        _meta[e.fieldName()] = META_TEXT;
                    }
                    else if (mongoutils::str::equals(e2.valuestr(), "diskloc")) {
                        _meta[e.fieldName()] = META_DISKLOC;
                    }
                    else if (mongoutils::str::equals(e2.valuestr(), "indexKey")) {
                        _hasReturnKey = true;
                        // The index key clobbers everything so just stop parsing here.
                        return;
                    }
                    else {
                        // This shouldn't happen, should be caught by parsing.
                        verify(0);
                    }
                }
                else {
                    verify(0);
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
            }

            if (mongoutils::str::contains(e.fieldName(), ".$")) {
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
        if (field.empty()) { // this is the field the user referred to
            _include = include;
        }
        else {
            // XXX document
            _include = !include;

            const size_t dot = field.find('.');
            const string subfield = field.substr(0,dot);
            const string rest = (dot == string::npos ? "" : field.substr(dot + 1, string::npos));

            ProjectionExec*& fm = _fields[subfield.c_str()];

            if (NULL == fm) {
                fm = new ProjectionExec();
            }

            fm->add(rest, include);
        }
    }

    void ProjectionExec::add(const string& field, int skip, int limit) {
        _special = true; // can't include or exclude whole object

        if (field.empty()) { // this is the field the user referred to
            _skip = skip;
            _limit = limit;
        }
        else {
            const size_t dot = field.find('.');
            const string subfield = field.substr(0,dot);
            const string rest = (dot == string::npos ? "" : field.substr(dot + 1, string::npos));

            ProjectionExec*& fm = _fields[subfield.c_str()];

            if (NULL == fm) {
                fm = new ProjectionExec();
            }

            fm->add(rest, skip, limit);
        }
    }

    //
    // Execution
    //

    Status ProjectionExec::transform(WorkingSetMember* member) const {
        if (_hasReturnKey) {
            BSONObj keyObj;

            if (member->hasComputed(WSM_INDEX_KEY)) {
                const IndexKeyComputedData* key
                    = static_cast<const IndexKeyComputedData*>(member->getComputed(WSM_INDEX_KEY));
                keyObj = key->getKey();
            }

            member->state = WorkingSetMember::OWNED_OBJ;
            member->obj = keyObj;
            member->keyData.clear();
            member->loc = DiskLoc();
            return Status::OK();
        }

        BSONObjBuilder bob;
        if (!requiresDocument()) {
            // Go field by field.
            if (_includeID) {
                BSONElement elt;
                // Sometimes the _id field doesn't exist...
                if (member->getFieldDotted("_id", &elt) && !elt.eoo()) {
                    bob.appendAs(elt, "_id");
                }
            }

            BSONObjIterator it(_source);
            while (it.more()) {
                BSONElement specElt = it.next();
                if (mongoutils::str::equals("_id", specElt.fieldName())) {
                    continue;
                }

                BSONElement keyElt;
                // We can project a field that doesn't exist.  We just ignore it.
                if (member->getFieldDotted(specElt.fieldName(), &keyElt) && !keyElt.eoo()) {
                    bob.appendAs(keyElt, specElt.fieldName());
                }
            }
        }
        else {
            // Planner should have done this.
            verify(member->hasObj());

            MatchDetails matchDetails;

            // If it's a positional projection we need a MatchDetails.
            if (transformRequiresDetails()) {
                matchDetails.requestElemMatchKey();
                verify(NULL != _queryExpression);
                verify(_queryExpression->matchesBSON(member->obj, &matchDetails));
            }

            Status projStatus = transform(member->obj, &bob, &matchDetails);
            if (!projStatus.isOK()) {
                return projStatus;
            }
        }

        for (MetaMap::const_iterator it = _meta.begin(); it != _meta.end(); ++it) {
            if (META_TEXT == it->second) {
                if (member->hasComputed(WSM_COMPUTED_TEXT_SCORE)) {
                    const TextScoreComputedData* score
                        = static_cast<const TextScoreComputedData*>(
                                member->getComputed(WSM_COMPUTED_TEXT_SCORE));
                    bob.append(it->first, score->getScore());
                }
                else {
                    bob.append(it->first, 0.0);
                }
            }
            else if (META_DISKLOC == it->second) {
                bob.append(it->first, member->loc.toBSONObj());
            }
        }

        BSONObj newObj = bob.obj();
        member->state = WorkingSetMember::OWNED_OBJ;
        member->obj = newObj;
        member->keyData.clear();
        member->loc = DiskLoc();

        return Status::OK();
    }

    Status ProjectionExec::transform(const BSONObj& in, BSONObj* out) const {
        BSONObjBuilder bob;
        Status s = transform(in, &bob, NULL);
        if (!s.isOK()) {
            return s;
        }
        *out = bob.obj();
        return Status::OK();
    }

    Status ProjectionExec::transform(const BSONObj& in,
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
                Status s = append(bob, elt, details, arrayOpType);
                if (!s.isOK()) {
                    return s;
                }
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

    void ProjectionExec::appendArray(BSONObjBuilder* bob, const BSONObj& array, bool nested) const {
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

    Status ProjectionExec::append(BSONObjBuilder* bob,
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

        ProjectionExec& subfm = *field->second;
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
