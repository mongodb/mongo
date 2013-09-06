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

#include "mongo/db/ops/modifier_push.h"

#include <algorithm>

#include "mongo/base/error_codes.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/db/ops/field_checker.h"
#include "mongo/db/ops/log_builder.h"
#include "mongo/db/ops/path_support.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    namespace {

        bool isPatternElement(const BSONElement& pattern) {
            if (!pattern.isNumber()) {
                return false;
            }

            // Patterns can be only 1 or -1.
            double val = pattern.Number();
            if (val != 1 && val != -1) {
                return false;
            }

            return true;
        }

        bool inEachMode(const BSONElement& modExpr) {
            if (modExpr.type() != Object) {
                return false;
            }
            BSONObj obj = modExpr.embeddedObject();
            if (obj["$each"].type() == EOO) {
                return false;
            }
            return true;
        }

        Status parseEachMode(ModifierPush::ModifierPushMode pushMode,
                             const BSONElement& modExpr,
                             BSONElement* eachElem,
                             BSONElement* sliceElem,
                             BSONElement* sortElem) {

            Status status = Status::OK();

            // If in $pushAll mode, all we need is the array.
            if (pushMode == ModifierPush::PUSH_ALL) {
                if (modExpr.type() != Array) {
                    return Status(ErrorCodes::BadValue, "$pushAll requires an array");
                }
                *eachElem = modExpr;
                return Status::OK();
            }

            // The $each clause must be an array.
            *eachElem = modExpr.embeddedObject()["$each"];
            if (eachElem->type() != Array) {
                return Status(ErrorCodes::BadValue, "$each must be an array");
            }

            // There must be only one $each clause.
            bool seenEach = false;
            BSONObjIterator itMod(modExpr.embeddedObject());
            while (itMod.more()) {
                BSONElement modElem = itMod.next();
                if (mongoutils::str::equals(modElem.fieldName(), "$each")) {
                    if (seenEach) {
                        return Status(ErrorCodes::BadValue, "duplicated $each clause");
                    }
                    seenEach = true;
                }
            }

            // Check if no invalid data (such as fields with '$'s) are being used in the $each
            // clause.
            BSONObjIterator itEach(eachElem->embeddedObject());
            while (itEach.more()) {
                BSONElement eachItem = itEach.next();
                if (eachItem.type() == Object || eachItem.type() == Array) {
                    if (! eachItem.Obj().okForStorage()) {
                        return Status(ErrorCodes::BadValue, "cannot use '$' in $each entries");
                    }
                }
            }

            // Slice and sort are optional and may be present in any order.
            bool seenSlice = false;
            bool seenSort = false;
            BSONObjIterator itPush(modExpr.embeddedObject());
            while (itPush.more()) {
                BSONElement elem = itPush.next();
                if (mongoutils::str::equals(elem.fieldName(), "$slice")) {
                    if (seenSlice) {
                        return Status(ErrorCodes::BadValue, "duplicate $slice clause");
                    }
                    *sliceElem = elem;
                    seenSlice = true;
                }
                else if (mongoutils::str::equals(elem.fieldName(), "$sort")) {
                    if (seenSort) {
                        return Status(ErrorCodes::BadValue, "duplicate $sort clause");
                    }
                    *sortElem = elem;
                    seenSort = true;
                }
                else if (!mongoutils::str::equals(elem.fieldName(), "$each")) {
                    return Status(ErrorCodes::BadValue, "unrecognized clause is $push");
                }
            }

            if (seenSort) {
                BSONObjIterator itEach(eachElem->embeddedObject());
                while (itEach.more()) {
                    BSONElement eachItem = itEach.next();
                    if (eachItem.type() != Object) {
                        return Status(
                            ErrorCodes::BadValue,
                            "$push like modifiers using $sort require all elements to be objects");
                    }
                }
            }

            return Status::OK();
        }

    } // unnamed namespace

    struct ModifierPush::PreparedState {

        PreparedState(mutablebson::Document* targetDoc)
            : doc(*targetDoc)
            , idxFound(0)
            , elemFound(doc.end())
            , boundDollar("") {
        }

        // Document that is going to be changed.
        mutablebson::Document& doc;

        // Index in _fieldRef for which an Element exist in the document.
        size_t idxFound;

        // Element corresponding to _fieldRef[0.._idxFound].
        mutablebson::Element elemFound;

        // Value to bind to a $-positional field, if one is provided.
        std::string boundDollar;

    };

    ModifierPush::ModifierPush(ModifierPush::ModifierPushMode pushMode)
        : _fieldRef()
        , _posDollar(0)
        , _eachMode(false)
        , _eachElem()
        , _slicePresent(false)
        , _slice(0)
        , _sortPresent(false)
        , _sort()
        , _pushMode(pushMode)
        , _val() {
    }

    ModifierPush::~ModifierPush() {
    }

    Status ModifierPush::init(const BSONElement& modExpr, const Options& opts) {

        //
        // field name analysis
        //

        // Break down the field name into its 'dotted' components (aka parts) and check that
        // the field is fit for updates.
        _fieldRef.parse(modExpr.fieldName());
        Status status = fieldchecker::isUpdatable(_fieldRef);
        if (! status.isOK()) {
            return status;
        }

        // If a $-positional operator was used, get the index in which it occurred
        // and ensure only one occurrence.
        size_t foundCount;
        bool foundDollar = fieldchecker::isPositional(_fieldRef, &_posDollar, &foundCount);
        if (foundDollar && foundCount > 1) {
            return Status(ErrorCodes::BadValue, "too many positional($) elements found.");
        }

        //
        // value analysis
        //

        // Are the target push values safe to store?
        BSONElement sliceElem;
        BSONElement sortElem;
        switch (modExpr.type()) {

        case Array:
            if (! modExpr.Obj().okForStorage()) {
                return Status(ErrorCodes::BadValue, "cannot use '$' or '.' as values");
            }

            if (_pushMode == PUSH_ALL) {
                _eachMode = true;
                Status status = parseEachMode(PUSH_ALL,
                                              modExpr,
                                              &_eachElem,
                                              &sliceElem,
                                              &sortElem);
                if (!status.isOK()) {
                    return status;
                }
            }
            else {
                _val = modExpr;
            }
            break;

        case Object:
            if (_pushMode == PUSH_ALL) {
                return Status(ErrorCodes::BadValue, "$pushAll requires an array of values");
            }

            // If any known clause ($each, $slice, or $sort) is present, we'd assume
            // we're using the $each variation of push and would parse accodingly.
            _eachMode = inEachMode(modExpr);
            if (_eachMode) {
                Status status = parseEachMode(PUSH_NORMAL,
                                              modExpr,
                                              &_eachElem,
                                              &sliceElem,
                                              &sortElem);
                if (!status.isOK()) {
                    return status;
                }
            }
            else {
                if (! modExpr.Obj().okForStorage()) {
                    return Status(ErrorCodes::BadValue, "cannot use '$' as values");
                }
                _val = modExpr;
            }
            break;

        default:
            if (_pushMode == PUSH_ALL) {
                return Status(ErrorCodes::BadValue, "$pushAll requires an array of values");
            }

            _val = modExpr;
            break;
        }

        // Is slice present and correct?
        if (sliceElem.type() != EOO) {
            if (_pushMode == PUSH_ALL) {
                return Status(ErrorCodes::BadValue, "cannot use $slice in $pushAll");
            }

            if (!sliceElem.isNumber()) {
                return Status(ErrorCodes::BadValue, "$slice must be a numeric value");
            }

            // If the value of slice is not fraction, even if it's a double, we allow it. The
            // reason here is that the shell will use doubles by default unless told otherwise.
            double fractional = sliceElem.numberDouble();
            if (fractional - static_cast<int64_t>(fractional) != 0) {
                return Status(ErrorCodes::BadValue, "$slice in $push cannot be fractional");
            }

            _slice = sliceElem.numberLong();
            if (_slice > 0) {
                return Status(ErrorCodes::BadValue, "$slice in $push must be zero or negative");
            }
            _slicePresent = true;
        }

        // Is sort present and correct?
        if (sortElem.type() != EOO) {
            if (_pushMode == PUSH_ALL) {
                return Status(ErrorCodes::BadValue, "cannot use $sort in $pushAll");
            }

            if (!_slicePresent) {
                return Status(ErrorCodes::BadValue, "$sort requires $slice to be present");
            }
            else if (sortElem.type() != Object) {
                return Status(ErrorCodes::BadValue, "invalid $sort clause");
            }

            BSONObj sortObj = sortElem.embeddedObject();
            if (sortObj.isEmpty()) {
                return Status(ErrorCodes::BadValue, "sort parttern is empty");
            }

            // Check if the sort pattern is sound.
            BSONObjIterator sortIter(sortObj);
            while (sortIter.more()) {

                BSONElement sortPatternElem = sortIter.next();

                // We require either <field>: 1 or -1 for asc and desc.
                if (!isPatternElement(sortPatternElem)) {
                    return Status(ErrorCodes::BadValue, "$sort elements' must be either 1 or -1");
                }

                // All fields parts must be valid.
                FieldRef sortField;
                sortField.parse(sortPatternElem.fieldName());
                if (sortField.numParts() == 0) {
                    return Status(ErrorCodes::BadValue, "$sort field cannot be empty");
                }

                for (size_t i = 0; i < sortField.numParts(); i++) {
                    if (sortField.getPart(i).size() == 0) {
                        return Status(ErrorCodes::BadValue, "empty field in dotted sort pattern");
                    }
                }
            }

            _sort = PatternElementCmp(sortElem.embeddedObject());
            _sortPresent = true;
        }

        return Status::OK();
    }

    Status ModifierPush::prepare(mutablebson::Element root,
                                 const StringData& matchedField,
                                 ExecInfo* execInfo) {

        _preparedState.reset(new PreparedState(&root.getDocument()));

        // If we have a $-positional field, it is time to bind it to an actual field part.
        if (_posDollar) {
            if (matchedField.empty()) {
                return Status(ErrorCodes::BadValue, "matched field not provided");
            }
            _preparedState->boundDollar = matchedField.toString();
            _fieldRef.setPart(_posDollar, _preparedState->boundDollar);
        }

        // Locate the field name in 'root'. Note that we may not have all the parts in the path
        // in the doc -- which is fine. Our goal now is merely to reason about whether this mod
        // apply is a noOp or whether is can be in place. The remainin path, if missing, will
        // be created during the apply.
        Status status = pathsupport::findLongestPrefix(_fieldRef,
                                                       root,
                                                       &_preparedState->idxFound,
                                                       &_preparedState->elemFound);

        // FindLongestPrefix may say the path does not exist at all, which is fine here, or
        // that the path was not viable or otherwise wrong, in which case, the mod cannot
        // proceed.
        if (status.code() == ErrorCodes::NonExistentPath) {
            _preparedState->elemFound = root.getDocument().end();

        }
        else if (status.isOK()) {

            const bool destExists = (_preparedState->idxFound == (_fieldRef.numParts()-1));
            // If the path exists, we require the target field to be already an
            // array.
            if (destExists && _preparedState->elemFound.getType() != Array) {
                return Status(ErrorCodes::BadValue, "can only $push into arrays");
            }

            // If the $sort clause is being used, we require all the items in the array to be
            // objects themselves (as opposed to base types). This is a temporary restriction
            // that can be lifted once we support full sort semantics in $push.
            if (_sortPresent && destExists) {
                mutablebson::Element curr = _preparedState->elemFound.leftChild();
                while (curr.ok()) {
                    if (curr.getType() != Object) {
                        return Status(ErrorCodes::BadValue,
                                      "$push with sort requires object arrays");
                    }
                    curr = curr.rightSibling();
                }
            }

        }
        else {
            return status;
        }

        // We register interest in the field name. The driver needs this info to sort out if
        // there is any conflict among mods.
        execInfo->fieldRef[0] = &_fieldRef;

        return Status::OK();
    }

    Status ModifierPush::apply() const {

        Status status = Status::OK();

        //
        // Applying a $push with an $clause has the following steps
        // 1. Create the doc array we'll push into, if it is not there
        // 2. Add the items in the $each array (or the simple $push) to the doc array
        // 3. Sort the resulting array according to $sort clause, if present
        // 4. Trim the resulting array according the $slice clasue, if present
        //
        // TODO There are _lots_ of optimization opportunities that we'll consider once the
        // test coverage is adequate.
        //

        // 1. If the array field is not there, create it as an array and attach it to the
        // document.
        if (!_preparedState->elemFound.ok() ||
            _preparedState->idxFound < (_fieldRef.numParts()-1)) {

            // Creates the array element
            mutablebson::Document& doc = _preparedState->doc;
            StringData lastPart = _fieldRef.getPart(_fieldRef.numParts()-1);
            mutablebson::Element baseArray = doc.makeElementArray(lastPart);
            if (!baseArray.ok()) {
                return Status(ErrorCodes::InternalError, "can't create new base array");
            }

            // Now, we can be in two cases here, as far as attaching the element being set
            // goes: (a) none of the parts in the element's path exist, or (b) some parts of
            // the path exist but not all.
            if (!_preparedState->elemFound.ok()) {
                _preparedState->elemFound = doc.root();
                _preparedState->idxFound = 0;
            }
            else {
                _preparedState->idxFound++;
            }

            // createPathAt() will complete the path and attach 'elemToSet' at the end of it.
            status =  pathsupport::createPathAt(_fieldRef,
                                                _preparedState->idxFound,
                                                _preparedState->elemFound,
                                                baseArray);
            if (!status.isOK()) {
                return status;
            }

            // Point to the base array just created. The subsequent code expects it to exist
            // already.
            _preparedState->elemFound = baseArray;

        }

        // 2. Concatenate the two arrays together, either by going over the $each array or by
        // appending the (old style $push) element. Note that if we're the latter case, we
        // won't need to proceed to the $sort and $slice phases of the apply.
        if (_eachMode || _pushMode == PUSH_ALL) {
            BSONObjIterator itEach(_eachElem.embeddedObject());
            while (itEach.more()) {
                BSONElement eachItem = itEach.next();
                status = _preparedState->elemFound.appendElement(eachItem);
                if (!status.isOK()) {
                    return status;
                }
            }
        }
        else {
            mutablebson::Element elem =
                _preparedState->doc.makeElementWithNewFieldName(StringData(), _val);
            if (!elem.ok()) {
                return Status(ErrorCodes::InternalError, "can't wrap element being $push-ed");
            }
            return  _preparedState->elemFound.pushBack(elem);
        }

        // 3. Sort the resulting array, if $sort was requested.
        if (_sortPresent) {
            sortChildren(_preparedState->elemFound, _sort);
        }

        // 4. Trim the resulting array according to $slice, if present. We are assuming here
        // that slices are negative. When we implement both sides slicing, this needs changing.
        if (_slicePresent) {

            int64_t numChildren = mutablebson::countChildren(_preparedState->elemFound);
            int64_t countRemoved = std::max(static_cast<int64_t>(0), numChildren + _slice);

            mutablebson::Element curr = _preparedState->elemFound.leftChild();
            while (curr.ok() && countRemoved > 0) {
                mutablebson::Element toRemove = curr;
                curr = curr.rightSibling();

                status = toRemove.remove();
                if (!status.isOK()) {
                    return status;
                }
                countRemoved--;
            }
        }

        return status;
    }

    Status ModifierPush::log(LogBuilder* logBuilder) const {
        // TODO We can log just a positional set in several cases. For now, let's just log the
        // full resulting array.

        // We'd like to create an entry such as {$set: {<fieldname>: [<resulting aray>]}} under
        // 'logRoot'.  We start by creating the {$set: ...} Element.
        mutablebson::Document& doc = logBuilder->getDocument();

        // value for the logElement ("field.path.name": <value>)
        mutablebson::Element logElement = doc.makeElementWithNewFieldName(
            _fieldRef.dottedField(),
            _preparedState->elemFound);

        if (!logElement.ok()) {
            return Status(ErrorCodes::InternalError, "cannot create details for $push mod");
        }

        return logBuilder->addToSets(logElement);
    }

} // namespace mongo
