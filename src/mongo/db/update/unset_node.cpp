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

#include "mongo/db/update/unset_node.h"

namespace mongo {

Status UnsetNode::init(BSONElement modExpr, const CollatorInterface* collator) {
    // Note that we don't need to store modExpr, because $unset does not do anything with its value.
    invariant(modExpr.ok());
    return Status::OK();
}

Status UnsetNode::apply(mutablebson::Element element,
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

    if (!pathToCreate->empty()) {
        // A non-empty "pathToCreate" implies that our search did not find the field that we wanted
        // to delete. We employ a simple and efficient strategy for deleting fields that don't yet
        // exist.
        *noop = true;
        return Status::OK();
    }

    // Determine if indexes are affected.
    if (indexData && indexData->mightBeIndexed(pathTaken->dottedField())) {
        *indexesAffected = true;
    }

    auto parent = element.parent();
    invariant(parent.ok());
    if (!parent.isType(BSONType::Array)) {
        invariantOK(element.remove());
    } else {
        // Special case: An $unset on an array element sets it to null instead of removing it from
        // the array.
        invariantOK(element.setValueNull());
    }

    // Log the unset.
    if (logBuilder) {
        return logBuilder->addToUnsets(pathTaken->dottedField());
    }

    return Status::OK();
}

}  // namespace mongo
