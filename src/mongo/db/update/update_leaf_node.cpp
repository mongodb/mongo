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

#include "mongo/db/update/update_leaf_node.h"

#include "mongo/util/stringutils.h"

namespace mongo {

void UpdateLeafNode::checkViability(mutablebson::Element element,
                                    const FieldRef& pathToCreate,
                                    const FieldRef& pathTaken) {
    invariant(!pathToCreate.empty());

    if (element.getType() == BSONType::Object) {
        // 'pathTaken' leads to an object, so we know it will be possible to create 'pathToCreate'
        // at that path.
    } else if (element.getType() == BSONType::Array &&
               parseUnsignedBase10Integer(pathToCreate.getPart(0))) {
        // 'pathTaken' leads to an array, so we know we can add elements at that path so long as the
        // next component is a valid array index. We don't check, but we expect that the index will
        // be out of bounds. (Otherwise it would be part of 'pathTaken' and we wouldn't need to
        // create it.)
    } else {
        uasserted(ErrorCodes::PathNotViable,
                  str::stream() << "Cannot use the part (" << pathToCreate.getPart(0) << ") of ("
                                << pathTaken.dottedField()
                                << "."
                                << pathToCreate.dottedField()
                                << ") to traverse the element ({"
                                << element.toString()
                                << "})");
    }
}

}  // namespace
