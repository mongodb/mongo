// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/forwardable_operation_metadata.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/auth/validated_tenancy_scope_factory.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/client.h"
#include "mongo/db/shard_role/shard_catalog/raw_data_operation.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/stats/direct_system_buckets_access.h"
#include "mongo/db/topology/user_write_block/replica_set_write_block_bypass.h"
#include "mongo/db/topology/user_write_block/user_write_block_bypass.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/otel/telemetry_context_holder.h"
#include "mongo/otel/traces/telemetry_context_serialization.h"
#include "mongo/rpc/metadata/audit_client_attrs.h"
#include "mongo/rpc/metadata/audit_metadata.h"
#include "mongo/rpc/metadata/audit_user_attrs.h"
#include "mongo/util/assert_util.h"

#include <mutex>
#include <string_view>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

ForwardableOperationMetadata::ForwardableOperationMetadata(const BSONObj& obj) {
    ForwardableOperationMetadataBase::parseProtected(
        obj, IDLParserContext("ForwardableOperationMetadataBase"));
}

ForwardableOperationMetadata::ForwardableOperationMetadata(OperationContext* opCtx) {
    if (auto optComment = opCtx->getComment()) {
        setComment(optComment->wrap());
    }

    setAuditUserMetadata(rpc::AuditUserAttrs::get(opCtx));

    if (auto auditClientAttrs = rpc::AuditClientAttrs::get(opCtx->getClient())) {
        setAuditClientMetadata(std::move(auditClientAttrs));
    }

    // TODO SERVER-99655: update once gSnapshotFCVInDDLCoordinators is enabled on the lastLTS
    if (auto& vCtx = VersionContext::getDecoration(opCtx); vCtx.hasOperationFCV()) {
        setVersionContext(VersionContext::getDecoration(opCtx));
    }

    boost::optional<std::string_view> originalSecurityToken = boost::none;
    const auto vts = auth::ValidatedTenancyScope::get(opCtx);
    if (vts != boost::none && !vts->getOriginalToken().empty()) {
        originalSecurityToken = vts->getOriginalToken();
    }
    setValidatedTenancyScopeToken(originalSecurityToken);

    setMayBypassWriteBlocking(WriteBlockBypass::get(opCtx).isWriteBlockBypassEnabled());

    const auto fcvSnap = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    if (feature_flags::gFeatureFlagBlockReplicaSetWrites.isEnabledUseLastLTSFCVWhenUninitialized(
            VersionContext::getDecoration(opCtx), fcvSnap)) {
        setMayBypassReplicaSetWritesBlocking(ReplicaSetWriteBlockBypass::get(opCtx).isEnabled());
    }

    setRawData(isRawDataOperation(opCtx));

    setIsDirectSystemBucketsAccess(isDirectSystemBucketsAccess(opCtx));

    if (auto telemetryCtx =
            otel::TelemetryContextHolder::getDecoration(opCtx).getTelemetryContext()) {
        setTelemetryContext(otel::traces::TelemetryContextSerializer::toBSON(telemetryCtx));
    }
}

void ForwardableOperationMetadata::setOn(OperationContext* opCtx) const {
    Client* client = opCtx->getClient();
    if (const auto& comment = getComment()) {
        std::lock_guard<Client> lk(*client);
        opCtx->setComment(comment.value());
    }

    if (const auto& optAuditUserMetadata = getAuditUserMetadata()) {
        rpc::AuditUserAttrs::set(opCtx, optAuditUserMetadata.value());
    }

    if (const auto& optAuditClientMetadata = getAuditClientMetadata()) {
        rpc::AuditClientAttrs::set(client, optAuditClientMetadata.value());
    }

    // TODO SERVER-99655: update once gSnapshotFCVInDDLCoordinators is enabled on the lastLTS
    if (const auto& vCtx = getVersionContext()) {
        ClientLock lk(opCtx->getClient());
        VersionContext::setDecoration(lk, opCtx, vCtx.value());
    }

    WriteBlockBypass::get(opCtx).set(getMayBypassWriteBlocking());

    ReplicaSetWriteBlockBypass::get(opCtx).set(getMayBypassReplicaSetWritesBlocking());

    isRawDataOperation(opCtx) = getRawData();

    isDirectSystemBucketsAccess(opCtx) = getIsDirectSystemBucketsAccess();

    boost::optional<auth::ValidatedTenancyScope> validatedTenancyScope = boost::none;
    const auto originalToken = getValidatedTenancyScopeToken();
    if (originalToken != boost::none && !originalToken->empty()) {
        validatedTenancyScope = auth::ValidatedTenancyScopeFactory::parse(client, *originalToken);
    }
    auth::ValidatedTenancyScope::set(opCtx, validatedTenancyScope);

    if (auto telemetryCtx = getTelemetryContext()) {
        auto deserializedTelemetryCtx =
            otel::traces::TelemetryContextSerializer::fromBSON(*telemetryCtx);
        if (deserializedTelemetryCtx) {
            auto& telemetryCtxHolder = otel::TelemetryContextHolder::getDecoration(opCtx);
            telemetryCtxHolder.setTelemetryContext(deserializedTelemetryCtx);
        }
    }
}

}  // namespace mongo
