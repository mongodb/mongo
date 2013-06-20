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

#include "mongo/db/ops/modifier_rename.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/db/ops/field_checker.h"
#include "mongo/db/ops/path_support.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using mongoutils::str::stream;

    struct ModifierRename::PreparedState {

        PreparedState(mutablebson::Element root)
            : doc(root.getDocument())
            , fromElemFound(doc.end())
            , toIdxFound(0)
            , toElemFound(doc.end())
            , applyCalled(false){
        }

        // Document that is going to be changed.
        mutablebson::Document& doc;

        // The element to rename
        mutablebson::Element fromElemFound;

        // Index in _fieldRef for which an Element exist in the document.
        int32_t toIdxFound;

        // Element to remove (in the destination position)
        mutablebson::Element toElemFound;

        // Was apply called?
        bool applyCalled;

    };

    ModifierRename::ModifierRename()
        : _fromFieldRef()
        , _toFieldRef() {
    }

    ModifierRename::~ModifierRename() {
    }

    Status ModifierRename::init(const BSONElement& modExpr) {

        if (modExpr.type() != String) {
            return Status(ErrorCodes::BadValue, "rename 'to' field must be a string");
        }

        // Extract the field names from the mod expression
        _fromFieldRef.parse(modExpr.fieldName());
        _toFieldRef.parse(modExpr.String());

        for (int i = 0; i < 2; i++) {

            // 0 - to field, 1 - from field
            const FieldRef& field = i ? _fromFieldRef : _toFieldRef;

            size_t numParts = field.numParts();
            if (numParts == 0) {
                return Status(ErrorCodes::BadValue, "empty field name");
            }

            // Not all well-formed fields can be updated. For instance, '_id' can't be touched.
            Status status = fieldchecker::isUpdatable(field);
            if (! status.isOK()) {
                return status;
            }
        }

        // TODO: Remove this restriction and make a noOp to lift restriction
        // Old restriction is that if the fields are the same then it is not allowed.
        if (_fromFieldRef == _toFieldRef)
            return Status(ErrorCodes::BadValue, "$rename source must differ from target");

        // TODO: Remove this restriction by allowing moving deeping from the 'from' path
        // Old restriction is that if the to/from is on the same path it fails
        if (_fromFieldRef.isPrefixOf(_toFieldRef) || _toFieldRef.isPrefixOf(_fromFieldRef)){
            return Status(ErrorCodes::BadValue,
                          "$rename source/destination cannot be on the same path");
        }
        // TODO: We can remove this restriction as long as there is only one,
        //       or it is the same array -- should think on this a bit.
        //
        // If a $-positional operator was used it is an error
        size_t dummyPos;
        if (fieldchecker::isPositional(_fromFieldRef, &dummyPos))
            return Status(ErrorCodes::BadValue, "$rename source may not be dynamic array");
        else if (fieldchecker::isPositional(_toFieldRef, &dummyPos))
            return Status(ErrorCodes::BadValue, "$rename target may not be dynamic array");

        return Status::OK();
    }

    Status ModifierRename::prepare(mutablebson::Element root,
                                   const StringData& matchedField,
                                   ExecInfo* execInfo) {
        // Rename doesn't work with positional fields ($)
        dassert(matchedField.empty());

        _preparedState.reset(new PreparedState(root));

        // Locate the to field name in 'root', which must exist.
        int32_t fromIdxFound;
        Status status = pathsupport::findLongestPrefix(_fromFieldRef,
                                                       root,
                                                       &fromIdxFound,
                                                       &_preparedState->fromElemFound);

        // If we can't find the full element in the from field then we can't do anything.
        if (!status.isOK()) {
            execInfo->inPlace = execInfo->noOp = true;
            _preparedState->fromElemFound = root.getDocument().end();

            // TODO: remove this special case from existing behavior
            if (status.code() == ErrorCodes::PathNotViable) {
                return status;
            }

            return Status::OK();
        }

        // Ensure no array in ancestry if what we found is not at the root
        mutablebson::Element curr = _preparedState->fromElemFound.parent();
        while (curr.ok()) {
            if (curr.getType() == Array)
                return Status(ErrorCodes::BadValue, "source field cannot come from an array");
            curr = curr.parent();
        }

        // "To" side validation below

        status = pathsupport::findLongestPrefix(_toFieldRef,
                                                root,
                                                &_preparedState->toIdxFound,
                                                &_preparedState->toElemFound);

        // FindLongestPrefix may say the path does not exist at all, which is fine here, or
        // that the path was not viable or otherwise wrong, in which case, the mod cannot
        // proceed.
        if (status.code() == ErrorCodes::NonExistentPath) {
            _preparedState->toElemFound = root.getDocument().end();
        } else if (!status.isOK()) {
            return status;
        }

        // Ensure no array in ancestry of to Element
        // Set to either parent, or node depending if the full path element was found
        curr = (_preparedState->toElemFound != root.getDocument().end() ?
                                        _preparedState->toElemFound.parent() :
                                        _preparedState->toElemFound);
        curr = _preparedState->toElemFound;
        while (curr.ok()) {
            if (curr.getType() == Array)
                return Status(ErrorCodes::BadValue,
                              "destination field cannot have an array ancestor");
            curr = curr.parent();
        }

        // We register interest in the field name. The driver needs this info to sort out if
        // there is any conflict among mods.
        execInfo->fieldRef[0] = &_fromFieldRef;
        execInfo->fieldRef[1] = &_toFieldRef;

        execInfo->inPlace = execInfo->noOp = false;

        return Status::OK();
    }

    Status ModifierRename::apply() const {
        dassert(_preparedState->fromElemFound.ok());

        _preparedState->applyCalled = true;

        // Remove from source
        Status removeStatus = _preparedState->fromElemFound.remove();
        if (!removeStatus.isOK()) {
            return removeStatus;
        }

        // If there's no need to create any further field part, the op is simply a value
        // assignment.
        const bool destExists = _preparedState->toElemFound.ok() &&
                                (_preparedState->toIdxFound ==
                                        static_cast<int32_t>(_toFieldRef.numParts()-1));

        if (destExists) {
            removeStatus = _preparedState->toElemFound.remove();
            if (!removeStatus.isOK()) {
                return removeStatus;
            }
        }

        // Creates the final element that's going to be the in 'doc'.
        mutablebson::Document& doc = _preparedState->doc;
        StringData lastPart = _toFieldRef.getPart(_toFieldRef.numParts()-1);
        mutablebson::Element elemToSet = doc.makeElementWithNewFieldName(
                                                lastPart,
                                                _preparedState->fromElemFound);
        if (!elemToSet.ok()) {
            return Status(ErrorCodes::InternalError, "can't create new element");
        }

        // Find the new place to put the "to" element:
        // createPathAt does not use existing prefix elements so we
        // need to get the prefix match position for createPathAt below
        int32_t tempIdx = 0;
        mutablebson::Element tempElem = doc.end();
        Status status = pathsupport::findLongestPrefix(_toFieldRef,
                                                       doc.root(),
                                                       &tempIdx,
                                                       &tempElem);

        // createPathAt will complete the path and attach 'elemToSet' at the end of it.
        return pathsupport::createPathAt(_toFieldRef,
                                         tempElem == doc.end() ? 0 : tempIdx + 1,
                                         tempElem == doc.end() ? doc.root() : tempElem,
                                         elemToSet);
    }

    Status ModifierRename::log(mutablebson::Element logRoot) const {

        // If there was no element found then it was a noop, so return immediately
        if (!_preparedState->fromElemFound.ok())
            return Status::OK();

        // debug assert if apply not called, since we found an element to move.
        dassert(_preparedState->applyCalled);

        const bool isPrefix = _fromFieldRef.isPrefixOf(_toFieldRef);
        const string setPath = (isPrefix ? _fromFieldRef : _toFieldRef).dottedField();
        const string unsetPath = isPrefix ? "" : _fromFieldRef.dottedField();
        const bool doUnset = !isPrefix;

        // We'd like to create an entry such as {$set: {<fieldname>: <value>}} under 'logRoot'.
        // We start by creating the {$set: ...} Element.
        mutablebson::Document& doc = logRoot.getDocument();

        // Create $set
        mutablebson::Element setElement = doc.makeElementObject("$set");
        if (!setElement.ok()) {
            return Status(ErrorCodes::InternalError, "cannot create log entry for $set mod");
        }

        // Then we create the {<fieldname>: <value>} Element. Note that we log the mod with a
        // dotted field, if it was applied over a dotted field. The rationale is that the
        // secondary may be in a different state than the primary and thus make different
        // decisions about creating the intermediate path in _fieldRef or not.
        mutablebson::Element logElement
             = doc.makeElementWithNewFieldName( setPath, _preparedState->fromElemFound.getValue());
        if (!logElement.ok()) {
            return Status(ErrorCodes::InternalError, "cannot create details for $set mod");
        }

        // Now, we attach the {<fieldname>: <value>} Element under the {$set: ...} one.
        Status status = setElement.pushBack(logElement);
        if (!status.isOK()) {
            return status;
        }

        mutablebson::Element unsetElement = doc.end();
        if (doUnset) {
            // Create $unset
            unsetElement = doc.makeElementObject("$unset");
            if (!setElement.ok()) {
                return Status(ErrorCodes::InternalError, "cannot create log entry for $unset mod");
            }

            // Then we create the {<fieldname>: <value>} Element. Note that we log the mod with a
            // dotted field, if it was applied over a dotted field. The rationale is that the
            // secondary may be in a different state than the primary and thus make different
            // decisions about creating the intermediate path in _fieldRef or not.
            mutablebson::Element unsetEntry = doc.makeElementBool(unsetPath, true);
            if (!unsetEntry.ok()) {
                return Status(ErrorCodes::InternalError, "cannot create details for $unset mod");
            }

            // Now, we attach the {<fieldname>: <value>} Element under the {$set: ...} one.
            status = unsetElement.pushBack(unsetEntry);
            if (!status.isOK()) {
                return status;
            }

        }

        // And attach the result under the 'logRoot' Element provided.
        status = logRoot.pushBack(setElement);
        if (!status.isOK())
            return status;

        if (doUnset) {
            status = logRoot.pushBack(unsetElement);
            if (!status.isOK())
                return status;
        }

        return Status::OK();
    }

} // namespace mongo
