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


#include "mongo/platform/basic.h"

#include "mongo/db/query/cursor_response.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/namespace_string_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

namespace {

const char kCursorsField[] = "cursors";
const char kCursorField[] = "cursor";
const char kIdField[] = "id";
const char kNsField[] = "ns";
const char kVarsField[] = "vars";
const char kTypeField[] = "type";
const char kAtClusterTimeField[] = "atClusterTime";
const char kBatchField[] = "nextBatch";
const char kBatchFieldInitial[] = "firstBatch";
const char kBatchDocSequenceField[] = "cursor.nextBatch";
const char kBatchDocSequenceFieldInitial[] = "cursor.firstBatch";
const char kPostBatchResumeTokenField[] = "postBatchResumeToken";
const char kPartialResultsReturnedField[] = "partialResultsReturned";
const char kInvalidatedField[] = "invalidated";

}  // namespace

CursorResponseBuilder::CursorResponseBuilder(rpc::ReplyBuilderInterface* replyBuilder,
                                             Options options = Options())
    : _options(options), _replyBuilder(replyBuilder) {
    _bodyBuilder.emplace(_replyBuilder->getBodyBuilder());
    _cursorObject.emplace(_bodyBuilder->subobjStart(kCursorField));
    _batch.emplace(_cursorObject->subarrayStart(_options.isInitialResponse ? kBatchFieldInitial
                                                                           : kBatchField));
}

void CursorResponseBuilder::done(CursorId cursorId, const NamespaceString& cursorNamespace) {
    invariant(_active);

    _batch.reset();
    if (!_postBatchResumeToken.isEmpty()) {
        _cursorObject->append(kPostBatchResumeTokenField, _postBatchResumeToken);
    }
    if (_partialResultsReturned) {
        _cursorObject->append(kPartialResultsReturnedField, true);
    }

    if (_invalidated) {
        _cursorObject->append(kInvalidatedField, _invalidated);
    }

    _cursorObject->append(kIdField, cursorId);
    _cursorObject->append(kNsField, NamespaceStringUtil::serialize(cursorNamespace));
    if (_options.atClusterTime) {
        _cursorObject->append(kAtClusterTimeField, _options.atClusterTime->asTimestamp());
    }
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
                                const NamespaceString& cursorNamespace,
                                BSONArray firstBatch,
                                boost::optional<StringData> cursorType,
                                BSONObjBuilder* builder) {
    BSONObjBuilder cursorObj(builder->subobjStart(kCursorField));
    cursorObj.append(kIdField, cursorId);
    cursorObj.append(kNsField, NamespaceStringUtil::serialize(cursorNamespace));
    cursorObj.append(kBatchFieldInitial, firstBatch);
    if (cursorType) {
        cursorObj.append(kTypeField, cursorType.value());
    }
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
                               boost::optional<Timestamp> atClusterTime,
                               boost::optional<BSONObj> postBatchResumeToken,
                               boost::optional<BSONObj> writeConcernError,
                               boost::optional<BSONObj> varsField,
                               boost::optional<std::string> cursorType,
                               bool partialResultsReturned,
                               bool invalidated)
    : _nss(std::move(nss)),
      _cursorId(cursorId),
      _batch(std::move(batch)),
      _atClusterTime(std::move(atClusterTime)),
      _postBatchResumeToken(std::move(postBatchResumeToken)),
      _writeConcernError(std::move(writeConcernError)),
      _varsField(std::move(varsField)),
      _cursorType(std::move(cursorType)),
      _partialResultsReturned(partialResultsReturned),
      _invalidated(invalidated) {}

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
                cursors.push_back(parseFromBSON(elt.Obj(), &cmdResponse));
            }
        }
    }

    return cursors;
}

StatusWith<CursorResponse> CursorResponse::parseFromBSON(const BSONObj& cmdResponse,
                                                         const BSONObj* ownedObj) {
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

    BSONElement varsElt = cmdResponse[kVarsField];
    if (!varsElt.eoo() && varsElt.type() != BSONType::Object) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << "Field '" << kVarsField
                              << "' must be of type object in: " << cmdResponse};
    }

    BSONElement typeElt = cursorObj[kTypeField];
    if (!typeElt.eoo() && typeElt.type() != BSONType::String) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << "Field '" << kTypeField << "' must be of type string but got "
                              << typeElt.type() << " in: " << cmdResponse};
    }

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

    tassert(6253102,
            "Must own one of the two arguments if there are documents in the batch",
            batch.size() == 0 || cmdResponse.isOwned() || (ownedObj && ownedObj->isOwned()));

    for (auto& doc : batch) {
        if (ownedObj) {
            doc.shareOwnershipWith(*ownedObj);
        } else {
            doc.shareOwnershipWith(cmdResponse);
        }
    }

    auto postBatchResumeTokenElem = cursorObj[kPostBatchResumeTokenField];
    if (postBatchResumeTokenElem && postBatchResumeTokenElem.type() != BSONType::Object) {
        return {ErrorCodes::BadValue,
                str::stream() << kPostBatchResumeTokenField
                              << " format is invalid; expected Object, but found: "
                              << postBatchResumeTokenElem.type()};
    }

    auto atClusterTimeElem = cursorObj[kAtClusterTimeField];
    if (atClusterTimeElem && atClusterTimeElem.type() != BSONType::bsonTimestamp) {
        return {ErrorCodes::BadValue,
                str::stream() << kAtClusterTimeField
                              << " format is invalid; expected Timestamp, but found: "
                              << atClusterTimeElem.type()};
    }

    auto partialResultsReturned = cursorObj[kPartialResultsReturnedField];

    if (partialResultsReturned) {
        if (partialResultsReturned.type() != BSONType::Bool) {
            return {ErrorCodes::BadValue,
                    str::stream() << kPartialResultsReturnedField
                                  << " format is invalid; expected Bool, but found: "
                                  << partialResultsReturned.type()};
        }
    }

    auto invalidatedElem = cursorObj[kInvalidatedField];
    if (invalidatedElem) {
        if (invalidatedElem.type() != BSONType::Bool) {
            return {ErrorCodes::BadValue,
                    str::stream() << kInvalidatedField
                                  << " format is invalid; expected Bool, but found: "
                                  << invalidatedElem.type()};
        }
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
             atClusterTimeElem ? atClusterTimeElem.timestamp() : boost::optional<Timestamp>{},
             postBatchResumeTokenElem ? postBatchResumeTokenElem.Obj().getOwned()
                                      : boost::optional<BSONObj>{},
             writeConcernError ? writeConcernError.Obj().getOwned() : boost::optional<BSONObj>{},
             varsElt ? varsElt.Obj().getOwned() : boost::optional<BSONObj>{},
             typeElt ? boost::make_optional<std::string>(typeElt.String()) : boost::none,
             partialResultsReturned.trueValue(),
             invalidatedElem.trueValue()}};
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

    if (_atClusterTime) {
        cursorBuilder.append(kAtClusterTimeField, *_atClusterTime);
    }

    if (_partialResultsReturned) {
        cursorBuilder.append(kPartialResultsReturnedField, true);
    }

    if (_invalidated) {
        cursorBuilder.append(kInvalidatedField, _invalidated);
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
