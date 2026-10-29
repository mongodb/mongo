// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/update/update_leaf_node.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"


namespace mongo {

void UpdateLeafNode::checkViability(mutablebson::Element element,
                                    const FieldRef& pathToCreate,
                                    const FieldRef& pathTaken) {
    invariant(!pathToCreate.empty());

    if (element.getType() == BSONType::object) {
        // 'pathTaken' leads to an object, so we know it will be possible to create 'pathToCreate'
        // at that path.
    } else if (element.getType() == BSONType::array &&
               str::parseUnsignedBase10Integer(pathToCreate.getPart(0))) {
        // 'pathTaken' leads to an array, so we know we can add elements at that path so long as the
        // next component is a valid array index. We don't check, but we expect that the index will
        // be out of bounds. (Otherwise it would be part of 'pathTaken' and we wouldn't need to
        // create it.)
    } else {
        uasserted(ErrorCodes::PathNotViable,
                  str::stream() << "Cannot use the part (" << pathToCreate.getPart(0) << ") of ("
                                << pathTaken.dottedField() << "." << pathToCreate.dottedField()
                                << ") to traverse the element ({" << element.toString() << "})");
    }
}

}  // namespace mongo
