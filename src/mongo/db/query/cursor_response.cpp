/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
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

namespace mongo {

namespace {

const char kCursorsField[] = "cursors";
const char kCursorField[] = "cursor";
const char kIdField[] = "id";
const char kNsField[] = "ns";
const char kBatchField[] = "nextBatch";
const char kBatchFieldInitial[] = "firstBatch";
const char kBatchDocSequenceField[] = "cursor.nextBatch";
const char kBatchDocSequenceFieldInitial[] = "cursor.firstBatch";
const char kPostBatchResumeTokenField[] = "postBatchResumeToken";

}  // namespace

CursorResponseBuilder::CursorResponseBuilder(rpc::ReplyBuilderInterface* replyBuilder,
                                             Options options = Options())
    : _options(options), _replyBuilder(replyBuilder) {
    if (_options.useDocumentSequences) {
        _docSeqBuilder.emplace(_replyBuilder->getDocSequenceBuilder(
            _options.isInitialResponse ? kBatchDocSequenceFieldInitial : kBatchDocSequenceField));
    } else {
        _bodyBuilder.emplace(_replyBuilder->getBodyBuilder());
        _cursorObject.emplace(_bodyBuilder->subobjStart(kCursorField));
        _batch.emplace(_cursorObject->subarrayStart(_options.isInitialResponse ? kBatchFieldInitial
                                                                               : kBatchField));
    }
}

void CursorResponseBuilder::done(CursorId cursorId, StringData cursorNamespace) {
    invariant(_active);
    if (_options.useDocumentSequences) {
        _docSeqBuilder.reset();
        _bodyBuilder.emplace(_replyBuilder->getBodyBuilder());
        _cursorObject.emplace(_bodyBuilder->subobjStart(kCursorField));
    } else {
        _batch.reset();
    }
    if (!_postBatchResumeToken.isEmpty()) {
        _cursorObject->append(kPostBatchResumeTokenField, _postBatchResumeToken);
    }
    _cursorObject->append(kIdField, cursorId);
    _cursorObject->append(kNsField, cursorNamespace);
    _cursorObject.reset();

    _bodyBuilder.reset();
    _active = false;
}

void CursorResponseBuilder::abandon() {
    invariant(_active);
    _batch.reset();
    _cursorObject.reset();
    _bodyBuilder.reset();
    _replyBuilder->reset();
    _numDocs = 0;
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
                               boost::optional<long long> numReturnedSoFar,
                               boost::optional<BSONObj> postBatchResumeToken,
                               boost::optional<BSONObj> writeConcernError)
    : _nss(std::move(nss)),
      _cursorId(cursorId),
      _batch(std::move(batch)),
      _numReturnedSoFar(numReturnedSoFar),
      _postBatchResumeToken(std::move(postBatchResumeToken)),
      _writeConcernError(std::move(writeConcernError)) {}

std::vector<StatusWith<CursorResponse>> CursorResponse::parseFromBSONMany(
    const BSONObj& cmdResponse) {
    std::vector<StatusWith<CursorResponse>> cursors;
    BSONElement cursorsElt = cmdResponse[kCursorsField];

    // If there is not "cursors" array then treat it as a single cursor response
    if (cursorsElt.type() != BSONType::Array) {
        cursors.push_back(parseFromBSON(cmdResponse));
    } else {
        BSONObj cursorsObj = cursorsElt.embeddedObject();
        for (BSONElement elt : cursorsObj) {
            if (elt.type() != BSONType::Object) {
                cursors.push_back({ErrorCodes::BadValue,
                                   str::stream()
                                       << "Cursors array element contains non-object element: "
                                       << elt});
            } else {
                cursors.push_back(parseFromBSON(elt.Obj()));
            }
        }
    }

    return cursors;
}

StatusWith<CursorResponse> CursorResponse::parseFromBSON(const BSONObj& cmdResponse) {
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
        batchElt = cursorObj[kBatchFieldInitial];
    }

    if (batchElt.type() != BSONType::Array) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << "Must have array field '" << kBatchFieldInitial << "' or '"
                              << kBatchField << "' in: " << cmdResponse};
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

    auto postBatchResumeTokenElem = cursorObj[kPostBatchResumeTokenField];
    if (postBatchResumeTokenElem && postBatchResumeTokenElem.type() != BSONType::Object) {
        return {ErrorCodes::BadValue,
                str::stream() << kPostBatchResumeTokenField
                              << " format is invalid; expected Object, but found: "
                              << postBatchResumeTokenElem.type()};
    }

    auto writeConcernError = cmdResponse["writeConcernError"];

    if (writeConcernError && writeConcernError.type() != BSONType::Object) {
        return {ErrorCodes::BadValue,
                str::stream() << "invalid writeConcernError format; expected object but found: "
                              << writeConcernError.type()};
    }

    return {{NamespaceString(fullns),
             cursorId,
             std::move(batch),
             boost::none,
             postBatchResumeTokenElem ? postBatchResumeTokenElem.Obj().getOwned()
                                      : boost::optional<BSONObj>{},
             writeConcernError ? writeConcernError.Obj().getOwned() : boost::optional<BSONObj>{}}};
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

    if (_postBatchResumeToken && !_postBatchResumeToken->isEmpty()) {
        cursorBuilder.append(kPostBatchResumeTokenField, *_postBatchResumeToken);
    }

    cursorBuilder.doneFast();

    builder->append("ok", 1.0);

    if (_writeConcernError) {
        builder->append("writeConcernError", *_writeConcernError);
    }
}

BSONObj CursorResponse::toBSON(CursorResponse::ResponseType responseType) const {
    BSONObjBuilder builder;
    addToBSON(responseType, &builder);
    return builder.obj();
}

}  // namespace mongo
