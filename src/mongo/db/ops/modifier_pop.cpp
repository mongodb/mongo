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

#include "mongo/db/ops/modifier_pop.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/db/ops/field_checker.h"
#include "mongo/db/ops/path_support.h"

namespace mongo {

    struct ModifierPop::PreparedState {

        PreparedState(mutablebson::Document* targetDoc)
            : doc(*targetDoc)
            , elementToRemove(doc.end())
            , pathFoundIndex(0)
            , pathFoundElement(doc.end())
            , pathPositionalPart() {
        }

        // Document that is going to be changed.
        mutablebson::Document& doc;

        // Element to be removed
        mutablebson::Element elementToRemove;

        // Index in _fieldRef for which an Element exist in the document.
        int32_t pathFoundIndex;

        // Element corresponding to _fieldRef[0.._idxFound].
        mutablebson::Element pathFoundElement;

        // Value to bind to a $-positional field, if one is provided.
        std::string pathPositionalPart;
    };

    ModifierPop::ModifierPop()
        : _fieldRef()
        , _positionalPathIndex(0)
        , _fromTop(false) {
    }

    ModifierPop::~ModifierPop() {
    }

    Status ModifierPop::init(const BSONElement& modExpr) {
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
        bool foundDollar = fieldchecker::isPositional(_fieldRef,
                                                      &_positionalPathIndex,
                                                      &foundCount);
        if (foundDollar && foundCount > 1) {
            return Status(ErrorCodes::BadValue, "too many positional($) elements found.");
        }

        //
        // value analysis
        //

        // TODO: tighten validation to numbers and just 1/-1 explicitly
        //if (!modExpr.isNumber()) {
        //    return Status(ErrorCodes::BadValue, "Must be a number");
        //}

        _fromTop = (modExpr.isNumber() && modExpr.number() < 0) ? true : false;

        return Status::OK();
    }

    Status ModifierPop::prepare(mutablebson::Element root,
                                  const StringData& matchedField,
                                  ExecInfo* execInfo) {

        _preparedState.reset(new PreparedState(&root.getDocument()));

        // If we have a $-positional field, it is time to bind it to an actual field part.
        if (_positionalPathIndex) {
            if (matchedField.empty()) {
                return Status(ErrorCodes::BadValue, "matched field not provided");
            }
            _preparedState->pathPositionalPart = matchedField.toString();
            _fieldRef.setPart(_positionalPathIndex, _preparedState->pathPositionalPart);
        }

        // Locate the field name in 'root'. Note that if we don't have the full path in the
        // doc, there isn't anything to unset, really.
        Status status = pathsupport::findLongestPrefix(_fieldRef,
                                                       root,
                                                       &_preparedState->pathFoundIndex,
                                                       &_preparedState->pathFoundElement);
        // Check if we didn't find the full path
        if (status.isOK()) {
            // If the path exists, we require the target field to be already an
            // array.
            if (_preparedState->pathFoundElement.getType() != Array) {
                return Status(ErrorCodes::BadValue, "can only $pop from arrays");
            }

            // No children, nothing to do -- not an error state
            if (!_preparedState->pathFoundElement.hasChildren()) {
                execInfo->noOp = execInfo->inPlace = true;
            } else {
                _preparedState->elementToRemove = _fromTop ?
                                _preparedState->pathFoundElement.leftChild() :
                                _preparedState->pathFoundElement.rightChild();
            }
        } else {
            // Let the caller know we can't do anything given the mod, _fieldRef, and doc.
            execInfo->noOp = execInfo->inPlace = true;
            _preparedState->pathFoundElement = root.getDocument().end();

            //okay if path not found
            if (status.code() == ErrorCodes::NonExistentPath)
                status = Status::OK();
        }

        // Let the caller know what field we care about
        execInfo->fieldRef[0] = &_fieldRef;

        return status;
    }

    Status ModifierPop::apply() const {
        return _preparedState->elementToRemove.remove();
    }

    Status ModifierPop::log(mutablebson::Element logRoot) const {
        // log document
        mutablebson::Document& doc = logRoot.getDocument();
        const bool pathExists = _preparedState->pathFoundElement.ok() &&
                                (_preparedState->pathFoundIndex ==
                                    static_cast<int32_t>(_fieldRef.numParts() - 1));

        // element to log, like $set/$unset
        mutablebson::Element opElement = pathExists ?
                                            doc.makeElementObject("$set") :
                                            doc.makeElementObject("$unset");
        if (!opElement.ok()) {
            return Status(ErrorCodes::InternalError, "cannot create log entry");
        }

        // value for the logElement ("field.path.name": <value>)
        mutablebson::Element logElement = pathExists ?
                                            logRoot.getDocument().makeElementWithNewFieldName(
                                                    _fieldRef.dottedField(),
                                                    _preparedState->pathFoundElement
                                                    ):
                                            doc.makeElementBool(_fieldRef.dottedField(), true);
        if (!logElement.ok()) {
            return Status(ErrorCodes::InternalError, "cannot create details");
        }

        // Now, we attach the {<fieldname>: <value>} Element under the {$op: ...} one.
        Status status = opElement.pushBack(logElement);
        if (!status.isOK()) {
            return status;
        }

        // And attach the result under the 'logRoot' Element provided.
        return logRoot.pushBack(opElement);
    }
} // namespace mongo
