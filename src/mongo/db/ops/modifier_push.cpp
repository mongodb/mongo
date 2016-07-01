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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/db/ops/modifier_push.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "mongo/base/error_codes.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/db/ops/field_checker.h"
#include "mongo/db/ops/log_builder.h"
#include "mongo/db/ops/path_support.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::abs;
using std::numeric_limits;

namespace mb = mutablebson;
namespace str = mongoutils::str;

namespace {

const char kEach[] = "$each";
const char kSlice[] = "$slice";
const char kSort[] = "$sort";
const char kPosition[] = "$position";

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
    if (obj[kEach].type() == EOO) {
        return false;
    }
    return true;
}

Status parseEachMode(ModifierPush::ModifierPushMode pushMode,
                     const BSONElement& modExpr,
                     BSONElement* eachElem,
                     BSONElement* sliceElem,
                     BSONElement* sortElem,
                     BSONElement* positionElem) {
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
    *eachElem = modExpr.embeddedObject()[kEach];
    if (eachElem->type() != Array) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "The argument to $each in $push must be"
                                       " an array but it was of type: "
                                    << typeName(eachElem->type()));
    }

    // There must be only one $each clause.
    bool seenEach = false;
    BSONObjIterator itMod(modExpr.embeddedObject());
    while (itMod.more()) {
        BSONElement modElem = itMod.next();
        if (mongoutils::str::equals(modElem.fieldName(), kEach)) {
            if (seenEach) {
                return Status(ErrorCodes::BadValue, "Only one $each clause is supported.");
            }
            seenEach = true;
        }
    }

    // Slice, sort, position are optional and may be present in any order.
    bool seenSlice = false;
    bool seenSort = false;
    bool seenPosition = false;
    BSONObjIterator itPush(modExpr.embeddedObject());
    while (itPush.more()) {
        BSONElement elem = itPush.next();
        if (mongoutils::str::equals(elem.fieldName(), kSlice)) {
            if (seenSlice) {
                return Status(ErrorCodes::BadValue, "Only one $slice clause is supported.");
            }
            *sliceElem = elem;
            seenSlice = true;
        } else if (mongoutils::str::equals(elem.fieldName(), kSort)) {
            if (seenSort) {
                return Status(ErrorCodes::BadValue, "Only one $sort clause is supported.");
            }
            *sortElem = elem;
            seenSort = true;
        } else if (mongoutils::str::equals(elem.fieldName(), kPosition)) {
            if (seenPosition) {
                return Status(ErrorCodes::BadValue, "Only one $position clause is supported.");
            }
            *positionElem = elem;
            seenPosition = true;
        } else if (!mongoutils::str::equals(elem.fieldName(), kEach)) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "Unrecognized clause in $push: "
                                        << elem.fieldNameStringData());
        }
    }

    return Status::OK();
}

}  // unnamed namespace

struct ModifierPush::PreparedState {
    PreparedState(mutablebson::Document* targetDoc)
        : doc(*targetDoc), idxFound(0), elemFound(doc.end()), arrayPreModSize(0) {}

    // Document that is going to be changed.
    mutablebson::Document& doc;

    // Index in _fieldRef for which an Element exist in the document.
    size_t idxFound;

    // Element corresponding to _fieldRef[0.._idxFound].
    mutablebson::Element elemFound;

    size_t arrayPreModSize;
};

ModifierPush::ModifierPush(ModifierPush::ModifierPushMode pushMode)
    : _fieldRef(),
      _posDollar(0),
      _eachMode(false),
      _eachElem(),
      _slicePresent(false),
      _slice(0),
      _sortPresent(false),
      _startPosition(std::numeric_limits<std::size_t>::max()),
      _sort(),
      _pushMode(pushMode),
      _val() {}

ModifierPush::~ModifierPush() {}

