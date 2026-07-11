// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/client_metadata_propagation_egress_hook.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/shard_role/shard_catalog/raw_data_operation.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/stats/direct_system_buckets_access.h"
#include "mongo/db/stats/external_client_on_router.h"
#include "mongo/db/topology/user_write_block/replica_set_write_block_bypass.h"
#include "mongo/db/topology/user_write_block/user_write_block_bypass.h"
#include "mongo/idl/generic_argument_gen.h"
#include "mongo/rpc/metadata/audit_metadata.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace rpc {

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
