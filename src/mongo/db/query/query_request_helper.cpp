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

#include "mongo/db/query/query_request_helper.h"

#include <memory>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/dbmessage.h"

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
    if (resumeInput.nFields() != 1 ||
        (recordIdType != BSONType::NumberLong && recordIdType != BSONType::BinData &&
         recordIdType != BSONType::jstNULL)) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Malformed resume token: the '" << resumeInputName
                                    << "' object must contain exactly one field named '$recordId', "
                                       "of type NumberLong, BinData or jstNULL.");
    }

    // Clustered collections can only accept '$_resumeAfter' or '$_startAt' parameter of type
    // BinData. Non clustered collections should only accept '$_resumeAfter' or '$_startAt' of type
    // Long.
    if ((isClusteredCollection && recordIdType == BSONType::NumberLong) ||
        (!isClusteredCollection && recordIdType == BSONType::BinData)) {
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

std::unique_ptr<FindCommandRequest> makeFromFindCommand(const BSONObj& cmdObj,
                                                        boost::optional<NamespaceString> nss,
                                                        bool apiStrict) {
    auto findCommand = std::make_unique<FindCommandRequest>(FindCommandRequest::parse(
        IDLParserContext("FindCommandRequest", apiStrict, nss ? nss->tenantId() : boost::none),
        cmdObj));

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
    const BSONObj& cmdObj, boost::optional<NamespaceString> nss, bool apiStrict) {
    return makeFromFindCommand(cmdObj, nss, apiStrict);
}

bool isTextScoreMeta(BSONElement elt) {
    // elt must be foo: {$meta: "textScore"}
    if (mongo::Object != elt.type()) {
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
    if (mongo::String != metaElt.type()) {
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

void validateCursorResponse(const BSONObj& outputAsBson, boost::optional<TenantId> tenantId) {
    if (getTestCommandsEnabled()) {
        CursorInitialReply::parse(
            IDLParserContext("CursorInitialReply", false /* apiStrict */, tenantId), outputAsBson);
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

}  // namespace query_request_helper
}  // namespace mongo