Status ModifierPush::init(const BSONElement& modExpr, const Options& opts, bool* positional) {
    //
    // field name analysis
    //

    // Break down the field name into its 'dotted' components (aka parts) and check that
    // the field is fit for updates.
    _fieldRef.parse(modExpr.fieldName());
    Status status = fieldchecker::isUpdatable(_fieldRef);
    if (!status.isOK()) {
        return status;
    }

    // If a $-positional operator was used, get the index in which it occurred
    // and ensure only one occurrence.
    size_t foundCount;
    bool foundDollar = fieldchecker::isPositional(_fieldRef, &_posDollar, &foundCount);

    if (positional)
        *positional = foundDollar;

    if (foundDollar && foundCount > 1) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Too many positional (i.e. '$') elements found in path '"
                                    << _fieldRef.dottedField()
                                    << "'");
    }

    //
    // value analysis
    //

    // Are the target push values safe to store?
    BSONElement sliceElem;
    BSONElement sortElem;
    BSONElement positionElem;
    switch (modExpr.type()) {
        case Array:
            if (_pushMode == PUSH_ALL) {
                _eachMode = true;
                Status status = parseEachMode(
                    PUSH_ALL, modExpr, &_eachElem, &sliceElem, &sortElem, &positionElem);
                if (!status.isOK()) {
                    return status;
                }
            } else {
                _val = modExpr;
            }
            break;

        case Object:
            if (_pushMode == PUSH_ALL) {
                return Status(ErrorCodes::BadValue,
                              str::stream() << "$pushAll requires an array of values "
                                               "but was given an embedded document.");
            }

            // If any known clause ($each, $slice, or $sort) is present, we'd assume
            // we're using the $each variation of push and would parse accodingly.
            _eachMode = inEachMode(modExpr);
            if (_eachMode) {
                Status status = parseEachMode(
                    PUSH_NORMAL, modExpr, &_eachElem, &sliceElem, &sortElem, &positionElem);
                if (!status.isOK()) {
                    return status;
                }
            } else {
                _val = modExpr;
            }
            break;

        default:
            if (_pushMode == PUSH_ALL) {
                return Status(ErrorCodes::BadValue,
                              str::stream() << "$pushAll requires an array of values "
                                               "but was given type: "
                                            << typeName(modExpr.type()));
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
            return Status(ErrorCodes::BadValue,
                          str::stream() << "The value for $slice must "
                                           "be a numeric value but was given type: "
                                        << typeName(sliceElem.type()));
        }

        // TODO: Cleanup and unify numbers wrt getting int32/64 bson values (from doubles)

        // If the value of slice is not fraction, even if it's a double, we allow it. The
        // reason here is that the shell will use doubles by default unless told otherwise.
        const double doubleVal = sliceElem.numberDouble();
        if (doubleVal - static_cast<int64_t>(doubleVal) != 0) {
            return Status(ErrorCodes::BadValue, "The $slice value in $push cannot be fractional");
        }

        _slice = sliceElem.numberLong();
        _slicePresent = true;
    }

    // Is position present and correct?
    if (positionElem.type() != EOO) {
        if (_pushMode == PUSH_ALL) {
            return Status(ErrorCodes::BadValue, "cannot use $position in $pushAll");
        }

        // Check that $position can be represented by a 32-bit integer.
        switch (positionElem.type()) {
            case NumberInt:
                break;
            case NumberLong:
                if (positionElem.numberInt() != positionElem.numberLong()) {
                    return Status(
                        ErrorCodes::BadValue,
                        "The $position value in $push must be representable as a 32-bit integer.");
                }
                break;
            case NumberDouble: {
                const auto doubleVal = positionElem.numberDouble();
                if (doubleVal != 0.0) {
                    if (!std::isnormal(doubleVal) || (doubleVal != positionElem.numberInt())) {
                        return Status(ErrorCodes::BadValue,
                                      "The $position value in $push must be representable as a "
                                      "32-bit integer.");
                    }
                }
                break;
            }
            default:
                return Status(ErrorCodes::BadValue,
                              str::stream() << "The value for $position must "
                                               "be a non-negative numeric value, not of type: "
                                            << typeName(positionElem.type()));
        }

        if (positionElem.numberInt() < 0) {
            return {
                Status(ErrorCodes::BadValue, "The $position value in $push must be non-negative.")};
        }

        _startPosition = size_t(positionElem.numberInt());
    }

    // Is sort present and correct?
    if (sortElem.type() != EOO) {
        if (_pushMode == PUSH_ALL) {
            return Status(ErrorCodes::BadValue, "cannot use $sort in $pushAll");
        }

        if (sortElem.type() != Object && !sortElem.isNumber()) {
            return Status(ErrorCodes::BadValue,
                          "The $sort is invalid: use 1/-1 to sort the whole element, "
                          "or {field:1/-1} to sort embedded fields");
        }

        if (sortElem.isABSONObj()) {
            BSONObj sortObj = sortElem.embeddedObject();
            if (sortObj.isEmpty()) {
                return Status(ErrorCodes::BadValue,
                              "The $sort pattern is empty when it should be a set of fields.");
            }

            // Check if the sort pattern is sound.
            BSONObjIterator sortIter(sortObj);
            while (sortIter.more()) {
                BSONElement sortPatternElem = sortIter.next();

                // We require either <field>: 1 or -1 for asc and desc.
                if (!isPatternElement(sortPatternElem)) {
                    return Status(ErrorCodes::BadValue,
                                  "The $sort element value must be either 1 or -1");
                }

                // All fields parts must be valid.
                FieldRef sortField(sortPatternElem.fieldName());
                if (sortField.numParts() == 0) {
                    return Status(ErrorCodes::BadValue, "The $sort field cannot be empty");
                }

                for (size_t i = 0; i < sortField.numParts(); i++) {
                    if (sortField.getPart(i).size() == 0) {
                        return Status(ErrorCodes::BadValue,
                                      str::stream() << "The $sort field is a dotted field "
                                                       "but has an empty part: "
                                                    << sortField.dottedField());
                    }
                }
            }

            _sort = PatternElementCmp(sortElem.embeddedObject(), opts.collator);
        } else {
            // Ensure the sortElem number is valid.
            if (!isPatternElement(sortElem)) {
                return Status(ErrorCodes::BadValue,
                              "The $sort element value must be either 1 or -1");
            }

            _sort = PatternElementCmp(BSON("" << sortElem.number()), opts.collator);
        }

        _sortPresent = true;
    }

    return Status::OK();
}

