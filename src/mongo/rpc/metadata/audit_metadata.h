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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/rpc/metadata/audit_metadata_gen.h"
#include "mongo/rpc/metadata/impersonated_client_session.h"

#include <cstddef>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace rpc {

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
static constexpr auto kImpersonationMetadataSectionName = "$audit"_sd;

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
