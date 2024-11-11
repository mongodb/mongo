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
    }

    if (opCtx->routedByReplicaSetEndpoint()) {
        ReadPreferenceSetting::get(opCtx).isPretargeted = true;
    } else if (ReadPreferenceSetting::get(opCtx).isPretargeted) {
        // '$_isPretargeted' is used exclusively by the replica set endpoint to mark commands
        // that it forces to go through the router as needing to target the local mongod.
        // Given that this request has been marked as pre-targeted, it must have originated from
        // a request routed by the replica set endpoint. Mark the opCtx with this info.
        opCtx->setRoutedByReplicaSetEndpoint(true);
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

    VectorClock::get(opCtx)->gossipIn(
        opCtx, requestArgs.getGossipedVectorClockComponents(), !cmdRequiresAuth);

    WriteBlockBypass::get(opCtx).setFromMetadata(
        opCtx, requestArgs.getMayBypassWriteBlocking().value_or(IDLAnyType()).getElement());
}

namespace {
boost::optional<StringData> commandNameToDocumentSequenceName(StringData commandName) {
    if (commandName == "insert"_sd) {
        return "documents"_sd;
    }
    if (commandName == "update"_sd) {
        return "updates"_sd;
    }
    if (commandName == "delete"_sd) {
        return "deletes"_sd;
    }
    return boost::none;
}

bool isArrayOfObjects(BSONElement array) {
    if (array.type() != Array)
        return false;

    for (auto elem : array.Obj()) {
        if (elem.type() != Object)
            return false;
    }

    return true;
}

boost::optional<OpMsgRequest::DocumentSequence> extractDocumentSequence(BSONObj cmdObj) {
    auto cmdName = cmdObj.firstElementFieldNameStringData();
    auto docSeqName = commandNameToDocumentSequenceName(cmdName);
    if (!docSeqName.has_value()) {
        return boost::none;
    }

    auto docSeqElem = cmdObj[*docSeqName];
    if (!isArrayOfObjects(docSeqElem)) {
        return boost::none;
    }

    OpMsgRequest::DocumentSequence sequence{docSeqName->toString()};
    for (auto elem : docSeqElem.Obj()) {
        sequence.objs.push_back(elem.Obj().shareOwnershipWith(cmdObj));
    }
    return sequence;
}

boost::optional<BSONObj> extractLegacyReadPreference(BSONObj cmdObj, int queryFlags) {
    if (auto queryOptions = cmdObj["$queryOptions"]) {
        if (auto readPref = queryOptions["$readPreference"]) {
            uassert(ErrorCodes::InvalidOptions,
                    "Duplicate readPreference found in command object.",
                    !cmdObj.hasField("$readPreference"));
            return readPref.wrap();
        }
    }

    if (cmdObj.hasField("$readPreference")) {
        // Early exit with 'boost::none' if a non-legacy read preference is specified. This
        // avoids unnecessary object rebuilds.
        return boost::none;
    }

    if (queryFlags & QueryOption_SecondaryOk) {
        return ReadPreferenceSetting(ReadPreference::SecondaryPreferred).toContainingBSON();
    }

    return boost::none;
}

BSONObj upconvertCommandObj(BSONObj cmdObj,
                            const boost::optional<OpMsgRequest::DocumentSequence>& docSeq,
                            const boost::optional<BSONObj>& readPref) {
    StringDataSet fieldsToRemove;
    if (cmdObj.hasField("$queryOptions"_sd)) {
        // TODO SERVER-29091: The use of $queryOptions is a holdover related to the
        // no-longer-supported OP_QUERY format. We should remove it from the code base.
        fieldsToRemove.insert("$queryOptions"_sd);
    }

    if (docSeq.has_value()) {
        // Avoid the need to copy a potentially large array.
        fieldsToRemove.insert(docSeq->name);
    }

    const bool needsRebuild = fieldsToRemove.size() > 0 || readPref.has_value();
    if (!needsRebuild) {
        // Avoid rebuilding 'cmdObj' if no changes are required.
        return cmdObj;
    }

    BSONObjBuilder builder = [&] {
        if (fieldsToRemove.size() == 0) {
            // Initialize the 'builder' via a single bulk copy rather than a series of inserts.
            return BSONObjBuilder{cmdObj};
        }

        BSONObjBuilder b;
        for (auto elem : cmdObj) {
            const bool removeField = fieldsToRemove.contains(elem.fieldNameStringData());
            if (!removeField) {
                b.append(elem);
            }
        }
        return b;
    }();
    if (readPref) {
        builder.append(readPref->firstElement());
    }
    return builder.obj();
}
}  // namespace

/**
 * Mongos rewrites commands with $readPreference by nesting the field inside of $queryOptions.
 * Before forwarding this command to a shard, we need to rewrite the command to a format the shard
 * can understand.
 */
OpMsgRequest upconvertRequest(const DatabaseName& dbName,
                              BSONObj cmdObj,
                              int queryFlags,
                              boost::optional<auth::ValidatedTenancyScope> vts) {
    uassert(40621, "$db is not allowed in OP_QUERY requests", !cmdObj.hasField("$db"));

    // Ensure 'cmdObj' is owned. Usually this is a no-op.
    cmdObj = cmdObj.getOwned();
    auto docSequence = extractDocumentSequence(cmdObj);
    auto readPref = extractLegacyReadPreference(cmdObj, queryFlags);
    auto out = OpMsgRequestBuilder::create(
        vts, dbName, upconvertCommandObj(std::move(cmdObj), docSequence, readPref));
    if (docSequence) {
        out.sequences.push_back(std::move(*docSequence));
    }
    return out;
}

}  // namespace rpc
}  // namespace mongo
