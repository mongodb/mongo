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

#include "mongo/db/query/query_request_helper.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/pipeline/legacy_runtime_constants_gen.h"
#include "mongo/db/query/client_cursor/cursor_response_gen.h"
#include "mongo/db/query/find_command_gen.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/tailable_mode.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cstdint>
#include <memory>
#include <string>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

namespace query_request_helper {
namespace {
/**
 * Add the meta projection to this object if needed.
 */
void addMetaProjection(FindCommandRequest* findCommand) {
    if (findCommand->getShowRecordId()) {
        addShowRecordIdMetaProj(findCommand);
    }
}

}  // namespace

void addShowRecordIdMetaProj(FindCommandRequest* findCommand) {
    if (findCommand->getProjection()["$recordId"]) {
        // There's already some projection on $recordId. Don't overwrite it.
        return;
    }

    BSONObjBuilder projBob;
    projBob.appendElements(findCommand->getProjection());
    BSONObj metaRecordId = BSON("$recordId" << BSON("$meta" << query_request_helper::metaRecordId));
    projBob.append(metaRecordId.firstElement());
    findCommand->setProjection(projBob.obj());
}


Status validateGetMoreCollectionName(StringData collectionName) {
    if (collectionName.empty()) {
        return Status(ErrorCodes::InvalidNamespace, "Collection names cannot be empty");
    }
    if (collectionName[0] == '.') {
        return Status(ErrorCodes::InvalidNamespace,
                      "Collection names cannot start with '.': " + collectionName);
    }
    if (collectionName.find('\0') != std::string::npos) {
        return Status(ErrorCodes::InvalidNamespace,
                      "Collection names cannot have embedded null characters");
    }

    return Status::OK();
}

Status validateResumeInput(OperationContext* opCtx,
                           const mongo::BSONObj& resumeAfter,
                           const mongo::BSONObj& startAt,
                           bool isClusteredCollection) {
    if (resumeAfter.isEmpty() && startAt.isEmpty()) {
        return Status::OK();
    }

    if (!resumeAfter.isEmpty() && !startAt.isEmpty()) {
        return Status(ErrorCodes::BadValue, "Cannot set both '$_resumeAfter' and '$_startAt'");
    }

    auto [resumeInput, resumeInputName] = !resumeAfter.isEmpty()
        ? std::make_pair(mongo::BSONObj(resumeAfter), FindCommandRequest::kResumeAfterFieldName)
        : std::make_pair(mongo::BSONObj(startAt), FindCommandRequest::kStartAtFieldName);

    BSONType recordIdType = resumeInput["$recordId"].type();
    if (resumeInput.nFields() > 2 ||
        (recordIdType != BSONType::numberLong && recordIdType != BSONType::binData &&
         recordIdType != BSONType::null) ||
        (resumeInput.nFields() == 2 &&
         (resumeInput["$initialSyncId"].type() != BSONType::binData ||
          resumeInput["$initialSyncId"].binDataType() != BinDataType::newUUID))) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "Malformed resume token: the '" << resumeInputName
                          << "' object must contain '$recordId', of type NumberLong, BinData "
                             "or jstNULL and optional '$initialSyncId of type BinData.");
    }
    if (resumeInput.hasField("$initialSyncId")) {
        auto initialSyncId = repl::ReplicationCoordinator::get(opCtx)->getInitialSyncId(opCtx);
        auto requestInitialSyncId = uassertStatusOK(UUID::parse(resumeInput["$initialSyncId"]));
        if (!initialSyncId || requestInitialSyncId != *initialSyncId) {
            return Status(ErrorCodes::Error(8132701),
                          "$initialSyncId mismatch, the query is no longer resumable.");
        }
    }

    // Clustered collections can only accept '$_resumeAfter' or '$_startAt' parameter of type
    // BinData. Non clustered collections should only accept '$_resumeAfter' or '$_startAt' of type
    // Long.
    if ((isClusteredCollection && recordIdType == BSONType::numberLong) ||
        (!isClusteredCollection && recordIdType == BSONType::binData)) {
        return Status(ErrorCodes::Error(7738600),
                      str::stream()
                          << "The '" << resumeInputName
                          << "' parameter must match collection type. Clustered "
                             "collections only have BinData recordIds, and all other collections"
                             "have Long recordId.");
    }

    return Status::OK();
}

