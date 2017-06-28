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

#include "mongo/db/update/pop_node.h"

#include "mongo/db/update/path_support.h"

namespace mongo {

Status PopNode::init(BSONElement modExpr, const CollatorInterface* collator) {
    _popFromFront = modExpr.isNumber() && modExpr.number() < 0;
    return Status::OK();
}

void PopNode::apply(mutablebson::Element element,
                    FieldRef* pathToCreate,
                    FieldRef* pathTaken,
                    StringData matchedField,
                    bool fromReplication,
                    const UpdateIndexData* indexData,
                    LogBuilder* logBuilder,
                    bool* indexesAffected,
                    bool* noop) const {
    *indexesAffected = false;
    *noop = false;

    if (pathTaken->empty()) {
        // No components of the path existed. The pop is treated as a no-op in this case.
        *noop = true;
        return;
    }

    if (!pathToCreate->empty()) {
        // There were path components which we could not traverse. If 'element' is a nested object
        // which does not contain 'pathToCreate', then this is a no-op (e.g. {$pop: {"a.b.c": 1}}
        // for document {a: {b: {}}}).
        //
        // If the element is an array, but the numeric path does not exist, then this is also a
        // no-op (e.g. {$pop: {"a.2.b": 1}} for document {a: [{b: 0}, {b: 1}]}).
        //
        // Otherwise, this the path contains a blocking leaf or array element, which is an error.
        if (element.getType() == BSONType::Object) {
            *noop = true;
            return;
        }

        size_t arrayIndex;
        if (element.getType() == BSONType::Array &&
            pathsupport::isNumericPathComponent(pathToCreate->getPart(0), &arrayIndex)) {
            *noop = true;
            return;
        }

        uasserted(ErrorCodes::PathNotViable,
                  str::stream() << "Cannot use the part (" << pathToCreate->getPart(0) << ") of ("
                                << pathTaken->dottedField()
                                << "."
                                << pathToCreate->dottedField()
                                << ") to traverse the element ({"
                                << element.toString()
                                << "})");
    }

    invariant(!pathTaken->empty());
    invariant(pathToCreate->empty());

    // The full path existed, but we must fail if the element at that path is not an array.
    invariant(element.ok());
    uassert(ErrorCodes::TypeMismatch,
            str::stream() << "Path '" << pathTaken->dottedField()
                          << "' contains an element of non-array type '"
                          << typeName(element.getType())
                          << "'",
            element.getType() == BSONType::Array);

    if (!element.hasChildren()) {
        // The path exists and contains an array, but the array is empty.
        *noop = true;
        return;
    }

    if (indexData && indexData->mightBeIndexed(pathTaken->dottedField())) {
        *indexesAffected = true;
    }

    auto elementToRemove = _popFromFront ? element.leftChild() : element.rightChild();
    invariantOK(elementToRemove.remove());

    if (logBuilder) {
        uassertStatusOK(logBuilder->addToSetsWithNewFieldName(pathTaken->dottedField(), element));
    }
}

}  // namespace mongo
