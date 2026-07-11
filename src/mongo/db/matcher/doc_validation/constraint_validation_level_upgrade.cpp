// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/doc_validation/constraint_validation_level_upgrade.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/run_aggregate.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/s/query/planner/cluster_aggregate.h"
#include "mongo/util/str.h"

#include <string_view>
#include <vector>

#include <boost/none.hpp>

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

// TODO SERVER-127395: Replace runAggregate/ClusterAggregate::runAggregate with public API calls
// (e.g. CommandHelpers::runCommandDirectly) and pass isSharded as a bool instead of a function.
ValidatorScanFn makeLocalValidatorScanFn(OperationContext* opCtx) {
    return
        [opCtx](AggregateCommandRequest& req, const PrivilegeVector& privs) -> StatusWith<BSONObj> {
            rpc::OpMsgReplyBuilder reply;
            if (auto s = runAggregate(
                    opCtx, req, LiteParsedPipeline{req}, req.toBSON(), privs, boost::none, &reply);
                !s.isOK()) {
                return s;
            }
            auto b = reply.getBodyBuilder();
            CommandHelpers::appendSimpleCommandStatus(b, true);
            b.doneFast();
            return reply.releaseBody();
        };
}

ValidatorScanFn makeClusterValidatorScanFn(OperationContext* opCtx) {
    return [opCtx](AggregateCommandRequest& req,
                   const PrivilegeVector& privs) -> StatusWith<BSONObj> {
        auto ns = req.getNamespace();
        BSONObjBuilder result;
        if (auto s =
                ClusterAggregate::runAggregate(opCtx, {ns, ns}, req, privs, boost::none, &result);
            !s.isOK()) {
            return s;
        }
        return result.obj();
    };
}

Status noDocumentsViolatingValidator(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     PlacementConcern placementConcern,
                                     ValidatorScanFn runAgg) {
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

    PrivilegeVector privs{Privilege(ResourcePattern::forExactNamespace(nss), ActionType::find)};

    auto resultOrStatus = runAgg(aggRequest, privs);
    if (!resultOrStatus.isOK()) {
        return resultOrStatus.getStatus();
    }

    auto cursorResponse = CursorResponse::parseFromBSON(resultOrStatus.getValue(),
                                                        nullptr,
                                                        nss.tenantId(),
                                                        SerializationContext::stateCommandReply());
    if (!cursorResponse.isOK()) {
        return cursorResponse.getStatus();
    }

    const auto& batch = cursorResponse.getValue().getBatch();
    if (batch.empty()) {
        return Status::OK();
    }

    return makeViolatingValidatorStatus(validator, nss.coll(), batch[0]["_id"]);
}

}  // namespace mongo
