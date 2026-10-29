// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/commands/query_cmd/explain_cmd_helpers.h"

#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/shard_role/shard_catalog/raw_data_operation.h"
#include "mongo/util/serialization_context.h"

#include <algorithm>

namespace mongo {
namespace explain_cmd_helpers {
using namespace std::literals::string_view_literals;

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

boost::optional<std::int64_t> resolveMaxTimeMS(boost::optional<std::int64_t> explainMaxTimeMS,
                                               boost::optional<std::int64_t> nestedMaxTimeMS) {
    const bool explainLimits = explainMaxTimeMS && *explainMaxTimeMS > 0;
    const bool nestedLimits = nestedMaxTimeMS && *nestedMaxTimeMS > 0;
    if (explainLimits && nestedLimits) {
        return std::min(*explainMaxTimeMS, *nestedMaxTimeMS);
    }
    if (explainLimits) {
        return explainMaxTimeMS;
    }
    if (nestedLimits) {
        return nestedMaxTimeMS;
    }
    // Neither value imposes a positive limit, so both are 0 ("no limit") or unset. An explicit 0
    // must be preserved (unlike an unset value) so it bypasses defaultMaxTimeMS. Keep the explain
    // value when it is set, otherwise fall back to the nested value (an explicit 0, or unset).
    return explainMaxTimeMS ? explainMaxTimeMS : nestedMaxTimeMS;
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
        if (name == "$db"sv) {
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
