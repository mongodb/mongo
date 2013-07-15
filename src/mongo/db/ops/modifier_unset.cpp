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

#include "mongo/db/ops/modifier_unset.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/db/ops/field_checker.h"
#include "mongo/db/ops/path_support.h"

namespace mongo {

    struct ModifierUnset::PreparedState {

        PreparedState(mutablebson::Document* targetDoc)
            : doc(*targetDoc)
            , idxFound(0)
            , elemFound(doc.end())
            , boundDollar("")
            , inPlace(false)
            , noOp(false) {
        }

        // Document that is going to be changed.
        mutablebson::Document& doc;

        // Index in _fieldRef for which an Element exist in the document.
        int32_t idxFound;

        // Element corresponding to _fieldRef[0.._idxFound].
        mutablebson::Element elemFound;

        // Value to bind to a $-positional field, if one is provided.
        std::string boundDollar;

        // Could this mod be applied in place?
        bool inPlace;

        // This $set is a no-op?
        bool noOp;

    };

    ModifierUnset::ModifierUnset()
        : _fieldRef()
        , _posDollar(0)
        , _val() {
    }

    ModifierUnset::~ModifierUnset() {
    }

    Status ModifierUnset::init(const BSONElement& modExpr) {

        //
        // field name analysis
        //

        // Break down the field name into its 'dotted' components (aka parts) and check that
        // there are no empty parts.
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

        // Unset takes any value, since there is no semantics attached to such value.
        _val = modExpr;

        return Status::OK();
    }

    Status ModifierUnset::prepare(mutablebson::Element root,
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

        // Locate the field name in 'root'. Note that if we don't have the full path in the
        // doc, there isn't anything to unset, really.
        Status status = pathsupport::findLongestPrefix(_fieldRef,
                                                       root,
                                                       &_preparedState->idxFound,
                                                       &_preparedState->elemFound);
        if (!status.isOK() ||
            _preparedState->idxFound != static_cast<int32_t>(_fieldRef.numParts() -1)) {
            execInfo->noOp = _preparedState->noOp = true;
            execInfo->inPlace = _preparedState->noOp = true;
            execInfo->fieldRef[0] = &_fieldRef;

            return Status::OK();
        }

        // If there is indeed something to unset, we register so, along with the interest in
        // the field name. The driver needs this info to sort out if there is any conflict
        // among mods.
        execInfo->fieldRef[0] = &_fieldRef;

        // The only way for an $unset to be inplace is for its target field to be the last one
        // of the object. That is, it is always the right child on its paths. The current
        // rationale is that there should be no holes in a BSONObj and, to be in place, no
        // field boundaries must change.
        //
        // TODO:
        // execInfo->inPlace = true;
        // mutablebson::Element curr = _preparedState->elemFound;
        // while (curr.ok()) {
        //     if (curr.rightSibling().ok()) {
        //         execInfo->inPlace = false;
        //     }
        //     curr = curr.parent();
        // }

        return Status::OK();
    }

    Status ModifierUnset::apply() const {
        dassert(!_preparedState->noOp);

        // Our semantics says that, if we're unseting an element of an array, we swap that
        // value to null. The rationale is that we don't want other array elements to change
        // indices. (That could be achieved with $pull-ing element from it.)
        if (_preparedState->elemFound.parent().ok() &&
            _preparedState->elemFound.parent().getType() == Array) {
            return _preparedState->elemFound.setValueNull();
        }
        else {
            return _preparedState->elemFound.remove();
        }
    }

    Status ModifierUnset::log(mutablebson::Element logRoot) const {

        // We'd like to create an entry such as {$unset: {<fieldname>: 1}} under 'logRoot'.
        // We start by creating the {$unset: ...} Element.
        mutablebson::Document& doc = logRoot.getDocument();
        mutablebson::Element unsetElement = doc.makeElementObject("$unset");
        if (!unsetElement.ok()) {
            return Status(ErrorCodes::InternalError, "cannot create log entry for $unset mod");
        }

        // Then we create the {<fieldname>: <value>} Element. Note that <fieldname> must be a
        // dotted field, and not only the last part of that field. The rationale here is that
        // somoene picking up this log entry -- e.g., a secondary -- must be capable of doing
        // the same path find/creation that was done in the previous calls here.
        mutablebson::Element logElement = doc.makeElementInt(_fieldRef.dottedField(), 1);
        if (!logElement.ok()) {
            return Status(ErrorCodes::InternalError, "cannot create log details for $unset mod");
        }

        // Now, we attach the {<fieldname>: `} Element under the {$unset: ...} one.
        Status status = unsetElement.pushBack(logElement);
        if (!status.isOK()) {
            return status;
        }

        // And attach the result under the 'logRoot' Element provided.
        return logRoot.pushBack(unsetElement);
    }

} // namespace mongo
