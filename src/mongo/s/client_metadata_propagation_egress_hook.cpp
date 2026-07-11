// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/client_metadata_propagation_egress_hook.h"

#include "mongo/db/feature_flag.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/shard_role/shard_catalog/raw_data_operation.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/stats/direct_system_buckets_access.h"
#include "mongo/db/stats/external_client_on_router.h"
#include "mongo/db/topology/user_write_block/replica_set_write_block_bypass.h"
#include "mongo/db/topology/user_write_block/user_write_block_bypass.h"
#include "mongo/idl/generic_argument_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/audit_metadata.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand
namespace mongo {
namespace rpc {
namespace {

void maybeAppendIFRContext(const std::shared_ptr<IncrementalFeatureRolloutContext>& ifrCtx,
                           BSONObjBuilder* metadataBob) {
    // A router stamps its own binary's full version so the receiving shard can distinguish "flag
    // omitted because sender predates it" from "flag omitted because sender removed it." A shard
    // whose current ifrCtx was created from an inbound wire payload forwards the *original*
    // sender's version verbatim: this is what preserves multi-hop semantics across e.g. mongos ->
    // shardA -> shardB $lookup fan-out.
    // A shard originating a fresh (non-wire) request — e.g. a background thread — emits nothing.
    const bool isRouter = serverGlobalParams.clusterRole.hasExclusively(ClusterRole::RouterServer);
    if (ifrCtx->isInstalledFromWire() || isRouter) {
        ifrCtx->appendToEgressMetadata(metadataBob);
    }
}
}  // namespace

Status ClientMetadataPropagationEgressHook::writeRequestMetadata(OperationContext* opCtx,
                                                                 BSONObjBuilder* metadataBob) {
    if (!opCtx) {
        return Status::OK();
    }

    try {
        writeAuditMetadata(opCtx, metadataBob);

        if (auto metadata = ClientMetadata::get(opCtx->getClient())) {
            metadata->writeToMetadata(metadataBob);
        }

        if (auto& vCtx = VersionContext::getDecoration(opCtx); vCtx.hasOperationFCV()) {
            tassert(11144301,
                    "Expected VersionContext with propagation across shards",
                    vCtx.canPropagateAcrossShards());
            metadataBob->append(GenericArguments::kVersionContextFieldName, vCtx.toBSON());
        }

        if (auto ifrCtx = IncrementalFeatureRolloutContext::tryGet(opCtx); ifrCtx) {
            maybeAppendIFRContext(ifrCtx, metadataBob);
        }

        WriteBlockBypass::get(opCtx).writeAsMetadata(metadataBob);

        const auto fcvSnap = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
        if (feature_flags::gFeatureFlagBlockReplicaSetWrites
                .isEnabledUseLastLTSFCVWhenUninitialized(VersionContext::getDecoration(opCtx),
                                                         fcvSnap)) {
            ReplicaSetWriteBlockBypass::get(opCtx).writeAsMetadata(metadataBob);
        }

        ExecutionAdmissionContext::get(opCtx).writeAsMetadata(opCtx, metadataBob);

        if (isRawDataOperation(opCtx)) {
            metadataBob->append(kRawDataFieldName, true);
        }

        if (isDirectSystemBucketsAccess(opCtx)) {
            metadataBob->append(kIsDirectSystemBucketsAccessFieldName, true);
        }

        if (opCtx->getClient() &&
            (opCtx->getClient()->isFromUserConnection() || isExternalClientOnRouter(opCtx)) &&
            repl::feature_flags::gFeatureFlagExternalClientOnRouter
                .isEnabledUseLastLTSFCVWhenUninitialized(VersionContext::getDecoration(opCtx),
                                                         fcvSnap)) {
            metadataBob->appendBool(kIsExternalClientOnRouterFieldName, true);
        }

        // If the request is using the 'defaultMaxTimeMS' value, attaches the field so shards can
        // record the metrics correctly.
        if (opCtx->usesDefaultMaxTimeMS()) {
            metadataBob->appendBool("usesDefaultMaxTimeMS", true);
        }

        return Status::OK();
    } catch (...) {
        return exceptionToStatus();
    }
}

Status ClientMetadataPropagationEgressHook::readReplyMetadata(OperationContext* opCtx,
                                                              const BSONObj& metadataObj) {
    return Status::OK();
}

}  // namespace rpc
}  // namespace mongo
