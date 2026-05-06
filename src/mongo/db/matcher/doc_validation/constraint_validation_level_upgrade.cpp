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

#include "mongo/db/matcher/doc_validation/constraint_validation_level_upgrade.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/run_aggregate.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/util/str.h"

#include <vector>

#include <boost/none.hpp>

namespace mongo {

Status noDocumentsViolatingValidator(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     PlacementConcern placementConcern) {
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

    auto aggCmdObj = aggRequest.toBSON();
    rpc::OpMsgReplyBuilder replyBuilder;

    // We use runAggregate here instead of something like DBDirectClient because in a sharded
    // environment we want the DDL coordinating shard to do targeting and send sub aggregations to
    // the other participants.
    auto status =
        runAggregate(opCtx,
                     aggRequest,
                     {aggRequest},
                     aggCmdObj,
                     {Privilege(ResourcePattern::forExactNamespace(nss), ActionType::find)},
                     boost::none,
                     &replyBuilder);
    if (!status.isOK()) {
        return status;
    }

    auto bodyBuilder = replyBuilder.getBodyBuilder();
    CommandHelpers::appendSimpleCommandStatus(bodyBuilder, true);
    bodyBuilder.doneFast();

    auto cursorResponse = CursorResponse::parseFromBSON(replyBuilder.releaseBody(),
                                                        nullptr,
                                                        nss.tenantId(),
                                                        SerializationContext::stateCommandReply());
    if (!cursorResponse.isOK()) {
        return cursorResponse.getStatus();
    }

    if (cursorResponse.getValue().getBatch().empty()) {
        return Status::OK();
    }

    str::stream msg;
    msg << "Cannot upgrade validationLevel to 'constraint': the collection contains documents "
           "that do not satisfy the validator.";
    constexpr size_t kMaxValidatorInErrorMessage = 10 * 1024;
    StringBuilder validatorStr;
    validator.toString(validatorStr, /*isArray=*/false, /*full=*/true);
    auto validatorStrMaterialized =
        static_cast<size_t>(validatorStr.len()) < kMaxValidatorInErrorMessage
        ? validatorStr.str()
        : "<your collection's validator>";
    msg << " Run db." << nss.coll() << ".find({\"$nor\": [" << validatorStrMaterialized
        << "]}) to find non-compliant documents.";
    return {ErrorCodes::Error(12370902), msg};
}

}  // namespace mongo