void ModifierPush::setCollator(const CollatorInterface* collator) {
    invariant(!_sort.collator);
    if (_sortPresent) {
        _sort.collator = collator;
    }
}

Status ModifierPush::prepare(mutablebson::Element root,
                             StringData matchedField,
                             ExecInfo* execInfo) {
    _preparedState.reset(new PreparedState(&root.getDocument()));

    // If we have a $-positional field, it is time to bind it to an actual field part.
    if (_posDollar) {
        if (matchedField.empty()) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "The positional operator did not find the match "
                                           "needed from the query. Unexpanded update: "
                                        << _fieldRef.dottedField());
        }
        _fieldRef.setPart(_posDollar, matchedField);
    }

    // Locate the field name in 'root'. Note that we may not have all the parts in the path
    // in the doc -- which is fine. Our goal now is merely to reason about whether this mod
    // apply is a noOp or whether is can be in place. The remainin path, if missing, will
    // be created during the apply.
    Status status = pathsupport::findLongestPrefix(
        _fieldRef, root, &_preparedState->idxFound, &_preparedState->elemFound);

    // FindLongestPrefix may say the path does not exist at all, which is fine here, or
    // that the path was not viable or otherwise wrong, in which case, the mod cannot
    // proceed.
    if (status.code() == ErrorCodes::NonExistentPath) {
        _preparedState->elemFound = root.getDocument().end();

    } else if (status.isOK()) {
        const bool destExists = (_preparedState->idxFound == (_fieldRef.numParts() - 1));
        // If the path exists, we require the target field to be already an
        // array.
        if (destExists && _preparedState->elemFound.getType() != Array) {
            mb::Element idElem = mb::findFirstChildNamed(root, "_id");
            return Status(ErrorCodes::BadValue,
                          str::stream() << "The field '" << _fieldRef.dottedField() << "'"
                                        << " must be an array but is of type "
                                        << typeName(_preparedState->elemFound.getType())
                                        << " in document {"
                                        << idElem.toString()
                                        << "}");
        }
    } else {
        return status;
    }

    // We register interest in the field name. The driver needs this info to sort out if
    // there is any conflict among mods.
    execInfo->fieldRef[0] = &_fieldRef;

    return Status::OK();
}

namespace {
Status pushFirstElement(mb::Element& arrayElem,
                        const size_t arraySize,
                        const size_t pos,
                        mb::Element& elem) {
    // Empty array or pushing to the front
    if (arraySize == 0 || pos == 0) {
        return arrayElem.pushFront(elem);
    } else {
        // Push position is at the end, or beyond
        if (pos >= arraySize) {
            return arrayElem.pushBack(elem);
        }

        const size_t appendPos = pos - 1;
        mutablebson::Element fromElem = getNthChild(arrayElem, appendPos);

        // This should not be possible since the checks above should
        // cover us but error just in case
        if (!fromElem.ok()) {
            return Status(ErrorCodes::InvalidLength,
                          str::stream() << "The specified position (" << appendPos << "/" << pos
                                        << ") is invalid based on the length ( "
                                        << arraySize
                                        << ") of the array");
        }

        return fromElem.addSiblingRight(elem);
    }
}
}  // unamed namespace

