// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/doc_validation/constraint_validation_level_upgrade.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/commands.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/str.h"

#include <string_view>
#include <vector>

namespace mongo {
namespace {

Status makeViolatingValidatorStatus(const BSONObj& validator,
                                    std::string_view collName,
                                    const BSONElement& offendingId) {
    str::stream msg;
    msg << "Cannot upgrade validationLevel to 'constraint': the collection contains documents "
           "that do not satisfy the validator.";
    if (!offendingId.eoo()) {
        msg << " First offending document _id: "
            << offendingId.toString(/*includeFieldName=*/false);
    }
    constexpr size_t kMaxValidatorInErrorMessage = 10 * 1024;
    StringBuilder validatorStr;
    validator.toString(validatorStr, /*isArray=*/false, /*full=*/true);
    auto validatorStrMaterialized =
        static_cast<size_t>(validatorStr.len()) < kMaxValidatorInErrorMessage
        ? validatorStr.str()
        : "<your collection's validator>";
    msg << " Run db." << collName << ".find({\"$nor\": [" << validatorStrMaterialized
        << "]}) to find non-compliant documents.";
    return {ErrorCodes::Error(12370902), msg};
}

}  // namespace

Status noDocumentsViolatingValidator(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     PlacementConcern placementConcern,
                                     bool localOnly) {
    BSONObj validator;
    {
        auto coll =
            acquireCollection(opCtx,
                              CollectionAcquisitionRequest(nss,
                                                           std::move(placementConcern),
                                                           repl::ReadConcernArgs::get(opCtx),
                                                           AcquisitionPrerequisites::kRead),
                              MODE_IS);
        if (!coll.exists()) {
            return Status::OK();
        }
        // If the collection is already at 'constraint' level, all existing documents must already
        // conform — the constraint level rejects any write that would violate the validator — so
        // there is nothing to scan.
        if (coll.getCollectionPtr()->getValidationLevel() == ValidationLevelEnum::constraint) {
            return Status::OK();
        }
        validator = coll.getCollectionPtr()->getCollectionOptions().validator.getOwned();
    }

    if (validator.isEmpty()) {
        // Empty validator matches all documents.
        return Status::OK();
    }

    std::vector<BSONObj> pipeline = {
        BSON("$match" << BSON("$nor" << BSON_ARRAY(validator))),
        BSON("$limit" << 1),
    };

    AggregateCommandRequest aggRequest(nss, std::move(pipeline));
    aggRequest.setHint(BSON("$natural" << 1));

    // For sharded collections "clusterAggregate" fans out to all shards; otherwise use local
    // "aggregate". appendElementsRenamed renames the first field and carries over the rest.
    auto aggBSON = aggRequest.toBSON();
    BSONObj cmdObj = localOnly
        ? aggBSON
        : BSONObjBuilder().appendElementsRenamed(aggBSON, BSON("clusterAggregate" << 1)).obj();

    auto request =
        OpMsgRequestBuilder::create(auth::ValidatedTenancyScope::get(opCtx), nss.dbName(), cmdObj);
    auto result = CommandHelpers::runCommandDirectly(opCtx, request);

    auto status = getStatusFromCommandResult(result);
    if (!status.isOK()) {
        return status;
    }

    auto cursorResponse = CursorResponse::parseFromBSON(
        result, nullptr, nss.tenantId(), SerializationContext::stateCommandReply());
    if (!cursorResponse.isOK()) {
        return cursorResponse.getStatus();
    }
    const auto& cursor = cursorResponse.getValue();
    if (cursor.getCursorId() != 0) {
        return Status(ErrorCodes::InternalError,
                      "validator scan cursor was not exhausted after $limit 1");
    }
    if (cursor.getBatch().empty()) {
        return Status::OK();
    }

    return makeViolatingValidatorStatus(validator, nss.coll(), cursor.getBatch()[0]["_id"]);
}

}  // namespace mongo
