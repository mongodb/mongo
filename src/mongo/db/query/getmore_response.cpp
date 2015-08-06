/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/query/getmore_response.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/rpc/get_status_from_command_result.h"

namespace mongo {

namespace {

const char kCursorField[] = "cursor";
const char kIdField[] = "id";
const char kNsField[] = "ns";
const char kBatchField[] = "nextBatch";
const char kBatchFieldAlt[] = "firstBatch";

}  // namespace

GetMoreResponse::GetMoreResponse(NamespaceString namespaceString,
                                 CursorId id,
                                 std::vector<BSONObj> objs)
    : nss(std::move(namespaceString)), cursorId(id), batch(std::move(objs)) {}

StatusWith<GetMoreResponse> GetMoreResponse::parseFromBSON(const BSONObj& cmdResponse) {
    Status cmdStatus = getStatusFromCommandResult(cmdResponse);
    if (!cmdStatus.isOK()) {
        return cmdStatus;
    }

    std::string fullns;
    BSONObj batchObj;
    CursorId cursorId;

    BSONElement cursorElt = cmdResponse[kCursorField];
    if (cursorElt.type() != BSONType::Object) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << "Field '" << kCursorField
                              << "' must be a nested object in: " << cmdResponse};
    }
    BSONObj cursorObj = cursorElt.Obj();

    BSONElement idElt = cursorObj[kIdField];
    if (idElt.type() != BSONType::NumberLong) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << "Field '" << kIdField
                              << "' must be of type long in: " << cmdResponse};
    }
    cursorId = idElt.Long();

    BSONElement nsElt = cursorObj[kNsField];
    if (nsElt.type() != BSONType::String) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << "Field '" << kNsField
                              << "' must be of type string in: " << cmdResponse};
    }
    fullns = nsElt.String();

    BSONElement batchElt = cursorObj[kBatchField];
    if (batchElt.eoo()) {
        batchElt = cursorObj[kBatchFieldAlt];
    }

    if (batchElt.type() != BSONType::Array) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << "Must have array field '" << kBatchFieldAlt << "' or '"
                              << kBatchField << "' in: " << cmdResponse};
    }
    batchObj = batchElt.Obj();

    std::vector<BSONObj> batch;
    for (BSONElement elt : batchObj) {
        if (elt.type() != BSONType::Object) {
            return {
                ErrorCodes::BadValue,
                str::stream() << "getMore response batch contains a non-object element: " << elt};
        }

        batch.push_back(elt.Obj().getOwned());
    }

    return {{NamespaceString(fullns), cursorId, batch}};
}

void GetMoreResponse::toBSON(BSONObjBuilder* builder) const {
    BSONObjBuilder cursorBuilder(builder->subobjStart(kCursorField));

    cursorBuilder.append(kIdField, cursorId);
    cursorBuilder.append(kNsField, nss.ns());

    BSONArrayBuilder batchBuilder(cursorBuilder.subarrayStart(kBatchField));
    for (const BSONObj& obj : batch) {
        batchBuilder.append(obj);
    }
    batchBuilder.doneFast();

    cursorBuilder.doneFast();

    builder->append("ok", 1.0);
}

BSONObj GetMoreResponse::toBSON() const {
    BSONObjBuilder builder;
    toBSON(&builder);
    return builder.obj();
}

}  // namespace mongo
