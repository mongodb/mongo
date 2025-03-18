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

#include <boost/none.hpp>
#include <cmath>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/auth/auth_name.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/metadata/audit_client_attrs.h"
#include "mongo/rpc/metadata/audit_metadata.h"
#include "mongo/rpc/metadata/audit_user_attrs.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/synchronized_value.h"

namespace mongo {
namespace rpc {
void setAuditClientMetadata(OperationContext* opCtx,
                            const ImpersonatedClientMetadata& clientMetadata,
                            boost::optional<ImpersonatedClientSessionGuard>& clientSessionGuard) {
    // TODO SERVER-83990: remove
    if (!gFeatureFlagExposeClientIpInAuditLogs.isEnabledUseLastLTSFCVWhenUninitialized(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        return;
    }
    // Do not set/reset client metadata if it was not sent from a mongos
    if (!opCtx->getClient()->isFromUserConnection()) {
        return;
    }

    clientSessionGuard.emplace(opCtx->getClient(), clientMetadata);
}

void setAuditMetadata(OperationContext* opCtx,
                      const boost::optional<AuditMetadata>& data,
                      boost::optional<ImpersonatedClientSessionGuard>& clientSessionGuard) {
    if (data) {
        const auto& user = data->getUser();
        const auto& roles = data->getRoles();
        const auto& clientMetadata = data->getClientMetadata();
        if (user || !roles.empty() || clientMetadata) {
            // Only set $impersonatedUser, $impersonatedRoles, or $impersonatedClient if the client
            // is authorized for cluster-level privileges.
            auto* authzSession = AuthorizationSession::get(opCtx->getClient());
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized use of impersonation metadata.",
                    authzSession->isAuthorizedForClusterActions({ActionType::impersonate},
                                                                boost::none));

            if (clientMetadata) {
                setAuditClientMetadata(opCtx, clientMetadata.value(), clientSessionGuard);
            }

            if (user || !roles.empty()) {
                rpc::AuditUserAttrs::set(opCtx, *user, roles, true /* isImpersonating */);
            }
        }
    }
}

boost::optional<AuditMetadata> getAuditAttrsToAuditMetadata(OperationContext* opCtx) {
    // If we have no opCtx, which does appear to happen, don't do anything.
    if (!opCtx) {
        return {};
    }

    boost::optional<AuditMetadata> metadata;
    auto auditUserAttrs = AuditUserAttrs::get(opCtx);

    if (auditUserAttrs) {
        metadata = AuditMetadata();
        metadata->setUser(auditUserAttrs->getUser());
        metadata->setRoles(auditUserAttrs->getRoles());
    }

    // TODO SERVER-83990: remove
    if (gFeatureFlagExposeClientIpInAuditLogs.isEnabledUseLastLTSFCVWhenUninitialized(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        if (auto clientAttrs = AuditClientAttrs::get(opCtx->getClient())) {
            if (!metadata) {
                metadata = AuditMetadata();
                // Roles is not an optional field so we initiate it with an empty vector.
                metadata->setRoles({});
            }
            metadata->setClientMetadata(clientAttrs->generateClientMetadataObj());
        }
    }

    return metadata;
}

void writeAuditMetadata(OperationContext* opCtx, BSONObjBuilder* out) {
    if (auto meta = getAuditAttrsToAuditMetadata(opCtx)) {
        BSONObjBuilder section(out->subobjStart(kImpersonationMetadataSectionName));
        meta->serialize(&section);
    }
}

std::size_t estimateAuditMetadataSize(
    const boost::optional<UserName>& userName,
    const std::vector<RoleName>& roleNames,
    const boost::optional<ImpersonatedClientMetadata>& clientMetadata = boost::none) {
    // If there are no users/roles being impersonated just exit
    if (!userName && roleNames.empty()) {
        return 0;
    }

    std::size_t ret = 4 +                                   // BSONObj size
        1 + kImpersonationMetadataSectionName.size() + 1 +  // "$audit" sub-object key
        4;                                                  // $audit object length

    if (userName) {
        // BSONObjType + "impersonatedUser" + NULL + UserName object.
        ret += 1 + AuditMetadata::kUserFieldName.size() + 1 + userName->getBSONObjSize();
    }

    // BSONArrayType + "impersonatedRoles" + NULL + BSONArray Length
    ret += 1 + AuditMetadata::kRolesFieldName.size() + 1 + 4;
    for (std::size_t i = 0; i < roleNames.size(); i++) {
        // BSONType::Object + strlen(indexId) + NULL byte
        // to_string(i).size() will be log10(i) plus some rounding and fuzzing.
        // Increment prior to taking the log so that we never take log10(0) which is NAN.
        // This estimates one extra byte every time we reach (i % 10) == 9.
        ret += 1 + static_cast<std::size_t>(1.1 + log10(i + 1)) + 1;
        ret += roleNames[i].getBSONObjSize();
    }

    // EOD terminator for impersonatedRoles array
    ret += 1;

    // TODO SERVER-83990: remove
    if (gFeatureFlagExposeClientIpInAuditLogs.isEnabledUseLastLTSFCVWhenUninitialized(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) &&
        clientMetadata) {
        // BSONObjType + "impersonatedClient" + NULL + Object start
        ret += 1 + AuditMetadata::kClientMetadataFieldName.size() + 1 + 4;

        // BSONArrayType + "hosts" + NULL + Array length
        ret += 1 + ImpersonatedClientMetadata::kHostsFieldName.size() + 1 + 4;

        const auto& hosts = clientMetadata->getHosts();
        for (std::size_t i = 0; i < hosts.size(); ++i) {
            // BSONType::String + strlen(indexId) + NULL byte
            ret += 1 + static_cast<std::size_t>(1.1 + log10(i + 1)) + 1;
            // String size + string content + NULL terminator
            ret += 4 + hosts[i].toString().size() + 1;
        }

        // EOD terminators for: hosts array and impersonatedClient object
        ret += 1 + 1;
    }

    // EOD terminators for: $audit object and metadata object
    ret += 1 + 1;

    return ret;
}

std::size_t estimateAuditMetadataSize(OperationContext* opCtx) {
    auto auditUserAttrs = AuditUserAttrs::get(opCtx);
    if (!auditUserAttrs) {
        return 0;
    }

    if (auto clientAttrs = AuditClientAttrs::get(opCtx->getClient())) {
        return estimateAuditMetadataSize(auditUserAttrs->getUser(),
                                         auditUserAttrs->getRoles(),
                                         clientAttrs->generateClientMetadataObj());
    }

    return estimateAuditMetadataSize(auditUserAttrs->getUser(), auditUserAttrs->getRoles());
}

std::size_t estimateAuditMetadataSize(const AuditMetadata& md) {
    return estimateAuditMetadataSize(md.getUser(), md.getRoles(), md.getClientMetadata());
}

}  // namespace rpc
}  // namespace mongo
