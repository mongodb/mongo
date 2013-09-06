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

#include "mongo/db/ops/modifier_pull_all.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/db/ops/field_checker.h"
#include "mongo/db/ops/log_builder.h"
#include "mongo/db/ops/path_support.h"

namespace mongo {

    struct ModifierPullAll::PreparedState {

        PreparedState(mutablebson::Document* targetDoc)
            : doc(*targetDoc)
            , pathFoundIndex(0)
            , pathFoundElement(doc.end())
            , pathPositionalPart(std::string())
            , applyCalled(false)
            , elementsToRemove() {
        }

        // Document that is going to be changed.
        mutablebson::Document& doc;

        // Index in _fieldRef for which an Element exist in the document.
        size_t pathFoundIndex;

        // Element corresponding to _fieldRef[0.._idxFound].
        mutablebson::Element pathFoundElement;

        // Value to bind to a $-positional field, if one is provided.
        std::string pathPositionalPart;

        bool applyCalled;

        // Elements to be removed
       vector<mutablebson::Element> elementsToRemove;
    };

    namespace {

        struct mutableElementEqualsBSONElement : std::unary_function<BSONElement, bool>
        {
            mutableElementEqualsBSONElement(const mutablebson::Element& elem) : _what(elem) {}
            bool operator()(const BSONElement& elem) const {
                return _what.compareWithBSONElement(elem, false) == 0;
            }
            const mutablebson::Element& _what;
        };
    } // namespace

    ModifierPullAll::ModifierPullAll()
        : _fieldRef()
        , _positionalPathIndex(0)
        , _elementsToFind() {
    }

    ModifierPullAll::~ModifierPullAll() {
    }

    Status ModifierPullAll::init(const BSONElement& modExpr, const Options& opts) {

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

        if (modExpr.type() != Array) {
            return Status(ErrorCodes::BadValue, "$pullAll requires an array argument");
        }

        // store the stuff to remove later
        _elementsToFind = modExpr.Array();

        return Status::OK();
    }

    Status ModifierPullAll::prepare(mutablebson::Element root,
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
                return Status(ErrorCodes::BadValue, "can only $pull* from arrays");
            }

            // No children, nothing to do -- not an error state
            if (!_preparedState->pathFoundElement.hasChildren()) {
                execInfo->noOp = true;
            } else {
                mutablebson::Element elem = _preparedState->pathFoundElement.leftChild();
                while (elem.ok()) {
                    if (std::find_if(_elementsToFind.begin(),
                                     _elementsToFind.end(),
                                     mutableElementEqualsBSONElement(elem))
                                        != _elementsToFind.end()) {
                        _preparedState->elementsToRemove.push_back(elem);
                    }
                    elem = elem.rightSibling();
                }

                // Nothing to remove so it is a noOp.
                if (_preparedState->elementsToRemove.empty())
                    execInfo->noOp = true;
            }

        } else {
            // Let the caller know we can't do anything given the mod, _fieldRef, and doc.
            execInfo->noOp = true;


            //okay if path not found
            if (status.code() == ErrorCodes::NonExistentPath)
                status = Status::OK();
        }

        // Let the caller know what field we care about
        execInfo->fieldRef[0] = &_fieldRef;

        return status;
    }

    Status ModifierPullAll::apply() const {
        _preparedState->applyCalled = true;

        vector<mutablebson::Element>::const_iterator curr =
                                            _preparedState->elementsToRemove.begin();
        const vector<mutablebson::Element>::const_iterator end =
                                            _preparedState->elementsToRemove.end();
        for ( ; curr != end; ++curr) {
            const_cast<mutablebson::Element&>(*curr).remove();
        }
        return Status::OK();
    }

    Status ModifierPullAll::log(LogBuilder* logBuilder) const {
        // log document
        mutablebson::Document& doc = logBuilder->getDocument();

        const bool pathExists = _preparedState->pathFoundElement.ok() &&
            (_preparedState->pathFoundIndex == (_fieldRef.numParts() - 1));

        // value for the logElement ("field.path.name": <value>)
        mutablebson::Element logElement = pathExists ?
            doc.makeElementWithNewFieldName(
                _fieldRef.dottedField(),
                _preparedState->pathFoundElement):
            doc.makeElementBool(_fieldRef.dottedField(), true);

        if (!logElement.ok()) {
            return Status(ErrorCodes::InternalError, "cannot create details");
        }

        // Now, we attach the {<fieldname>: <value>} Element under the {$op: ...} one.
        return pathExists ?
            logBuilder->addToSets(logElement) :
            logBuilder->addToUnsets(logElement);
    }

} // namespace mongo
