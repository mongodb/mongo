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


#include "mongo/db/query/client_cursor/cursor_response.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/query/client_cursor/cursor_response_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/type_traits/decay.hpp>

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
const char kWasStatementExecuted[] = "$_wasStatementExecuted";
const char kMetricsField[] = "metrics";
const char kExplainField[] = "explain";

}  // namespace

CursorResponseBuilder::CursorResponseBuilder(rpc::ReplyBuilderInterface* replyBuilder,
                                             Options options = Options())
    : _options(options), _replyBuilder(replyBuilder) {
    _bodyBuilder.emplace(_replyBuilder->getBodyBuilder());
    _cursorObject.emplace(_bodyBuilder->subobjStart(kCursorField));
    _batch.emplace(_cursorObject->subarrayStart(_options.isInitialResponse ? kBatchFieldInitial
                                                                           : kBatchField));
}

void CursorResponseBuilder::done(CursorId cursorId,
                                 const NamespaceString& cursorNamespace,
                                 boost::optional<CursorMetrics> metrics,
                                 const SerializationContext& serializationContext) {
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

    if (_wasStatementExecuted) {
        _cursorObject->append(kWasStatementExecuted, _wasStatementExecuted);
    }

    _cursorObject->append(kIdField, cursorId);
    _cursorObject->append(kNsField,
                          NamespaceStringUtil::serialize(cursorNamespace, serializationContext));
    if (_options.atClusterTime) {
        _cursorObject->append(kAtClusterTimeField, _options.atClusterTime->asTimestamp());
    }

    if (metrics) {
        _cursorObject->append(kMetricsField, metrics->toBSON());
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
                                BSONObjBuilder* builder,
                                const SerializationContext& serializationContext) {
    BSONObjBuilder cursorObj(builder->subobjStart(kCursorField));
    cursorObj.append(kIdField, cursorId);
    cursorObj.append(kNsField,
                     NamespaceStringUtil::serialize(cursorNamespace, serializationContext));
    cursorObj.append(kBatchFieldInitial, firstBatch);
    if (cursorType) {
        cursorObj.append(kTypeField, cursorType.value());
    }
    cursorObj.done();
}

CursorResponse::CursorResponse(NamespaceString nss,
                               CursorId cursorId,
                               std::vector<BSONObj> batch,
                               boost::optional<Timestamp> atClusterTime,
                               boost::optional<BSONObj> postBatchResumeToken,
                               boost::optional<BSONObj> writeConcernError,
                               boost::optional<BSONObj> varsField,
                               boost::optional<BSONObj> explain,
                               boost::optional<CursorTypeEnum> cursorType,
                               boost::optional<CursorMetrics> metrics,
                               bool partialResultsReturned,
                               bool invalidated,
                               bool wasStatementExecuted)
    : _nss(std::move(nss)),
      _cursorId(cursorId),
      _batch(std::move(batch)),
      _atClusterTime(std::move(atClusterTime)),
      _postBatchResumeToken(std::move(postBatchResumeToken)),
      _writeConcernError(std::move(writeConcernError)),
      _varsField(std::move(varsField)),
      _explain(std::move(explain)),
      _cursorType(std::move(cursorType)),
      _metrics(std::move(metrics)),
      _partialResultsReturned(partialResultsReturned),
      _invalidated(invalidated),
      _wasStatementExecuted(wasStatementExecuted) {}

std::vector<StatusWith<CursorResponse>> CursorResponse::parseFromBSONMany(
    const BSONObj& cmdResponse) {
    std::vector<StatusWith<CursorResponse>> cursors;
    BSONElement cursorsElt = cmdResponse[kCursorsField];

    // If there is not "cursors" array then treat it as a single cursor response
    if (cursorsElt.type() != BSONType::array) {
        cursors.push_back(parseFromBSON(cmdResponse));
    } else {
        BSONObj cursorsObj = cursorsElt.embeddedObject();
        for (BSONElement elt : cursorsObj) {
            if (elt.type() != BSONType::object) {
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

StatusWith<CursorResponse> CursorResponse::parseFromBSON(
    const BSONObj& cmdResponse,
    const BSONObj* ownedObj,
    boost::optional<TenantId> tenantId,
    const SerializationContext& serializationContext) {
    Status cmdStatus = getStatusFromCommandResult(cmdResponse);
    if (!cmdStatus.isOK()) {
        return cmdStatus;
    }

    AnyCursorResponse response;
    try {
        const auto vts = tenantId
            ? boost::make_optional(auth::ValidatedTenancyScopeFactory::create(
                  *tenantId, auth::ValidatedTenancyScopeFactory::TrustedForInnerOpMsgRequestTag{}))
            : boost::none;
        IDLParserContext idlCtx("CursorResponse", vts, tenantId, serializationContext);
        response = AnyCursorResponse::parse(cmdResponse, idlCtx);
    } catch (const DBException& e) {
        return e.toStatus();
    }

    // Take non-const references to the cursor data here so that we do not need to copy it.
    auto& cursor = response.getCursor();
    auto& maybeBatch = cursor.getFirstBatch() ? cursor.getFirstBatch() : cursor.getNextBatch();

    // IDL verifies that exactly one of these fields is present
    tassert(
        8362700, "CursorResponse cursor must contain one of firstBatch or nextBatch", maybeBatch);
    auto& batch = *maybeBatch;

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

    auto metrics = cursor.getMetrics();

    auto getOwnedBSONObj = [](const boost::optional<BSONObj>& unownedObj) {
        return unownedObj ? unownedObj->getOwned() : unownedObj;
    };

    return {{cursor.getNs(),
             cursor.getCursorId(),
             std::move(batch),
             cursor.getAtClusterTime(),
             getOwnedBSONObj(cursor.getPostBatchResumeToken()),
             getOwnedBSONObj(response.getWriteConcernError()),
             getOwnedBSONObj(response.getVars()),
             getOwnedBSONObj(response.getExplain()),
             cursor.getCursorType(),
             std::move(metrics),
             cursor.getPartialResultsReturned(),
             cursor.getInvalidated(),
             cursor.getWasStatementExecuted()}};
}

void CursorResponse::addToBSON(CursorResponse::ResponseType responseType,
                               BSONObjBuilder* builder,
                               const SerializationContext& serializationContext) const {
    BSONObjBuilder cursorBuilder(builder->subobjStart(kCursorField));

    cursorBuilder.append(kIdField, _cursorId);
    cursorBuilder.append(kNsField, NamespaceStringUtil::serialize(_nss, serializationContext));

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

    if (_wasStatementExecuted) {
        cursorBuilder.append(kWasStatementExecuted, _wasStatementExecuted);
    }

    if (_metrics) {
        cursorBuilder.append(kMetricsField, _metrics->toBSON());
    }

    cursorBuilder.doneFast();

    builder->append("ok", 1.0);

    if (_writeConcernError) {
        builder->append("writeConcernError", *_writeConcernError);
    }
}

BSONObj CursorResponse::toBSON(CursorResponse::ResponseType responseType,
                               const SerializationContext& serializationContext) const {
    BSONObjBuilder builder;
    addToBSON(responseType, &builder, serializationContext);
    return builder.obj();
}

}  // namespace mongo
