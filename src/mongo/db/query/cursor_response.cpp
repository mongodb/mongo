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

#include "mongo/db/query/cursor_response.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/chunk_version.h"

namespace mongo {

namespace {

const char kCursorField[] = "cursor";
const char kIdField[] = "id";
const char kNsField[] = "ns";
const char kBatchField[] = "nextBatch";
const char kBatchFieldInitial[] = "firstBatch";

}  // namespace

CursorResponseBuilder::CursorResponseBuilder(bool isInitialResponse,
                                             BSONObjBuilder* commandResponse)
    : _responseInitialLen(commandResponse->bb().len()),
      _commandResponse(commandResponse),
      _cursorObject(commandResponse->subobjStart(kCursorField)),
      _batch(_cursorObject.subarrayStart(isInitialResponse ? kBatchFieldInitial : kBatchField)) {}

void CursorResponseBuilder::done(CursorId cursorId, StringData cursorNamespace) {
    invariant(_active);
    _batch.doneFast();
    _cursorObject.append(kIdField, cursorId);
    _cursorObject.append(kNsField, cursorNamespace);
    _cursorObject.doneFast();
    _active = false;
}

void CursorResponseBuilder::abandon() {
    invariant(_active);
    _batch.doneFast();
    _cursorObject.doneFast();
    _commandResponse->bb().setlen(_responseInitialLen);  // Removes everything we've added.
    _active = false;
}

void appendCursorResponseObject(long long cursorId,
                                StringData cursorNamespace,
                                BSONArray firstBatch,
                                BSONObjBuilder* builder) {
    BSONObjBuilder cursorObj(builder->subobjStart(kCursorField));
    cursorObj.append(kIdField, cursorId);
    cursorObj.append(kNsField, cursorNamespace);
    cursorObj.append(kBatchFieldInitial, firstBatch);
    cursorObj.done();
}

void appendGetMoreResponseObject(long long cursorId,
                                 StringData cursorNamespace,
                                 BSONArray nextBatch,
                                 BSONObjBuilder* builder) {
    BSONObjBuilder cursorObj(builder->subobjStart(kCursorField));
    cursorObj.append(kIdField, cursorId);
    cursorObj.append(kNsField, cursorNamespace);
    cursorObj.append(kBatchField, nextBatch);
    cursorObj.done();
}

CursorResponse::CursorResponse(NamespaceString nss,
                               CursorId cursorId,
                               std::vector<BSONObj> batch,
                               boost::optional<long long> numReturnedSoFar)
    : _nss(std::move(nss)),
      _cursorId(cursorId),
      _batch(std::move(batch)),
      _numReturnedSoFar(numReturnedSoFar) {}

StatusWith<CursorResponse> CursorResponse::parseFromBSON(const BSONObj& cmdResponse) {
    Status cmdStatus = getStatusFromCommandResult(cmdResponse);
    if (!cmdStatus.isOK()) {
        if (ErrorCodes::isStaleShardingError(cmdStatus.code())) {
            auto vWanted = ChunkVersion::fromBSON(cmdResponse, "vWanted");
            auto vReceived = ChunkVersion::fromBSON(cmdResponse, "vReceived");
            if (!vWanted.hasEqualEpoch(vReceived)) {
                return Status(ErrorCodes::StaleEpoch, cmdStatus.reason());
            }
        }
        return cmdStatus;
    }

    std::string fullns;
    BSONObj batchObj;
    CursorId cursorId;

    BSONElement cursorElt = cmdResponse[kCursorField];
    if (cursorElt.type() != BSONType::Object) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << "Field '" << kCursorField << "' must be a nested object in: "
                              << cmdResponse};
    }
    BSONObj cursorObj = cursorElt.Obj();

    BSONElement idElt = cursorObj[kIdField];
    if (idElt.type() != BSONType::NumberLong) {
        return {
            ErrorCodes::TypeMismatch,
            str::stream() << "Field '" << kIdField << "' must be of type long in: " << cmdResponse};
    }
    cursorId = idElt.Long();

    BSONElement nsElt = cursorObj[kNsField];
    if (nsElt.type() != BSONType::String) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << "Field '" << kNsField << "' must be of type string in: "
                              << cmdResponse};
    }
    fullns = nsElt.String();

    BSONElement batchElt = cursorObj[kBatchField];
    if (batchElt.eoo()) {
        batchElt = cursorObj[kBatchFieldInitial];
    }

    if (batchElt.type() != BSONType::Array) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << "Must have array field '" << kBatchFieldInitial << "' or '"
                              << kBatchField
                              << "' in: "
                              << cmdResponse};
    }
    batchObj = batchElt.Obj();

    std::vector<BSONObj> batch;
    for (BSONElement elt : batchObj) {
        if (elt.type() != BSONType::Object) {
            return {ErrorCodes::BadValue,
                    str::stream() << "getMore response batch contains a non-object element: "
                                  << elt};
        }

        batch.push_back(elt.Obj());
    }

    for (auto& doc : batch) {
        doc.shareOwnershipWith(cmdResponse);
    }

    return {{NamespaceString(fullns), cursorId, std::move(batch)}};
}

void CursorResponse::addToBSON(CursorResponse::ResponseType responseType,
                               BSONObjBuilder* builder) const {
    BSONObjBuilder cursorBuilder(builder->subobjStart(kCursorField));

    cursorBuilder.append(kIdField, _cursorId);
    cursorBuilder.append(kNsField, _nss.ns());

    const char* batchFieldName =
        (responseType == ResponseType::InitialResponse) ? kBatchFieldInitial : kBatchField;
    BSONArrayBuilder batchBuilder(cursorBuilder.subarrayStart(batchFieldName));
    for (const BSONObj& obj : _batch) {
        batchBuilder.append(obj);
    }
    batchBuilder.doneFast();

    cursorBuilder.doneFast();

    builder->append("ok", 1.0);
}

BSONObj CursorResponse::toBSON(CursorResponse::ResponseType responseType) const {
    BSONObjBuilder builder;
    addToBSON(responseType, &builder);
    return builder.obj();
}

}  // namespace mongo
