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

#include "mongo/db/ops/modifier_unset.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/db/ops/field_checker.h"
#include "mongo/db/ops/log_builder.h"
#include "mongo/db/ops/path_support.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace str = mongoutils::str;

struct ModifierUnset::PreparedState {
    PreparedState(mutablebson::Document* targetDoc)
        : doc(*targetDoc), idxFound(0), elemFound(doc.end()), noOp(false) {}

    // Document that is going to be changed.
    mutablebson::Document& doc;

    // Index in _fieldRef for which an Element exist in the document.
    size_t idxFound;

    // Element corresponding to _fieldRef[0.._idxFound].
    mutablebson::Element elemFound;

    // This $set is a no-op?
    bool noOp;
};

ModifierUnset::ModifierUnset() : _fieldRef(), _posDollar(0), _val() {}

ModifierUnset::~ModifierUnset() {}

Status ModifierUnset::init(const BSONElement& modExpr, const Options& opts, bool* positional) {
    //
    // field name analysis
    //

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


    //
    // value analysis
    //

    // Unset takes any value, since there is no semantics attached to such value.
    _val = modExpr;

    return Status::OK();
}

Status ModifierUnset::prepare(mutablebson::Element root,
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


    // Locate the field name in 'root'. Note that if we don't have the full path in the
    // doc, there isn't anything to unset, really.
    Status status = pathsupport::findLongestPrefix(
        _fieldRef, root, &_preparedState->idxFound, &_preparedState->elemFound);
    if (!status.isOK() || _preparedState->idxFound != (_fieldRef.numParts() - 1)) {
        execInfo->noOp = _preparedState->noOp = true;
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
    // mutablebson::Element curr = _preparedState->elemFound;
    // while (curr.ok()) {
    //     if (curr.rightSibling().ok()) {
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
    } else {
        return _preparedState->elemFound.remove();
    }
}

Status ModifierUnset::log(LogBuilder* logBuilder) const {
    return logBuilder->addToUnsets(_fieldRef.dottedField());
}

}  // namespace mongo
