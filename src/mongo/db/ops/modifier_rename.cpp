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

#include "mongo/db/ops/modifier_rename.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/db/ops/field_checker.h"
#include "mongo/db/ops/log_builder.h"
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
        size_t toIdxFound;

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

    Status ModifierRename::init(const BSONElement& modExpr, const Options& opts) {

        if (modExpr.type() != String) {
            return Status(ErrorCodes::BadValue, "rename 'to' field must be a string");
        }

        // Extract the field names from the mod expression

        _fromFieldRef.parse(modExpr.fieldName());
        // The 'from' field is checked with legacy so that we can rename away from malformed values.
        Status status = fieldchecker::isUpdatableLegacy(_fromFieldRef);
        if (!status.isOK())
            return status;

        _toFieldRef.parse(modExpr.String());
        // The 'to' field is checked normally so we can't create new malformed values.
        status = fieldchecker::isUpdatable(_toFieldRef);
        if (!status.isOK())
            return status;

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
        size_t fromIdxFound;
        Status status = pathsupport::findLongestPrefix(_fromFieldRef,
                                                       root,
                                                       &fromIdxFound,
                                                       &_preparedState->fromElemFound);

        // If we can't find the full element in the from field then we can't do anything.
        if (!status.isOK()) {
            execInfo->noOp = true;
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

        execInfo->noOp = false;

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
                                (_preparedState->toIdxFound == (_toFieldRef.numParts()-1));

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
        size_t tempIdx = 0;
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

    Status ModifierRename::log(LogBuilder* logBuilder) const {

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
        mutablebson::Document& doc = logBuilder->getDocument();

        // Create the {<fieldname>: <value>} Element. Note that we log the mod with a
        // dotted field, if it was applied over a dotted field. The rationale is that the
        // secondary may be in a different state than the primary and thus make different
        // decisions about creating the intermediate path in _fieldRef or not.
        mutablebson::Element logElement = doc.makeElementWithNewFieldName(
            setPath, _preparedState->fromElemFound.getValue());

        if (!logElement.ok()) {
            return Status(ErrorCodes::InternalError, "cannot create details for $rename mod");
        }

        // Now, we attach the {<fieldname>: <value>} Element under the {$set: ...} section.
        Status status = logBuilder->addToSets(logElement);

        if (status.isOK() && doUnset) {
            // Create the {<fieldname>: <value>} Element. Note that we log the mod with a
            // dotted field, if it was applied over a dotted field. The rationale is that the
            // secondary may be in a different state than the primary and thus make different
            // decisions about creating the intermediate path in _fieldRef or not.
            mutablebson::Element unsetEntry = doc.makeElementBool(unsetPath, true);
            if (!unsetEntry.ok()) {
                return Status(ErrorCodes::InternalError, "cannot create details for $rename mod");
            }

            // Now, we attach the Element under the {$unset: ...} section.
            status = logBuilder->addToUnsets(unsetEntry);
        }

        return status;
    }

} // namespace mongo
