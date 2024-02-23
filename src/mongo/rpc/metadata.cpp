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

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/vector_clock.h"
#include "mongo/db/write_block_bypass.h"
#include "mongo/rpc/metadata.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/rpc/metadata/impersonated_user_metadata.h"
#include "mongo/rpc/metadata/tracking_metadata.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/string_map.h"
#include "mongo/util/testing_proctor.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace rpc {
MONGO_FAIL_POINT_DEFINE(failIfOperationKeyMismatch);

BSONObj makeEmptyMetadata() {
    return BSONObj();
}

void readRequestMetadata(OperationContext* opCtx,
                         const CommonRequestArgs& requestArgs,
                         const OpMsgRequest& request,
                         bool cmdRequiresAuth) {
    AuthorizationSession* authSession = AuthorizationSession::get(opCtx->getClient());

    if (requestArgs.getClientOperationKey() &&
        (TestingProctor::instance().isEnabled() ||
         authSession->isAuthorizedForActionsOnResource(
             ResourcePattern::forClusterResource(request.getValidatedTenantId()),
             ActionType::internal))) {
        {
            // We must obtain the client lock to set the OperationKey on the operation context as
            // it may be concurrently read by CurrentOp.
            stdx::lock_guard lg(*opCtx->getClient());
            opCtx->setOperationKey(std::move(*requestArgs.getClientOperationKey()));
        }
        failIfOperationKeyMismatch.execute([&](const BSONObj& data) {
            tassert(7446600,
                    "OperationKey in request does not match test provided OperationKey",
                    data["clientOperationKey"].String() ==
                        opCtx->getOperationKey()->toBSON()["uuid"].String());
        });
    }

    if (requestArgs.getReadPreference()) {
        ReadPreferenceSetting::get(opCtx) = uassertStatusOK(
            ReadPreferenceSetting::fromInnerBSON(requestArgs.getReadPreference()->getElement()));

        if (opCtx->routedByReplicaSetEndpoint()) {
            ReadPreferenceSetting::get(opCtx).isPretargeted = true;
        } else if (ReadPreferenceSetting::get(opCtx).isPretargeted) {
            // '$_isPretargeted' is used exclusively by the replica set endpoint to mark commands
            // that it forces to go through the router as needing to target the local mongod.
            // Given that this request has been marked as pre-targeted, it must have originated from
            // a request routed by the replica set endpoint. Mark the opCtx with this info.
            opCtx->setRoutedByReplicaSetEndpoint(true);
        }
    }

    readImpersonatedUserMetadata(requestArgs.getImpersonation().value_or(IDLAnyType()).getElement(),
                                 opCtx);

    invariant(!auth::ValidatedTenancyScope::get(opCtx).has_value() ||
              (request.validatedTenancyScope &&
               *auth::ValidatedTenancyScope::get(opCtx) == *request.validatedTenancyScope));

    auth::ValidatedTenancyScope::set(opCtx, request.validatedTenancyScope);

    // We check for "$client" but not "client" here, because currentOp can filter on "client" as
    // a top-level field.
    if (requestArgs.getClientMetadata()) {
        // The '$client' field is populated by mongos when it sends requests to shards on behalf of
        // its own requests. This may or may not be relevant for SERVER-50804.
        ClientMetadata::setFromMetadataForOperation(opCtx,
                                                    requestArgs.getClientMetadata()->getElement());
    }

    if (auto ti = requestArgs.getTracking_info()) {
        TrackingMetadata::get(opCtx) =
            uassertStatusOK(TrackingMetadata::readFromMetadata(ti->getElement()));
    } else {
        TrackingMetadata::get(opCtx) = {};
    }

    VectorClock::get(opCtx)->gossipIn(
        opCtx, requestArgs.getGossipedVectorClockComponents(), !cmdRequiresAuth);

    WriteBlockBypass::get(opCtx).setFromMetadata(
        opCtx, requestArgs.getMayBypassWriteBlocking().value_or(IDLAnyType()).getElement());
}

namespace {
const auto docSequenceFieldsForCommands = StringMap<std::string>{
    {"insert", "documents"},  //
    {"update", "updates"},
    {"delete", "deletes"},
};

bool isArrayOfObjects(BSONElement array) {
    if (array.type() != Array)
        return false;

    for (auto elem : array.Obj()) {
        if (elem.type() != Object)
            return false;
    }

    return true;
}
}  // namespace


OpMsgRequest upconvertRequest(const DatabaseName& dbName,
                              BSONObj cmdObj,
                              int queryFlags,
                              boost::optional<auth::ValidatedTenancyScope> vts) {
    cmdObj = cmdObj.getOwned();  // Usually this is a no-op since it is already owned.

    auto readPrefContainer = BSONObj();
    if (auto queryOptions = cmdObj["$queryOptions"]) {
        // Mongos rewrites commands with $readPreference to put it in a field nested inside of
        // $queryOptions. Its command implementations often forward commands in that format to
        // shards. This function is responsible for rewriting it to a format that the shards
        // understand.
        //
        // TODO SERVER-29091: The use of $queryOptions is a holdover related to the
        // no-longer-supported OP_QUERY format. We should remove it from the code base.
        readPrefContainer = queryOptions.Obj().shareOwnershipWith(cmdObj);
        cmdObj = cmdObj.removeField("$queryOptions");
    }

    if (!readPrefContainer.isEmpty()) {
        uassert(ErrorCodes::InvalidOptions,
                "Duplicate readPreference found in command object.",
                !cmdObj.hasField("$readPreference"));
        cmdObj = BSONObjBuilder(std::move(cmdObj)).appendElements(readPrefContainer).obj();
    } else if (!cmdObj.hasField("$readPreference") && (queryFlags & QueryOption_SecondaryOk)) {
        BSONObjBuilder bodyBuilder(std::move(cmdObj));
        ReadPreferenceSetting(ReadPreference::SecondaryPreferred).toContainingBSON(&bodyBuilder);
        cmdObj = bodyBuilder.obj();
    }

    uassert(40621, "$db is not allowed in OP_QUERY requests", !cmdObj.hasField("$db"));

    // Try to move supported array fields into document sequences.
    auto docSequenceIt = docSequenceFieldsForCommands.find(cmdObj.firstElementFieldName());
    auto docSequenceElem = docSequenceIt == docSequenceFieldsForCommands.end()
        ? BSONElement()
        : cmdObj[docSequenceIt->second];
    if (!isArrayOfObjects(docSequenceElem))
        return OpMsgRequestBuilder::createWithValidatedTenancyScope(dbName, vts, std::move(cmdObj));

    auto docSequenceName = docSequenceElem.fieldNameStringData();

    // Note: removing field before adding "$db" to avoid the need to copy the potentially large
    // array.
    auto out = OpMsgRequestBuilder::createWithValidatedTenancyScope(
        dbName, vts, cmdObj.removeField(docSequenceName));
    out.sequences.push_back({docSequenceName.toString()});
    for (auto elem : docSequenceElem.Obj()) {
        out.sequences[0].objs.push_back(elem.Obj().shareOwnershipWith(cmdObj));
    }
    return out;
}

}  // namespace rpc
}  // namespace mongo
