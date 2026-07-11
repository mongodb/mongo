// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/rpc/metadata.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/util/deferred.h"
#include "mongo/db/server_options.h"
#include "mongo/db/shard_role/shard_catalog/raw_data_operation.h"
#include "mongo/db/stats/direct_system_buckets_access.h"
#include "mongo/db/stats/external_client_on_router.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/topology/user_write_block/replica_set_write_block_bypass.h"
#include "mongo/db/topology/user_write_block/user_write_block_bypass.h"
#include "mongo/db/topology/vector_clock/vector_clock.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/audit_metadata.h"
#include "mongo/rpc/metadata/audit_user_attrs.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/string_map.h"
#include "mongo/util/testing_proctor.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {
namespace rpc {
MONGO_FAIL_POINT_DEFINE(failIfOperationKeyMismatch);

BSONObj makeEmptyMetadata() {
    return BSONObj();
}

namespace {
// True if the request's client holds the internal-cluster privilege required to propagate
// internal-only generic arguments (operation key, write-blocking bypass, versionContext, ifrFlags).
bool hasInternalAuthorization(OperationContext* opCtx) {
    auto authSession = AuthorizationSession::get(opCtx->getClient());
    return authSession->isAuthorizedForActionsOnResource(
        ResourcePattern::forClusterResource(authSession->getUserTenantId()), ActionType::internal);
}

void readPrivilegedRequestMetadata(OperationContext* opCtx, const GenericArguments& requestArgs) {
    // If we are in direct client, privileged metadata should already be set by the initial request.
    if (opCtx->getClient()->isInDirectClient()) {
        tassert(9955802,
                "Unexpected clientOperationKey in direct client",
                !requestArgs.getClientOperationKey());
        return;
    }

    // Check for authorization lazily, to optimize for the common case with no arguments present.
    Deferred hasInternalAuthorization{[&] {
        return rpc::hasInternalAuthorization(opCtx);
    }};

    if (requestArgs.getClientOperationKey() &&
        (TestingProctor::instance().isEnabled() || hasInternalAuthorization())) {
        {
            // We must obtain the client lock to set the OperationKey on the operation context as
            // it may be concurrently read by CurrentOp.
            std::lock_guard lg(*opCtx->getClient());
            opCtx->setOperationKey(std::move(*requestArgs.getClientOperationKey()));
        }
        failIfOperationKeyMismatch.execute([&](const BSONObj& data) {
            tassert(7446600,
                    "OperationKey in request does not match test provided OperationKey",
                    data["clientOperationKey"].String() ==
                        opCtx->getOperationKey()->toBSON()["uuid"].String());
        });
    }

    uassert(6317500,
            "Client is not properly authorized to propagate mayBypassWriteBlocking",
            !requestArgs.getMayBypassWriteBlocking() || hasInternalAuthorization());
    // setFromMetadata must still be called to set the default value if it's not set in the request
    WriteBlockBypass::get(opCtx).setFromMetadata(opCtx, requestArgs.getMayBypassWriteBlocking());

    uassert(12097002,
            "Client is not properly authorized to propagate mayBypassReplicaSetWriteBlocking",
            !requestArgs.getMayBypassReplicaSetWriteBlocking() || hasInternalAuthorization());
    // setFromMetadata must still be called to set the default value if it's not set in the request
    ReplicaSetWriteBlockBypass::get(opCtx).setFromMetadata(
        opCtx, requestArgs.getMayBypassReplicaSetWriteBlocking());

    uassert(9955800,
            "Client is not properly authorized to propagate versionContext",
            !requestArgs.getVersionContext() || hasInternalAuthorization());
    if (requestArgs.getVersionContext()) {
        ClientLock lg(opCtx->getClient());
        // Enable a versionContext we received through a network request to transitively propagate
        // to other shards as part of network commands. This is safe because the original operation
        // that enabled propagation must be durable, subject to draining by setFCV, and retry until
        // all cluster-wide work it dispatches is done. So if _this_ operation starts propagating
        // versionContext in a sub-command and is then killed (ostensibly leaving a versionContext
        // in-flight that setFCV won't wait for), the draining by cluster-wide setFCV still waits
        // for the original operation, which has to keep retrying until it completes all work.
        // Once the work is done, any replayed commands become a no-op (or e.g. rejected via replay
        // protection), so they do no harm even if they are admitted with an stale versionContext.
        VersionContext::setFromMetadata(
            lg, opCtx, requestArgs.getVersionContext()->withPropagationAcrossShards_UNSAFE());
    }

    uassert(12137303,
            "Client is not properly authorized to propagate executionAdmissionContextType",
            !requestArgs.getExecutionAdmissionContextType() || hasInternalAuthorization());
    ExecutionAdmissionContext::get(opCtx).setFromMetadata(
        opCtx, requestArgs.getExecutionAdmissionContextType());

    // Typed commands which go through TypedCommand::InvocationBaseInternal will already have this
    // installed, but we keep the install check here to ensure even legacy commands can benefit from
    // a stable IFRContext.
    installIfrContextFromWire(opCtx, requestArgs);
}
}  // namespace

void installIfrContextFromWire(OperationContext* opCtx, const GenericArguments& requestArgs) {
    if (opCtx->getClient()->isInDirectClient()) {
        // Nested operations (DBDirectClient sub-commands) share the parent operation's opCtx and
        // must never install: they inherit the parent's context once it is installed. Several
        // flows run nested commands *before* the parent command is even parsed — e.g. the
        // localhost-auth-bypass check. If such a nested command installed here, it would claim the
        // decoration with a conservative no-flags context and the parent's later install would be
        // skipped, silently discarding the wire-provided IFR flag values.
        // TODO SERVER-131000 consider if we can get an IFRContext initialized earlier for this
        // case.
        return;
    }
    if (IncrementalFeatureRolloutContext::isInstalled(opCtx)) {
        LOGV2_DEBUG(13002304,
                    4,
                    "Skipping IFRContext initialization since the given opCtx already has one");
        return;
    }

    if (const auto& ifrFlags = requestArgs.getIfrFlags()) {
        // Both ifrFlags and ifrSenderVersion are internal-only fields; check authorization once
        // here rather than unconditionally on every command (the common path has no ifrFlags).
        uassert(13002302,
                "ifrFlags are an internal mechanism. Client is not properly authorized to "
                "propagate ifrFlags",
                hasInternalAuthorization(opCtx));

        const auto& senderVersion = requestArgs.getIfrSenderVersion();
        IncrementalFeatureRolloutContext::set(
            opCtx,
            IncrementalFeatureRolloutContext::fromWire(
                *ifrFlags,
                senderVersion ? std::make_unique<IFRSenderVersion>(*senderVersion) : nullptr));
    } else {
        // Sender didn't include the ifrFlags field at all — either a binary that predates the
        // IFR wire protocol (e.g. a last-lts mongos forwarding to a latest shard) or a caller
        // that isn't participating in the protocol. Conservatively disables kLatest-introduced
        // flags on a shard server (no router coordinated a value) and uses local defaults on a
        // standalone / plain replica set.
        IncrementalFeatureRolloutContext::installForRequestWithoutIfrFlags(opCtx);
    }
}

void readRequestMetadata(OperationContext* opCtx,
                         const GenericArguments& requestArgs,
                         bool cmdRequiresAuth,
                         boost::optional<ImpersonatedClientSessionGuard>& clientSessionGuard) {
    readPrivilegedRequestMetadata(opCtx, requestArgs);

    if (auto& rp = requestArgs.getReadPreference()) {
        ReadPreferenceSetting::get(opCtx) = *rp;
    }

    setAuditMetadata(opCtx, requestArgs.getDollarAudit(), clientSessionGuard);

    // We check for "$client" but not "client" here, because currentOp can filter on "client" as
    // a top-level field.
    if (const auto& md = requestArgs.getDollarClient()) {
        // The '$client' field is populated by mongos when it sends requests to shards on behalf of
        // its own requests. This may or may not be relevant for SERVER-50804.
        ClientMetadata::setFromMetadataForOperation(opCtx, *md);
    }


    GossipedVectorClockComponents components;
    components.setDollarConfigTime(requestArgs.getDollarConfigTime());
    components.setDollarTopologyTime(requestArgs.getDollarTopologyTime());
    components.setDollarClusterTime(requestArgs.getDollarClusterTime());
    VectorClock::get(opCtx)->gossipIn(opCtx, components, !cmdRequiresAuth);

    if (requestArgs.getRawData()) {
        isRawDataOperation(opCtx) = true;
    }

    if (requestArgs.getIsDirectSystemBucketsAccess()) {
        isDirectSystemBucketsAccess(opCtx) = true;
    }

    if (requestArgs.getIsExternalClientOnRouter()) {
        isExternalClientOnRouter(opCtx) = true;
    }
}

namespace {
using namespace std::literals::string_view_literals;
boost::optional<std::string_view> commandNameToDocumentSequenceName(std::string_view commandName) {
    if (commandName == "insert"sv) {
        return "documents"sv;
    }
    if (commandName == "update"sv) {
        return "updates"sv;
    }
    if (commandName == "delete"sv) {
        return "deletes"sv;
    }
    return boost::none;
}

bool isArrayOfObjects(BSONElement array) {
    if (array.type() != BSONType::array)
        return false;

    for (auto elem : array.Obj()) {
        if (elem.type() != BSONType::object)
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

    OpMsgRequest::DocumentSequence sequence{std::string{*docSeqName}};
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
    if (cmdObj.hasField("$queryOptions"sv)) {
        // TODO SERVER-29091: The use of $queryOptions is a holdover related to the
        // no-longer-supported OP_QUERY format. We should remove it from the code base.
        fieldsToRemove.insert("$queryOptions"sv);
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
        vts, dbName, upconvertCommandObj(std::move(cmdObj), docSequence, std::move(readPref)));
    if (docSequence) {
        out.sequences.push_back(std::move(*docSequence));
    }
    return out;
}

}  // namespace rpc
}  // namespace mongo
