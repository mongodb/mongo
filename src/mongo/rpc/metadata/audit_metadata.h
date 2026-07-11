// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/rpc/metadata/audit_metadata_gen.h"
#include "mongo/rpc/metadata/impersonated_client_session.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] rpc {

/*
 * The name of the BSON element in message metadata that contains the impersonated user and the
 * client address data.
 *
 * This is called "$audit" because in pre-4.2 the enterprise audit subsystem already passed
 * the impersonated users info around in metadata for auditing purposes. This has been lifted
 * into the community edition and appears in network messages as "$audit" for backwards
 * compatibility.
 *
 * This metadata should only appear in requests from mongos to mongod.
 */
[[MONGO_MOD_FILE_PRIVATE]] inline constexpr std::string_view kImpersonationMetadataSectionName{
    "$audit"};

/*
 * Sets the provided audit metadata on the AuditClientAttrs decorator (via
 * ImpersonatedClientSessionGuard) or AuditUserAttrs decorator respectively only if their data is
 * present.
 */
void setAuditMetadata(OperationContext* opCtx,
                      const boost::optional<AuditMetadata>& data,
                      boost::optional<ImpersonatedClientSessionGuard>& clientSessionGuard);

/*
 * Get impersonation metadata off the opCtx
 */
boost::optional<AuditMetadata> getAuditAttrsToAuditMetadata(OperationContext* opCtx);

/*
 * Writes the current impersonation metadata off the opCtx and into a BSONObjBuilder
 */
void writeAuditMetadata(OperationContext* opCtx, BSONObjBuilder* out);

/*
 * Estimates the size of impersonation metadata which will be written by
 * writeAuditMetadata.
 */
std::size_t estimateAuditMetadataSize(const AuditMetadata& md);
std::size_t estimateAuditMetadataSize(OperationContext* opCtx);

}  // namespace rpc
}  // namespace mongo
