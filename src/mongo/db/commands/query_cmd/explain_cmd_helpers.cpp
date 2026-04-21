/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/commands/query_cmd/explain_cmd_helpers.h"

#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/shard_role/shard_catalog/raw_data_operation.h"
#include "mongo/util/serialization_context.h"

namespace mongo {
namespace explain_cmd_helpers {

ExplainedCommand makeExplainedCommand(OperationContext* opCtx,
                                      const OpMsgRequest& opMsgRequest,
                                      const DatabaseName& dbName,
                                      const BSONObj& explainedObj,
                                      ExplainOptions::Verbosity verbosity,
                                      const SerializationContext& serializationContext) {
    // Extract 'comment' field from the 'explainedObj' only if there is no top-level
    // comment.
    auto commentField = explainedObj["comment"];
    if (!opCtx->getComment() && commentField) {
        std::lock_guard<Client> lk(*opCtx->getClient());
        opCtx->setComment(commentField.wrap());
    }

    if (auto innerDb = explainedObj["$db"]) {
        auto innerDbName = DatabaseNameUtil::deserialize(
            dbName.tenantId(), innerDb.checkAndGetStringData(), serializationContext);
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Mismatched $db in explain command. Expected "
                              << dbName.toStringForErrorMsg() << " but got "
                              << innerDbName.toStringForErrorMsg(),
                innerDbName == dbName);
    }

    auto explainedCommand =
        CommandHelpers::findCommand(opCtx, explainedObj.firstElementFieldNameStringData());
    uassert(ErrorCodes::CommandNotFound,
            str::stream() << "Explain failed due to unknown command: "
                          << explainedObj.firstElementFieldName(),
            explainedCommand);

    auto innerRequest = std::make_unique<OpMsgRequest>(
        OpMsgRequestBuilder::create(opMsgRequest.validatedTenancyScope, dbName, explainedObj));
    auto innerInvocation = explainedCommand->parseForExplain(opCtx, *innerRequest, verbosity);

    uassert(ErrorCodes::InvalidOptions,
            "Command does not support the rawData option",
            !innerInvocation->getGenericArguments().getRawData() ||
                innerInvocation->supportsRawData());
    uassert(ErrorCodes::InvalidOptions,
            "rawData is not enabled",
            !innerInvocation->getGenericArguments().getRawData() ||
                gFeatureFlagRawDataCrudOperations.isEnabled());
    if (innerInvocation->getGenericArguments().getRawData()) {
        isRawDataOperation(opCtx) = true;
    }
    return {std::move(innerRequest), std::move(innerInvocation)};
}

BSONObj makeExplainedObjForMongos(const BSONObj& outerObj, const BSONObj& innerObj) {
    BSONObjBuilder bob;
    bob.appendElements(innerObj);
    for (auto outerElem : outerObj) {
        // If the argument is in both the inner and outer command, we currently let the
        // inner version take precedence.
        const auto name = outerElem.fieldNameStringData();
        // Don't copy $db from the outer explain into the inner command. This is handled later when
        // we make the request for the inner command.
        if (name == "$db"_sd) {
            continue;
        }
        if (isGenericArgument(name) && !innerObj.hasField(name)) {
            bob.append(outerElem);
        }
    }
    return bob.obj();
}

}  // namespace explain_cmd_helpers
}  // namespace mongo
