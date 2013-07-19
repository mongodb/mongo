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
 */

#include "mongo/db/ops/modifier_inc.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/db/ops/field_checker.h"
#include "mongo/db/ops/log_builder.h"
#include "mongo/db/ops/path_support.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using mongoutils::str::stream;

    struct ModifierInc::PreparedState {

        PreparedState(mutablebson::Document& doc)
            : doc(doc)
            , idxFound(0)
            , elemFound(doc.end())
            , boundDollar("")
            , newValue()
            , inPlace(false)
            , noOp(false) {
        }

        // Document that is going to be changed.
        mutablebson::Document& doc;

        // Index in _fieldRef for which an Element exist in the document.
        size_t idxFound;

        // Element corresponding to _fieldRef[0.._idxFound].
        mutablebson::Element elemFound;

        // Value to bind to a $-positional field, if one is provided.
        std::string boundDollar;

        // Value to be applied
        SafeNum newValue;

        // This $inc is in-place?
        bool inPlace;

        // This $inc is a no-op?
        bool noOp;
    };

    ModifierInc::ModifierInc()
        : ModifierInterface ()
        , _fieldRef()
        , _posDollar(0)
        , _val() {
    }

    ModifierInc::~ModifierInc() {
    }

    Status ModifierInc::init(const BSONElement& modExpr) {

        //
        // field name analysis
        //

        // Perform standard field name and updateable checks.
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

        if (!modExpr.isNumber()) {
            // TODO: Context for mod error messages would be helpful
            // include mod code, etc.
            return Status(ErrorCodes::BadValue,
                          "Cannot increment with non-numeric argument");
        }

        _val = modExpr;
        dassert(_val.isValid());

        return Status::OK();
    }

    Status ModifierInc::prepare(mutablebson::Element root,
                                const StringData& matchedField,
                                ExecInfo* execInfo) {

        _preparedState.reset(new PreparedState(root.getDocument()));

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
        // apply is a noOp or whether is can be in place. The remaining path, if missing, will
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
        else if (!status.isOK()) {
            return status;
        }

        // We register interest in the field name. The driver needs this info to sort out if
        // there is any conflict among mods.
        execInfo->fieldRef[0] = &_fieldRef;

        // Capture the value we are going to write. At this point, there may not be a value
        // against which to operate, so the result will be simply _val.
        _preparedState->newValue = _val;

        //
        // in-place and no-op logic
        //
        // If the field path is not fully present, then this mod cannot be in place, nor is a
        // noOp.
        if (!_preparedState->elemFound.ok() ||
            _preparedState->idxFound < (_fieldRef.numParts() - 1)) {
            return Status::OK();
        }

        // If the value being $inc'ed is the same as the one already in the doc, than this is a
        // noOp.
        if (!_preparedState->elemFound.isNumeric())
            return Status(ErrorCodes::BadValue,
                          "invalid attempt to increment a non-numeric field");

        const SafeNum currentValue = _preparedState->elemFound.getValueSafeNum();

        // Update newValue w.r.t to the current value of the found element.
        _preparedState->newValue += currentValue;

        // If the result of the addition is invalid, we must return an error.
        if (!_preparedState->newValue.isValid())
            return Status(ErrorCodes::BadValue,
                          "Failed to increment current value");

        // If the values are identical (same type, same value), then this is a no-op, and
        // therefore in-place as well.
        if (_preparedState->newValue.isIdentical(currentValue)) {
            _preparedState->noOp = execInfo->noOp = true;
            _preparedState->inPlace = execInfo->inPlace = true;
            return Status::OK();
        }

        // If the types are the same, this can be done in place.
        //
        // TODO: Potentially, cases where $inc results in a mixed type of the same size could
        // be in-place as well, but we don't currently handle them.
        if (_preparedState->newValue.type() == currentValue.type()) {
            _preparedState->inPlace = execInfo->inPlace = true;
        }

        return Status::OK();
    }

    Status ModifierInc::apply() const {
        dassert(_preparedState->noOp == false);

        // If there's no need to create any further field part, the $inc is simply a value
        // assignment.
        if (_preparedState->elemFound.ok() &&
            _preparedState->idxFound == (_fieldRef.numParts() - 1)) {
            return _preparedState->elemFound.setValueSafeNum(_preparedState->newValue);
        }

        dassert(_preparedState->inPlace == false);

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
        }
        else {
            _preparedState->idxFound++;
        }

        // createPathAt() will complete the path and attach 'elemToSet' at the end of it.
        return pathsupport::createPathAt(_fieldRef,
                                         _preparedState->idxFound,
                                         _preparedState->elemFound,
                                         elemToSet);
    }

    Status ModifierInc::log(LogBuilder* logBuilder) const {

        dassert(_preparedState->newValue.isValid());

        // We'd like to create an entry such as {$set: {<fieldname>: <value>}} under 'logRoot'.
        // We start by creating the {$set: ...} Element.
        mutablebson::Document& doc = logBuilder->getDocument();

        // Then we create the {<fieldname>: <value>} Element.
        mutablebson::Element logElement = doc.makeElementSafeNum(
            _fieldRef.dottedField(),
            _preparedState->newValue);

        if (!logElement.ok()) {
            return Status(ErrorCodes::InternalError, "cannot append details for $inc mod");
        }

        // Now, we attach the {<fieldname>: <value>} Element under the {$set: ...} segment.
        return logBuilder->addToSets(logElement);
    }

} // namespace mongo
