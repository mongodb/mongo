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

#include "mongo/db/ops/modifier_current_date.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/db/global_timestamp.h"
#include "mongo/db/ops/field_checker.h"
#include "mongo/db/ops/log_builder.h"
#include "mongo/db/ops/path_support.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace str = mongoutils::str;

namespace {
const char kType[] = "$type";
const char kDate[] = "date";
const char kTimestamp[] = "timestamp";
}

struct ModifierCurrentDate::PreparedState {
    PreparedState(mutablebson::Document& doc) : doc(doc), elemFound(doc.end()), idxFound(0) {}

    // Document that is going to be changed.
    mutablebson::Document& doc;

    // Element corresponding to _fieldRef[0.._idxFound].
    mutablebson::Element elemFound;

    // Index in _fieldRef for which an Element exist in the document.
    size_t idxFound;
};

ModifierCurrentDate::ModifierCurrentDate() : _pathReplacementPosition(0), _typeIsDate(true) {}

ModifierCurrentDate::~ModifierCurrentDate() {}

Status ModifierCurrentDate::init(const BSONElement& modExpr,
                                 const Options& opts,
                                 bool* positional) {
    _updatePath.parse(modExpr.fieldName());
    Status status = fieldchecker::isUpdatable(_updatePath);
    if (!status.isOK()) {
        return status;
    }

    // If a $-positional operator was used, get the index in which it occurred
    // and ensure only one occurrence.
    size_t foundCount;
    bool foundDollar =
        fieldchecker::isPositional(_updatePath, &_pathReplacementPosition, &foundCount);

    if (positional)
        *positional = foundDollar;

    if (foundDollar && foundCount > 1) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Too many positional (i.e. '$') elements found in path '"
                                    << _updatePath.dottedField()
                                    << "'");
    }

    // Validate and store the type to produce
    switch (modExpr.type()) {
        case Bool:
            _typeIsDate = true;
            break;
        case Object: {
            const BSONObj argObj = modExpr.embeddedObject();
            const BSONElement typeElem = argObj.getField(kType);
            bool badInput = typeElem.eoo() || !(typeElem.type() == String);

            if (!badInput) {
                std::string typeVal = typeElem.String();
                badInput = !(typeElem.String() == kDate || typeElem.String() == kTimestamp);
                if (!badInput)
                    _typeIsDate = (typeVal == kDate);

                if (!badInput) {
                    // Check to make sure only the $type field was given as an arg
                    BSONObjIterator i(argObj);
                    const bool onlyHasTypeField =
                        ((i.next().fieldNameStringData() == kType) && i.next().eoo());
                    if (!onlyHasTypeField) {
                        return Status(ErrorCodes::BadValue,
                                      str::stream()
                                          << "The only valid field of the option is '$type': "
                                             "{$currentDate: {field : {$type: 'date/timestamp'}}}; "
                                          << "arg: "
                                          << argObj);
                    }
                }
            }

            if (badInput) {
                return Status(ErrorCodes::BadValue,
                              "The '$type' string field is required "
                              "to be 'date' or 'timestamp': "
                              "{$currentDate: {field : {$type: 'date'}}}");
            }
            break;
        }
        default:
            return Status(ErrorCodes::BadValue,
                          str::stream() << typeName(modExpr.type())
                                        << " is not valid type for $currentDate."
                                           " Please use a boolean ('true')"
                                           " or a $type expression ({$type: 'timestamp/date'}).");
    }

    return Status::OK();
}

