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

#include "mongo/db/ops/modifier_add_to_set.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/db/ops/field_checker.h"
#include "mongo/db/ops/log_builder.h"
#include "mongo/db/ops/path_support.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace mb = mutablebson;
namespace str = mongoutils::str;

namespace {

template <typename Ordering, typename Equality>
void deduplicate(mb::Element parent, Ordering comp, Equality equal) {
    // First, build a vector of the children.
    std::vector<mb::Element> children;
    mb::Element current = parent.leftChild();
    while (current.ok()) {
        children.push_back(current);
        current = current.rightSibling();
    }

    // Then, sort the child vector with our comparator.
    std::sort(children.begin(), children.end(), comp);

    // Next, remove duplicates by walking the vector.
    std::vector<mb::Element>::iterator where = children.begin();
    const std::vector<mb::Element>::iterator end = children.end();

    while (where != end) {
        std::vector<mb::Element>::iterator next = where;
        ++next;
        while (next != end && equal(*where, *next)) {
            next->remove();
            ++next;
        }
        where = next;
    }
}

}  // namespace

struct ModifierAddToSet::PreparedState {
    PreparedState(mb::Document& doc)
        : doc(doc),
          idxFound(0),
          elemFound(doc.end()),
          addAll(false),
          elementsToAdd(),
          noOp(false) {}

    // Document that is going to be changed.
    mb::Document& doc;

    // Index in _fieldRef for which an Element exist in the document.
    size_t idxFound;

    // Element corresponding to _fieldRef[0.._idxFound].
    mb::Element elemFound;

    // Are we adding all of the $each elements, or just a subset?
    bool addAll;

    // Values to be applied.
    std::vector<mb::Element> elementsToAdd;

    // True if this update is a no-op
    bool noOp;
};

ModifierAddToSet::ModifierAddToSet()
    : ModifierInterface(), _fieldRef(), _posDollar(0), _valDoc(), _val(_valDoc.end()) {}

ModifierAddToSet::~ModifierAddToSet() {}

Status ModifierAddToSet::init(const BSONElement& modExpr, const Options& opts, bool* positional) {
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
                                    << _fieldRef.dottedField()
                                    << "'");
    }

    // TODO: The driver could potentially do this re-writing.

    // If the type of the value is 'Object', we might be dealing with a $each. See if that
    // is the case.
    if (modExpr.type() == mongo::Object) {
        BSONElement modExprObjPayload = modExpr.embeddedObject().firstElement();
        if (!modExprObjPayload.eoo() && StringData(modExprObjPayload.fieldName()) == "$each") {
            // It is a $each. Verify that the payload is an array as is required for $each,
            // set our flag, and store the array as our value.
            if (modExprObjPayload.type() != mongo::Array) {
                return Status(ErrorCodes::BadValue,
                              str::stream() << "The argument to $each in $addToSet must "
                                               "be an array but it was of type "
                                            << typeName(modExprObjPayload.type()));
            }

            status = _valDoc.root().appendElement(modExprObjPayload);
            if (!status.isOK())
                return status;

            _val = _valDoc.root().leftChild();
        }
    }

    // If this wasn't an 'each', turn it into one. No need to sort or de-dup since we only
    // have one element.
    if (_val == _valDoc.end()) {
        mb::Element each = _valDoc.makeElementArray("$each");

        status = each.appendElement(modExpr);
        if (!status.isOK())
            return status;

        status = _valDoc.root().pushBack(each);
        if (!status.isOK())
            return status;

        _val = each;
    }

    // Check if no invalid data (such as fields with '$'s) are being used in the $each
    // clause.
    mb::ConstElement valCursor = _val.leftChild();
    while (valCursor.ok()) {
        const BSONType type = valCursor.getType();
        dassert(valCursor.hasValue());
        switch (type) {
            case mongo::Object: {
                Status s = valCursor.getValueObject().storageValidEmbedded();
                if (!s.isOK())
                    return s;

                break;
            }
            case mongo::Array: {
                Status s = valCursor.getValueArray().storageValidEmbedded();
                if (!s.isOK())
                    return s;

                break;
            }
            default:
                break;
        }

        valCursor = valCursor.rightSibling();
    }

    setCollator(opts.collator);
    return Status::OK();
}

void ModifierAddToSet::setCollator(const CollatorInterface* collator) {
    invariant(!_collator);
    _collator = collator;
    // Deduplicate _val (must be performed after collator is set to final value.)
    deduplicate(_val, mb::woLess(false, _collator), mb::woEqual(false, _collator));
}