Status validateFindCommandRequest(const FindCommandRequest& findCommand) {
    // Min and Max objects must have the same fields.
    if (!findCommand.getMin().isEmpty() && !findCommand.getMax().isEmpty()) {
        if (!findCommand.getMin().isFieldNamePrefixOf(findCommand.getMax()) ||
            (findCommand.getMin().nFields() != findCommand.getMax().nFields())) {
            return Status(ErrorCodes::Error(51176), "min and max must have the same field names");
        }
    }

    if (hasInvalidNaturalParam(findCommand.getSort())) {
        return Status(ErrorCodes::BadValue,
                      "$natural sort cannot be set to a value other than -1 or 1.");
    }
    if (hasInvalidNaturalParam(findCommand.getHint())) {
        return Status(ErrorCodes::BadValue,
                      "$natural hint cannot be set to a value other than -1 or 1.");
    }

    if (query_request_helper::getTailableMode(findCommand) != TailableModeEnum::kNormal) {
        // Tailable cursors cannot have any sort other than {$natural: 1}.
        const BSONObj expectedSort = BSON(query_request_helper::kNaturalSortField << 1);
        if (!findCommand.getSort().isEmpty() &&
            SimpleBSONObjComparator::kInstance.evaluate(findCommand.getSort() != expectedSort)) {
            return Status(ErrorCodes::BadValue,
                          "cannot use tailable option with a sort other than {$natural: 1}");
        }

        // Cannot indicate that you want a 'singleBatch' if the cursor is tailable.
        if (findCommand.getSingleBatch()) {
            return Status(ErrorCodes::BadValue,
                          "cannot use tailable option with the 'singleBatch' option");
        }
    }

    if (findCommand.getRequestResumeToken()) {
        if (SimpleBSONObjComparator::kInstance.evaluate(
                findCommand.getHint() != BSON(query_request_helper::kNaturalSortField << 1))) {
            return Status(ErrorCodes::BadValue,
                          "hint must be {$natural:1} if 'requestResumeToken' is enabled");
        }
        if (!findCommand.getSort().isEmpty() &&
            SimpleBSONObjComparator::kInstance.evaluate(
                findCommand.getSort() != BSON(query_request_helper::kNaturalSortField << 1))) {
            return Status(ErrorCodes::BadValue,
                          "sort must be unset or {$natural:1} if 'requestResumeToken' is enabled");
        }
        // The $_resumeAfter/ $_startAt parameter is checked in 'validateResumeInput()'.

    } else if (!findCommand.getResumeAfter().isEmpty()) {
        return Status(ErrorCodes::BadValue,
                      "'requestResumeToken' must be true if 'resumeAfter' is"
                      " specified");
    } else if (!findCommand.getStartAt().isEmpty()) {
        return Status(ErrorCodes::BadValue,
                      "'requestResumeToken' must be true if 'startAt' is"
                      " specified");
    }

    return Status::OK();
}

std::unique_ptr<FindCommandRequest> makeFromFindCommand(
    const BSONObj& cmdObj,
    const boost::optional<auth::ValidatedTenancyScope>& vts,
    const boost::optional<TenantId>& tenantId,
    const SerializationContext& sc) {

    auto findCommand =
        std::make_unique<FindCommandRequest>(idl::parseCommandDocument<FindCommandRequest>(
            cmdObj,
            IDLParserContext("FindCommandRequest", vts, tenantId ? tenantId : boost::none, sc)));

    addMetaProjection(findCommand.get());

    if (findCommand->getSkip() && *findCommand->getSkip() == 0) {
        findCommand->setSkip(boost::none);
    }
    if (findCommand->getLimit() && *findCommand->getLimit() == 0) {
        findCommand->setLimit(boost::none);
    }
    uassertStatusOK(validateFindCommandRequest(*findCommand));

    return findCommand;
}

std::unique_ptr<FindCommandRequest> makeFromFindCommandForTests(
    const BSONObj& cmdObj, boost::optional<NamespaceString> nss) {
    return makeFromFindCommand(cmdObj,
                               boost::none /*vts*/,
                               nss ? nss->tenantId() : boost::none,
                               SerializationContext::stateDefault());
}

bool isTextScoreMeta(BSONElement elt) {
    // elt must be foo: {$meta: "textScore"}
    if (BSONType::object != elt.type()) {
        return false;
    }
    BSONObj metaObj = elt.Obj();
    BSONObjIterator metaIt(metaObj);
    // must have exactly 1 element
    if (!metaIt.more()) {
        return false;
    }
    BSONElement metaElt = metaIt.next();
    if (metaElt.fieldNameStringData() != "$meta") {
        return false;
    }
    if (BSONType::string != metaElt.type()) {
        return false;
    }
    if (metaElt.valueStringData() != metaTextScore) {
        return false;
    }
    // must have exactly 1 element
    if (metaIt.more()) {
        return false;
    }
    return true;
}

void setTailableMode(TailableModeEnum tailableMode, FindCommandRequest* findCommand) {
    if (tailableMode == TailableModeEnum::kTailableAndAwaitData) {
        findCommand->setAwaitData(true);
        findCommand->setTailable(true);
    } else if (tailableMode == TailableModeEnum::kTailable) {
        findCommand->setTailable(true);
    }
}

TailableModeEnum getTailableMode(const FindCommandRequest& findCommand) {
    return uassertStatusOK(
        tailableModeFromBools(findCommand.getTailable(), findCommand.getAwaitData()));
}

void validateCursorResponse(const BSONObj& outputAsBson,
                            const boost::optional<auth::ValidatedTenancyScope>& vts,
                            boost::optional<TenantId> tenantId,
                            const SerializationContext& serializationContext) {
    if (getTestCommandsEnabled()) {
        CursorInitialReply::parse(
            outputAsBson,
            IDLParserContext("CursorInitialReply",
                             vts,
                             tenantId,
                             SerializationContext::stateCommandReply(serializationContext)));
    }
}

bool hasInvalidNaturalParam(const BSONObj& obj) {
    if (!obj.hasElement(query_request_helper::kNaturalSortField)) {
        return false;
    }
    auto naturalElem = obj[query_request_helper::kNaturalSortField];
    if (!naturalElem.isNumber()) {
        return true;
    }
    if (obj.woCompare(BSON(query_request_helper::kNaturalSortField << 1)) == 0 ||
        obj.woCompare(BSON(query_request_helper::kNaturalSortField << -1)) == 0) {
        return false;
    }

    return true;
}

long long getDefaultBatchSize() {
    return internalQueryFindCommandBatchSize.load();
}

}  // namespace query_request_helper
}  // namespace mongo