Status ModifierPush::apply() const {
    Status status = Status::OK();

    //
    // Applying a $push with an $clause has the following steps
    // 1. Create the doc array we'll push into, if it is not there
    // 2. Add the items in the $each array (or the simple $push) to the doc array
    // 3. Sort the resulting array according to $sort clause, if present
    // 4. Trim the resulting array according the $slice clause, if present
    //
    // TODO There are _lots_ of optimization opportunities that we'll consider once the
    // test coverage is adequate.
    //

    // 1. If the array field is not there, create it as an array and attach it to the
    // document.
    if (!_preparedState->elemFound.ok() || _preparedState->idxFound < (_fieldRef.numParts() - 1)) {
        // Creates the array element
        mutablebson::Document& doc = _preparedState->doc;
        StringData lastPart = _fieldRef.getPart(_fieldRef.numParts() - 1);
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
        } else {
            _preparedState->idxFound++;
        }

        // createPathAt() will complete the path and attach 'elemToSet' at the end of it.
        status = pathsupport::createPathAt(
            _fieldRef, _preparedState->idxFound, _preparedState->elemFound, baseArray);
        if (!status.isOK()) {
            return status;
        }

        // Point to the base array just created. The subsequent code expects it to exist
        // already.
        _preparedState->elemFound = baseArray;
    }

    // This is the count of the array before we change it, or 0 if missing from the doc.
    _preparedState->arrayPreModSize = countChildren(_preparedState->elemFound);

    // 2. Add new elements to the array either by going over the $each array or by
    // appending the (old style $push) element.
    if (_eachMode || _pushMode == PUSH_ALL) {
        BSONObjIterator itEach(_eachElem.embeddedObject());

        // When adding more than one element we keep track of the previous one
        // so we can add right siblings to it.
        mutablebson::Element prevElem = _preparedState->doc.end();

        // The first element is special below
        bool first = true;

        while (itEach.more()) {
            BSONElement eachItem = itEach.next();
            mutablebson::Element elem =
                _preparedState->doc.makeElementWithNewFieldName(StringData(), eachItem);

            if (first) {
                status = pushFirstElement(_preparedState->elemFound,
                                          _preparedState->arrayPreModSize,
                                          _startPosition,
                                          elem);
            } else {
                status = prevElem.addSiblingRight(elem);
            }

            if (!status.isOK()) {
                return status;
            }

            // For the next iteration the previous element will be the left sibling
            prevElem = elem;
            first = false;
        }
    } else {
        mutablebson::Element elem =
            _preparedState->doc.makeElementWithNewFieldName(StringData(), _val);
        if (!elem.ok()) {
            return Status(ErrorCodes::InternalError, "can't wrap element being $push-ed");
        }
        return pushFirstElement(
            _preparedState->elemFound, _preparedState->arrayPreModSize, _startPosition, elem);
    }

    // 3. Sort the resulting array, if $sort was requested.
    if (_sortPresent) {
        sortChildren(_preparedState->elemFound, _sort);
    }

    // 4. Trim the resulting array according to $slice, if present.
    if (_slicePresent) {
        // Slice 0 means to remove all
        if (_slice == 0) {
            while (_preparedState->elemFound.ok() && _preparedState->elemFound.rightChild().ok()) {
                _preparedState->elemFound.rightChild().remove();
            }
        }

        const int64_t numChildren = mutablebson::countChildren(_preparedState->elemFound);
        int64_t countRemoved = std::max(static_cast<int64_t>(0), numChildren - abs(_slice));

        // If _slice is negative, remove from the bottom, otherwise from the top
        const bool removeFromEnd = (_slice > 0);

        // Either start at right or left depending if we are taking from top or bottom
        mutablebson::Element curr = removeFromEnd ? _preparedState->elemFound.rightChild()
                                                  : _preparedState->elemFound.leftChild();
        while (curr.ok() && countRemoved > 0) {
            mutablebson::Element toRemove = curr;
            // Either go right or left depending if we are taking from top or bottom
            curr = removeFromEnd ? curr.leftSibling() : curr.rightSibling();

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
    // The start position to use for positional (ordinal) updates to the array
    // (We will increment as we append elements to the oplog entry so can't be const)
    size_t position = _preparedState->arrayPreModSize;

    // NOTE: Idempotence Requirement
    // In the case that the document does't have an array or it is empty we need to make sure
    // that the first time the field gets filled with items that it is a full set of the array.

    // If we sorted, sliced, or added the first items to the array, make a full array copy.
    const bool doFullCopy = _slicePresent || _sortPresent ||
        (position == 0)                                         // first element in new/empty array
        || (_startPosition < _preparedState->arrayPreModSize);  // add in middle

    if (doFullCopy) {
        return logBuilder->addToSetsWithNewFieldName(_fieldRef.dottedField(),
                                                     _preparedState->elemFound);
    } else {
        // Set only the positional elements appended
        if (_eachMode || _pushMode == PUSH_ALL) {
            // For each input element log it as a posisional $set
            BSONObjIterator itEach(_eachElem.embeddedObject());
            while (itEach.more()) {
                BSONElement eachItem = itEach.next();
                // value for the logElement ("field.path.name.N": <value>)
                const std::string positionalName = mongoutils::str::stream()
                    << _fieldRef.dottedField() << "." << position++;

                Status s = logBuilder->addToSetsWithNewFieldName(positionalName, eachItem);
                if (!s.isOK())
                    return s;
            }

            return Status::OK();
        } else {
            // single value for the logElement ("field.path.name.N": <value>)
            const std::string positionalName = mongoutils::str::stream() << _fieldRef.dottedField()
                                                                         << "." << position++;

            return logBuilder->addToSetsWithNewFieldName(positionalName, _val);
        }
    }
}

}  // namespace mongo
