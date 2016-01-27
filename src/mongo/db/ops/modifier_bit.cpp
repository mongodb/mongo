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

#include "mongo/db/ops/modifier_bit.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/db/ops/field_checker.h"
#include "mongo/db/ops/log_builder.h"
#include "mongo/db/ops/path_support.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace mb = mutablebson;
namespace str = mongoutils::str;

struct ModifierBit::PreparedState {
    PreparedState(mutablebson::Document& doc)
        : doc(doc), idxFound(0), elemFound(doc.end()), noOp(false) {}

    // Document that is going to be changed.
    mutablebson::Document& doc;

    // Index in _fieldRef for which an Element exist in the document.
    size_t idxFound;

    // Element corresponding to _fieldRef[0.._idxFound].
    mutablebson::Element elemFound;

    // Value to be applied.
    SafeNum newValue;

    // True if this update is a no-op
    bool noOp;
};

ModifierBit::ModifierBit() : ModifierInterface(), _fieldRef(), _posDollar(0), _ops() {}

ModifierBit::~ModifierBit() {}

Status ModifierBit::init(const BSONElement& modExpr, const Options& opts, bool* positional) {
    // Perform standard field name and updateable checks.
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
                                    << _fieldRef.dottedField() << "'");
    }

    if (modExpr.type() != mongo::Object)
        return Status(ErrorCodes::BadValue,
                      str::stream() << "The $bit modifier is not compatible with a "
                                    << typeName(modExpr.type())
                                    << ". You must pass in an embedded document: "
                                       "{$bit: {field: {and/or/xor: #}}");

    if (modExpr.embeddedObject().isEmpty()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "You must pass in at least one bitwise operation. "
                                    << "The format is: "
                                       "{$bit: {field: {and/or/xor: #}}");
    }

    BSONObjIterator opsIterator(modExpr.embeddedObject());

    while (opsIterator.more()) {
        BSONElement curOp = opsIterator.next();

        const StringData payloadFieldName = curOp.fieldName();

        SafeNumOp op = NULL;

        if (payloadFieldName == "and") {
            op = &SafeNum::bitAnd;
        } else if (payloadFieldName == "or") {
            op = &SafeNum::bitOr;
        } else if (payloadFieldName == "xor") {
            op = &SafeNum::bitXor;
        } else {
            return Status(ErrorCodes::BadValue,
                          str::stream()
                              << "The $bit modifier only supports 'and', 'or', and 'xor', not '"
                              << payloadFieldName << "' which is an unknown operator: {" << curOp
                              << "}");
        }

        if ((curOp.type() != mongo::NumberInt) && (curOp.type() != mongo::NumberLong))
            return Status(ErrorCodes::BadValue,
                          str::stream()
                              << "The $bit modifier field must be an Integer(32/64 bit); a '"
                              << typeName(curOp.type()) << "' is not supported here: {" << curOp
                              << "}");

        const OpEntry entry = {SafeNum(curOp), op};
        _ops.push_back(entry);
    }

    dassert(!_ops.empty());

    return Status::OK();
}

Status ModifierBit::prepare(mutablebson::Element root,
                            StringData matchedField,
                            ExecInfo* execInfo) {
    _preparedState.reset(new PreparedState(root.getDocument()));

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

    // Locate the field name in 'root'.
    Status status = pathsupport::findLongestPrefix(
        _fieldRef, root, &_preparedState->idxFound, &_preparedState->elemFound);


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
    execInfo->fieldRef[0] = &_fieldRef;

    //
    // in-place and no-op logic
    //

    // If the field path is not fully present, then this mod cannot be in place, nor is a
    // noOp.
    if (!_preparedState->elemFound.ok() || _preparedState->idxFound < (_fieldRef.numParts() - 1)) {
        // If no target element exists, the value we will write is the result of applying
        // the operation to a zero-initialized integer element.
        _preparedState->newValue = apply(SafeNum(static_cast<int32_t>(0)));
        return Status::OK();
    }

    if (!_preparedState->elemFound.isIntegral()) {
        mb::Element idElem = mb::findElementNamed(root.leftChild(), "_id");
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Cannot apply $bit to a value of non-integral type."
                                    << idElem.toString() << " has the field "
                                    << _preparedState->elemFound.getFieldName()
                                    << " of non-integer type "
                                    << typeName(_preparedState->elemFound.getType()));
    }

    const SafeNum currentValue = _preparedState->elemFound.getValueSafeNum();

    // Apply the op over the existing value and the mod value, and capture the result.
    _preparedState->newValue = apply(currentValue);

    if (!_preparedState->newValue.isValid()) {
        // TODO: Include list of ops, if that is easy, at some future point.
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Failed to apply $bit operations to current value: "
                                    << currentValue.debugString());
    }
    // If the values are identical (same type, same value), then this is a no-op.
    if (_preparedState->newValue.isIdentical(currentValue)) {
        _preparedState->noOp = execInfo->noOp = true;
        return Status::OK();
    }

    return Status::OK();
}

Status ModifierBit::apply() const {
    dassert(_preparedState->noOp == false);

    // If there's no need to create any further field part, the $bit is simply a value
    // assignment.
    if (_preparedState->elemFound.ok() && _preparedState->idxFound == (_fieldRef.numParts() - 1)) {
        return _preparedState->elemFound.setValueSafeNum(_preparedState->newValue);
    }

    //
    // Complete document path logic
    //

    // Creates the final element that's going to be $set in 'doc'.
    mutablebson::Document& doc = _preparedState->doc;
    StringData lastPart = _fieldRef.getPart(_fieldRef.numParts() - 1);
    mutablebson::Element elemToSet = doc.makeElementSafeNum(lastPart, _preparedState->newValue);
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
    return pathsupport::createPathAt(
        _fieldRef, _preparedState->idxFound, _preparedState->elemFound, elemToSet);
}

Status ModifierBit::log(LogBuilder* logBuilder) const {
    mutablebson::Element logElement = logBuilder->getDocument().makeElementSafeNum(
        _fieldRef.dottedField(), _preparedState->newValue);

    if (!logElement.ok()) {
        return Status(ErrorCodes::InternalError,
                      str::stream() << "Could not append entry to $bit oplog entry: "
                                    << "set '" << _fieldRef.dottedField() << "' -> "
                                    << _preparedState->newValue.debugString());
    }
    return logBuilder->addToSets(logElement);
}

SafeNum ModifierBit::apply(SafeNum value) const {
    OpEntries::const_iterator where = _ops.begin();
    const OpEntries::const_iterator end = _ops.end();
    for (; where != end; ++where)
        value = (value.*(where->op))(where->val);
    return value;
}

}  // namespace mongo