Status ModifierCurrentDate::prepare(mutablebson::Element root,
                                    StringData matchedField,
                                    ExecInfo* execInfo) {
    _preparedState.reset(new PreparedState(root.getDocument()));

    // If we have a $-positional field, it is time to bind it to an actual field part.
    if (_pathReplacementPosition) {
        if (matchedField.empty()) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "The positional operator did not find the match "
                                           "needed from the query. Unexpanded update: "
                                        << _updatePath.dottedField());
        }
        _updatePath.setPart(_pathReplacementPosition, matchedField);
    }

    // Locate the field name in 'root'. Note that we may not have all the parts in the path
    // in the doc -- which is fine. Our goal now is merely to reason about whether this mod
    // apply is a noOp or whether is can be in place. The remaining path, if missing, will
    // be created during the apply.
    Status status = pathsupport::findLongestPrefix(
        _updatePath, root, &_preparedState->idxFound, &_preparedState->elemFound);

    // FindLongestPrefix may say the path does not exist at all, which is fine here, or
    // that the path was not viable or otherwise wrong, in which case, the mod cannot
    // proceed.
    if (status.code() == ErrorCodes::NonExistentPath) {
        _preparedState->elemFound = root.getDocument().end();
    } else if (!status.isOK()) {
        return status;
    }

    // We register interest in the field name. The driver needs this info to sort out if
    // there is any conflict among mods.
    execInfo->fieldRef[0] = &_updatePath;

    return Status::OK();
}

Status ModifierCurrentDate::apply() const {
    const bool destExists = (_preparedState->elemFound.ok() &&
                             _preparedState->idxFound == (_updatePath.numParts() - 1));

    mutablebson::Document& doc = _preparedState->doc;
    StringData lastPart = _updatePath.getPart(_updatePath.numParts() - 1);
    // If the element exists and is the same type, then that is what we want to work with
    mutablebson::Element elemToSet = destExists ? _preparedState->elemFound : doc.end();

    if (!destExists) {
        // Creates the final element that's going to be $set in 'doc'.
        // fills in the value with place-holder/empty

        elemToSet = _typeIsDate ? doc.makeElementDate(lastPart, Date_t())
                                : doc.makeElementTimestamp(lastPart, Timestamp());

        if (!elemToSet.ok()) {
            return Status(ErrorCodes::InternalError, "can't create new element");
        }

        // Now, we can be in two cases here, as far as attaching the element being set goes:
        // (a) none of the parts in the element's path exist, or (b) some parts of the path
        // exist but not all.
        if (!_preparedState->elemFound.ok()) {
            _preparedState->elemFound = doc.root();
            _preparedState->idxFound = 0;
        } else {
            _preparedState->idxFound++;
        }

        // createPathAt() will complete the path and attach 'elemToSet' at the end of it.
        Status s = pathsupport::createPathAt(
            _updatePath, _preparedState->idxFound, _preparedState->elemFound, elemToSet);
        if (!s.isOK())
            return s;
    }

    dassert(elemToSet.ok());

    // By the time we are here the element is in place and we just need to update the value
    if (_typeIsDate) {
        const mongo::Date_t now = mongo::jsTime();
        Status s = elemToSet.setValueDate(now);
        if (!s.isOK())
            return s;
    } else {
        Status s = elemToSet.setValueTimestamp(getNextGlobalTimestamp());
        if (!s.isOK())
            return s;
    }

    // Set the elemFound, idxFound to the changed element for oplog logging.
    _preparedState->elemFound = elemToSet;
    _preparedState->idxFound = (_updatePath.numParts() - 1);

    return Status::OK();
}

Status ModifierCurrentDate::log(LogBuilder* logBuilder) const {
    // TODO: None of this checks should be needed unless someone calls if we are a noOp
    // When we cleanup we should build in testing that no-one calls in the noOp case
    const bool destExists = (_preparedState->elemFound.ok() &&
                             _preparedState->idxFound == (_updatePath.numParts() - 1));

    // If the destination doesn't exist then we have nothing to log.
    // This would only happen if apply isn't called or fails when it had to create an element
    if (!destExists)
        return Status::OK();

    return logBuilder->addToSetsWithNewFieldName(_updatePath.dottedField(),
                                                 _preparedState->elemFound);
}

}  // namespace mongo
