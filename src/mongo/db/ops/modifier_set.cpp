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

#include "mongo/db/ops/modifier_set.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/db/ops/field_checker.h"
#include "mongo/db/ops/log_builder.h"
#include "mongo/db/ops/path_support.h"

namespace mongo {


    struct ModifierSet::PreparedState {

        PreparedState(mutablebson::Document* targetDoc)
            : doc(*targetDoc)
            , idxFound(0)
            , elemFound(doc.end())
            , boundDollar("")
            , noOp(false)
            , elemIsBlocking(false) {
        }

        // Document that is going to be changed.
        mutablebson::Document& doc;

        // Index in _fieldRef for which an Element exist in the document.
        size_t idxFound;

        // Element corresponding to _fieldRef[0.._idxFound].
        mutablebson::Element elemFound;

        // Value to bind to a $-positional field, if one is provided.
        std::string boundDollar;

        // This $set is a no-op?
        bool noOp;

        // The element we find during a replication operation that blocks our update path
        bool elemIsBlocking;

    };

    ModifierSet::ModifierSet(ModifierSet::ModifierSetMode mode)
        : _fieldRef()
        , _posDollar(0)
        , _setMode(mode)
        , _val()
        , _modOptions() {
    }

    ModifierSet::~ModifierSet() {
    }

    Status ModifierSet::init(const BSONElement& modExpr, const Options& opts) {

        //
        // field name analysis
        //

        // Break down the field name into its 'dotted' components (aka parts) and check that
        // the field is fit for updates
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

        // Is the target set value safe?
        switch (modExpr.type()) {

        case EOO:
            return Status(ErrorCodes::BadValue, "cannot $set an empty value");

        case Object:
        case Array:
            if (! modExpr.Obj().okForStorage()) {
                return Status(ErrorCodes::BadValue, "cannot use '$' as values");
            }
            break;

        default:
            break;
        }

        _val = modExpr;
        _modOptions = opts;

        return Status::OK();
    }

    Status ModifierSet::prepare(mutablebson::Element root,
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
        else if (_modOptions.fromReplication && status.code() == ErrorCodes::PathNotViable) {
            // If we are coming from replication and it is an invalid path,
            // then push on indicating that we had a blocking element, which we stopped at
            _preparedState->elemIsBlocking = true;
        }
        else if (!status.isOK()) {
            return status;
        }

        if (_setMode == SET_ON_INSERT) {
            execInfo->context = ModifierInterface::ExecInfo::INSERT_CONTEXT;
        }

        // We register interest in the field name. The driver needs this info to sort out if
        // there is any conflict among mods.
        execInfo->fieldRef[0] = &_fieldRef;

        //
        // in-place and no-op logic
        //

        // If the field path is not fully present, then this mod cannot be in place, nor is a
        // noOp.
        if (!_preparedState->elemFound.ok() ||
            _preparedState->idxFound < (_fieldRef.numParts()-1)) {
            return Status::OK();
        }

        // If the value being $set is the same as the one already in the doc, than this is a
        // noOp.
        if (_preparedState->elemFound.ok() &&
            _preparedState->idxFound == (_fieldRef.numParts()-1) &&
            _preparedState->elemFound.compareWithBSONElement(_val, false /*ignore field*/) == 0) {
            execInfo->noOp = _preparedState->noOp = true;
        }

        return Status::OK();
    }

    Status ModifierSet::apply() const {
        dassert(!_preparedState->noOp);

        const bool destExists = _preparedState->elemFound.ok() &&
                                _preparedState->idxFound == (_fieldRef.numParts()-1);
        // If there's no need to create any further field part, the $set is simply a value
        // assignment.
        if (destExists) {
            return _preparedState->elemFound.setValueBSONElement(_val);
        }

        //
        // Complete document path logic
        //

        // Creates the final element that's going to be $set in 'doc'.
        mutablebson::Document& doc = _preparedState->doc;
        StringData lastPart = _fieldRef.getPart(_fieldRef.numParts()-1);
        mutablebson::Element elemToSet = doc.makeElementWithNewFieldName(lastPart, _val);
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

        // Remove the blocking element, if we are from replication applier. See comment below.
        if (_modOptions.fromReplication && !destExists && _preparedState->elemFound.ok() &&
            _preparedState->elemIsBlocking &&
            (!(_preparedState->elemFound.isType(Array)) ||
             !(_preparedState->elemFound.isType(Object)))
           ) {

            /**
             * With replication we want to be able to remove blocking elements for $set (only).
             *
             * Imagine that we started with this:
             * {_id:1, a:1} + {$set : {"a.b.c" : 1}} -> {_id:1, a: {b: {c:1}}}
             * Above we found that element (a:1) is blocking at position 1. We now will remove it,
             * point to its parent as the element we found and change our position so that
             * the normal logic below can be applied from the root (in this example case).
             */

            // keep track of the one to remove
            mutablebson::Element toRemove = _preparedState->elemFound;

            // decrement index position, and get parent for apply below
            _preparedState->idxFound--;
            _preparedState->elemFound = _preparedState->elemFound.parent();

            if (!_preparedState->elemFound.ok())
                return Status(ErrorCodes::IllegalOperation, "The parent is not valid.");

            // remove the element and check error
            Status ok = toRemove.remove();
            if (!ok.isOK())
                return ok;
        }

        // createPathAt() will complete the path and attach 'elemToSet' at the end of it.
        return pathsupport::createPathAt(_fieldRef,
                                         _preparedState->idxFound,
                                         _preparedState->elemFound,
                                         elemToSet);
    }

    Status ModifierSet::log(LogBuilder* logBuilder) const {

        // We'd like to create an entry such as {$set: {<fieldname>: <value>}} under 'logRoot'.
        // We start by creating the {$set: ...} Element.
        mutablebson::Document& doc = logBuilder->getDocument();

        // Create the {<fieldname>: <value>} Element. Note that we log the mod with a
        // dotted field, if it was applied over a dotted field. The rationale is that the
        // secondary may be in a different state than the primary and thus make different
        // decisions about creating the intermediate path in _fieldRef or not.
        mutablebson::Element logElement = doc.makeElementWithNewFieldName(
            _fieldRef.dottedField(), _val);

        if (!logElement.ok()) {
            return Status(ErrorCodes::InternalError, "cannot create details for $set mod");
        }

        return logBuilder->addToSets(logElement);
    }

} // namespace mongo