Status ModifierAddToSet::prepare(mb::Element root, StringData matchedField, ExecInfo* execInfo) {
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
        // If no target element exists, we will simply be creating a new array.
        _preparedState->addAll = true;
        return Status::OK();
    }

    // This operation only applies to arrays
    if (_preparedState->elemFound.getType() != mongo::Array) {
        mb::Element idElem = mb::findElementNamed(root.leftChild(), "_id");
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Cannot apply $addToSet to a non-array field. Field named '"
                                    << _preparedState->elemFound.getFieldName()
                                    << "' has a non-array type "
                                    << typeName(_preparedState->elemFound.getType())
                                    << " in the document "
                                    << idElem.toString());
    }

    // If the array is empty, then we don't need to check anything: all of the values are
    // going to be added.
    if (!_preparedState->elemFound.hasChildren()) {
        _preparedState->addAll = true;
        return Status::OK();
    }

    // For each value in the $each clause, compare it against the values in the array. If
    // the element is not present, record it as one to add.
    mb::Element eachIter = _val.leftChild();
    while (eachIter.ok()) {
        mb::Element where = mb::findElement(_preparedState->elemFound.leftChild(),
                                            mb::woEqualTo(eachIter, false, _collator));
        if (!where.ok()) {
            // The element was not found. Record the element from $each as one to be added.
            _preparedState->elementsToAdd.push_back(eachIter);
        }
        eachIter = eachIter.rightSibling();
    }

    // If we didn't find any elements to add, then this is a no-op.
    if (_preparedState->elementsToAdd.empty()) {
        _preparedState->noOp = execInfo->noOp = true;
    }

    return Status::OK();
}

Status ModifierAddToSet::apply() const {
    dassert(_preparedState->noOp == false);

    // TODO: The contents of this block are lifted directly from $push.

    // If the array field is not there, create it as an array and attach it to the
    // document.
    if (!_preparedState->elemFound.ok() || _preparedState->idxFound < (_fieldRef.numParts() - 1)) {
        // Creates the array element
        mb::Document& doc = _preparedState->doc;
        StringData lastPart = _fieldRef.getPart(_fieldRef.numParts() - 1);
        mb::Element baseArray = doc.makeElementArray(lastPart);
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
        Status status = pathsupport::createPathAt(
            _fieldRef, _preparedState->idxFound, _preparedState->elemFound, baseArray);
        if (!status.isOK()) {
            return status;
        }

        // Point to the base array just created. The subsequent code expects it to exist
        // already.
        _preparedState->elemFound = baseArray;
    }

    if (_preparedState->addAll) {
        // If we are adding all the values, we can just walk over _val;

        mb::Element where = _val.leftChild();
        while (where.ok()) {
            dassert(where.hasValue());

            mb::Element toAdd = _preparedState->doc.makeElement(where.getValue());
            Status status = _preparedState->elemFound.pushBack(toAdd);
            if (!status.isOK())
                return status;

            where = where.rightSibling();
        }

    } else {
        // Otherwise, we aren't adding all the values, and we need to add exactly those
        // elements that were found to be missing during our scan in prepare.
        std::vector<mb::Element>::const_iterator where = _preparedState->elementsToAdd.begin();

        const std::vector<mb::Element>::const_iterator end = _preparedState->elementsToAdd.end();

        for (; where != end; ++where) {
            dassert(where->hasValue());

            mb::Element toAdd = _preparedState->doc.makeElement(where->getValue());
            Status status = _preparedState->elemFound.pushBack(toAdd);
            if (!status.isOK())
                return status;
        }
    }

    return Status::OK();
}

Status ModifierAddToSet::log(LogBuilder* logBuilder) const {
    // TODO: This is copied more or less identically from $push. As a result, it copies the
    // behavior in $push that relies on 'apply' having been called unless this is a no-op.

    // TODO We can log just a positional set in several cases. For now, let's just log the
    // full resulting array.

    // We'd like to create an entry such as {$set: {<fieldname>: [<resulting aray>]}} under
    // 'logRoot'.  We start by creating the {$set: ...} Element.
    mb::Document& doc = logBuilder->getDocument();

    // Then we create the {<fieldname>:[]} Element, that is, an empty array.
    mb::Element logElement = doc.makeElementArray(_fieldRef.dottedField());
    if (!logElement.ok()) {
        return Status(ErrorCodes::InternalError, "cannot create details for $addToSet mod");
    }

    // Fill up the empty array.
    mb::Element curr = _preparedState->elemFound.leftChild();
    while (curr.ok()) {
        dassert(curr.hasValue());

        // We need to copy each array entry from the resulting document to the log
        // document.
        mb::Element currCopy = doc.makeElementWithNewFieldName(StringData(), curr.getValue());
        if (!currCopy.ok()) {
            return Status(ErrorCodes::InternalError, "could create copy element");
        }
        Status status = logElement.pushBack(currCopy);
        if (!status.isOK()) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "Could not append entry for $addToSet oplog entry."
                                        << "Underlying cause: "
                                        << status.toString());
        }
        curr = curr.rightSibling();
    }

    return logBuilder->addToSets(logElement);
}

}  // namespace mongo
