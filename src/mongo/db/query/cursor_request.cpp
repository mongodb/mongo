/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/query/cursor_request.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"

namespace mongo {

Status CursorRequest::parseCommandCursorOptions(const BSONObj& cmdObj,
                                                long long defaultBatchSize,
                                                long long* batchSize) {
    invariant(batchSize);
    *batchSize = defaultBatchSize;

    BSONElement cursorElem = cmdObj["cursor"];
    if (cursorElem.eoo()) {
        return Status::OK();
    }

    if (cursorElem.type() != mongo::Object) {
        return Status(ErrorCodes::TypeMismatch, "cursor field must be missing or an object");
    }

    BSONObj cursor = cursorElem.embeddedObject();
    BSONElement batchSizeElem = cursor["batchSize"];

    const int expectedNumberOfCursorFields = batchSizeElem.eoo() ? 0 : 1;
    if (cursor.nFields() != expectedNumberOfCursorFields) {
        return Status(ErrorCodes::BadValue,
                      "cursor object can't contain fields other than batchSize");
    }

    if (batchSizeElem.eoo()) {
        return Status::OK();
    }

    if (!batchSizeElem.isNumber()) {
        return Status(ErrorCodes::TypeMismatch, "cursor.batchSize must be a number");
    }

    // This can change in the future, but for now all negatives are reserved.
    if (batchSizeElem.numberLong() < 0) {
        return Status(ErrorCodes::BadValue, "cursor.batchSize must not be negative");
    }

    *batchSize = batchSizeElem.numberLong();

    return Status::OK();
}

}  // namespace mongo
