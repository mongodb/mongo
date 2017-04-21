/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/update/set_node.h"

#include "mongo/db/update/path_support.h"

namespace mongo {

Status SetNode::init(BSONElement modExpr, const CollatorInterface* collator) {
    invariant(modExpr.ok());

    _val = modExpr;

    return Status::OK();
}

Status SetNode::apply(mutablebson::Element element,
                      FieldRef* pathToCreate,
                      FieldRef* pathTaken,
                      StringData matchedField,
                      bool fromReplication,
                      const UpdateIndexData* indexData,
                      LogBuilder* logBuilder,
                      bool* indexesAffected,
                      bool* noop) {
    *indexesAffected = false;
    *noop = false;

    // Set the element with the new value.
    if (pathToCreate->empty()) {
        // If 'element' is deserialized, then element.getValue() will be EOO, which will never equal
        // _val.
        if (element.getValue().binaryEqualValues(_val)) {
            *noop = true;
            return Status::OK();
        }
        element.setValueBSONElement(_val);
    } else {
        auto elemToSet = element.getDocument().makeElementWithNewFieldName(
            pathToCreate->getPart(pathToCreate->numParts() - 1), _val);
        invariant(elemToSet.ok());
        auto status = pathsupport::createPathAt(*pathToCreate, 0, element, elemToSet);
        if (!status.isOK()) {
            // $sets on non-viable paths are ignored when the update came from replication. We do
            // not error because idempotency requires that any other update modifiers must still be
            // applied. For example, consider applying the following updates twice to an initially
            // empty document:
            // {$set: {c: 0}}
            // {$set: {'a.b': 0, c: 1}}
            // {$set: {a: 0}}
            // Setting 'a.b' will fail the second time, but we must still set 'c'.
            if (status.code() == ErrorCodes::PathNotViable && fromReplication) {
                *noop = true;
                return Status::OK();
            }
            return status;
        }
    }

    // Create full field path of set element.
    StringBuilder builder;
    builder << pathTaken->dottedField();
    if (!pathTaken->empty() && !pathToCreate->empty()) {
        builder << ".";
    }
    builder << pathToCreate->dottedField();
    auto fullPath = builder.str();

    // Determine if indexes are affected.
    if (indexData && indexData->mightBeIndexed(fullPath)) {
        *indexesAffected = true;
    }

    // Log the set.
    if (logBuilder) {
        auto logElement = logBuilder->getDocument().makeElementWithNewFieldName(fullPath, _val);
        invariant(logElement.ok());
        return logBuilder->addToSets(logElement);
    }

    return Status::OK();
}

}  // namespace mongo
