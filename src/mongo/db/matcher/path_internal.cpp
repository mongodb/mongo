// path_internal.h

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

#include "mongo/db/matcher/path_internal.h"

namespace mongo {

bool isAllDigits(StringData str) {
    for (unsigned i = 0; i < str.size(); i++) {
        if (!isdigit(str[i]))
            return false;
    }
    return true;
}

BSONElement getFieldDottedOrArray(const BSONObj& doc, const FieldRef& path, size_t* idxPath) {
    if (path.numParts() == 0)
        return doc.getField("");

    BSONElement res;

    BSONObj curr = doc;
    bool stop = false;
    size_t partNum = 0;
    while (partNum < path.numParts() && !stop) {
        res = curr.getField(path.getPart(partNum));

        switch (res.type()) {
            case EOO:
                stop = true;
                break;

            case Object:
                curr = res.Obj();
                ++partNum;
                break;

            case Array:
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
