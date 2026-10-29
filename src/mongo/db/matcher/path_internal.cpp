// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/path_internal.h"

#include "mongo/bson/bsontypes.h"


namespace mongo {

BSONElement getFieldDottedOrArray(const BSONObj& doc,
                                  const FieldRef& path,
                                  size_t* idxPath,
                                  size_t startIndex) {
    if (path.numParts() == startIndex)
        return doc.getField("");

    BSONElement res;

    BSONObj curr = doc;
    bool stop = false;
    size_t partNum = startIndex;
    while (partNum < path.numParts() && !stop) {
        res = curr.getField(path.getPart(partNum));

        switch (res.type()) {
            case BSONType::eoo:
                stop = true;
                break;

            case BSONType::object:
                curr = res.Obj();
                ++partNum;
                break;

            case BSONType::array:
                stop = true;
                break;

            default:
                if (partNum + 1 < path.numParts()) {
                    res = BSONElement();
                }
                stop = true;
        }
    }

    *idxPath = partNum;
    return res;
}


}  // namespace mongo
