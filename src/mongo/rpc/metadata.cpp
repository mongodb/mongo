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

#include "mongo/rpc/metadata.h"

#include "mongo/client/read_preference.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/multitenancy.h"
#include "mongo/db/vector_clock.h"
#include "mongo/db/write_block_bypass.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/rpc/metadata/impersonated_user_metadata.h"
#include "mongo/rpc/metadata/tracking_metadata.h"
#include "mongo/util/string_map.h"
#include "mongo/util/testing_proctor.h"

namespace mongo {
namespace rpc {

BSONObj makeEmptyMetadata() {
    return BSONObj();
}

void readRequestMetadata(OperationContext* opCtx, const OpMsg& opMsg, bool cmdRequiresAuth) {
    BSONElement readPreferenceElem;
    BSONElement trackingElem;
    BSONElement clientElem;
    BSONElement helloClientElem;
    BSONElement impersonationElem;
    BSONElement clientOperationKeyElem;
    BSONElement mayBypassWriteBlockingElem;

    for (const auto& metadataElem : opMsg.body) {
        auto fieldName = metadataElem.fieldNameStringData();
        if (fieldName == "$readPreference") {
            readPreferenceElem = metadataElem;
        } else if (fieldName == ClientMetadata::fieldName()) {
            clientElem = metadataElem;
        } else if (fieldName == TrackingMetadata::fieldName()) {
            trackingElem = metadataElem;
        } else if (fieldName == kImpersonationMetadataSectionName) {
            impersonationElem = metadataElem;
        } else if (fieldName == "clientOperationKey"_sd) {
            clientOperationKeyElem = metadataElem;
        } else if (fieldName == WriteBlockBypass::fieldName()) {
            mayBypassWriteBlockingElem = metadataElem;
        }
    }

    AuthorizationSession* authSession = AuthorizationSession::get(opCtx->getClient());

    if (clientOperationKeyElem &&
        (TestingProctor::instance().isEnabled() ||
         authSession->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                       ActionType::internal))) {
        auto opKey = uassertStatusOK(UUID::parse(clientOperationKeyElem));
        opCtx->setOperationKey(std::move(opKey));
    }

    if (readPreferenceElem) {
        ReadPreferenceSetting::get(opCtx) =
            uassertStatusOK(ReadPreferenceSetting::fromInnerBSON(readPreferenceElem));
    }

    readImpersonatedUserMetadata(impersonationElem, opCtx);
    auth::ValidatedTenancyScope::set(opCtx, opMsg.validatedTenancyScope);

    // We check for "$client" but not "client" here, because currentOp can filter on "client" as
    // a top-level field.
    if (clientElem) {
        // The '$client' field is populated by mongos when it sends requests to shards on behalf of
        // its own requests. This may or may not be relevant for SERVER-50804.
        ClientMetadata::setFromMetadataForOperation(opCtx, clientElem);
    }

    TrackingMetadata::get(opCtx) =
        uassertStatusOK(TrackingMetadata::readFromMetadata(trackingElem));

    VectorClock::get(opCtx)->gossipIn(opCtx, opMsg.body, !cmdRequiresAuth);

    WriteBlockBypass::get(opCtx).setFromMetadata(opCtx, mayBypassWriteBlockingElem);
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

    BSONObjBuilder builder;
    for (auto elem : cmdObj) {
        const bool removeField = fieldsToRemove.contains(elem.fieldNameStringData());
        if (!removeField) {
            builder.append(elem);
        }
    }
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
OpMsgRequest upconvertRequest(const DatabaseName& dbName, BSONObj cmdObj, int queryFlags) {
    uassert(40621, "$db is not allowed in OP_QUERY requests", !cmdObj.hasField("$db"));

    // Ensure 'cmdObj' is owned. Usually this is a no-op.
    cmdObj = cmdObj.getOwned();
    auto docSequence = extractDocumentSequence(cmdObj);
    auto readPref = extractLegacyReadPreference(cmdObj, queryFlags);
    auto out = OpMsgRequestBuilder::create(
        dbName, upconvertCommandObj(std::move(cmdObj), docSequence, std::move(readPref)));
    if (docSequence) {
        out.sequences.push_back(std::move(*docSequence));
    }
    return out;
}

}  // namespace rpc
}  // namespace mongo
