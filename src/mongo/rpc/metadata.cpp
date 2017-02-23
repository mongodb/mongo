/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/rpc/metadata.h"

#include "mongo/base/init.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/logical_clock.h"
#include "mongo/rpc/metadata/audit_metadata.h"
#include "mongo/rpc/metadata/client_metadata_ismaster.h"
#include "mongo/rpc/metadata/config_server_metadata.h"
#include "mongo/rpc/metadata/logical_time_metadata.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/rpc/metadata/sharding_metadata.h"
#include "mongo/rpc/metadata/tracking_metadata.h"

namespace mongo {
namespace rpc {

namespace {

std::vector<Privilege> advanceLogicalClockPrivilege;

MONGO_INITIALIZER(InitializeAdvanceLogicalClockPrivilegeVector)(InitializerContext* const) {
    ActionSet actions;
    actions.addAction(ActionType::internal);
    advanceLogicalClockPrivilege.emplace_back(ResourcePattern::forClusterResource(), actions);
    return Status::OK();
}

bool isAuthorizedToAdvanceClock(OperationContext* opCtx) {
    auto client = opCtx->getClient();
    // Note: returns true if auth is off, courtesy of
    // AuthzSessionExternalStateServerCommon::shouldIgnoreAuthChecks.
    return AuthorizationSession::get(client)->isAuthorizedForPrivileges(
        advanceLogicalClockPrivilege);
}

}  // unnamed namespace

BSONObj makeEmptyMetadata() {
    return BSONObj();
}

Status readRequestMetadata(OperationContext* opCtx, const BSONObj& metadataObj) {
    BSONElement ssmElem;
    BSONElement auditElem;
    BSONElement configSvrElem;
    BSONElement trackingElem;
    BSONElement clientElem;
    BSONElement logicalTimeElem;

    for (const auto& metadataElem : metadataObj) {
        auto fieldName = metadataElem.fieldNameStringData();
        if (fieldName == ServerSelectionMetadata::fieldName()) {
            ssmElem = metadataElem;
        } else if (fieldName == AuditMetadata::fieldName()) {
            auditElem = metadataElem;
        } else if (fieldName == ConfigServerMetadata::fieldName()) {
            configSvrElem = metadataElem;
        } else if (fieldName == ClientMetadata::fieldName()) {
            clientElem = metadataElem;
        } else if (fieldName == TrackingMetadata::fieldName()) {
            trackingElem = metadataElem;
        } else if (fieldName == LogicalTimeMetadata::fieldName()) {
            logicalTimeElem = metadataElem;
        }
    }

    auto swServerSelectionMetadata = ServerSelectionMetadata::readFromMetadata(ssmElem);
    if (!swServerSelectionMetadata.isOK()) {
        return swServerSelectionMetadata.getStatus();
    }
    ServerSelectionMetadata::get(opCtx) = std::move(swServerSelectionMetadata.getValue());

    auto swAuditMetadata = AuditMetadata::readFromMetadata(auditElem);
    if (!swAuditMetadata.isOK()) {
        return swAuditMetadata.getStatus();
    }
    AuditMetadata::get(opCtx) = std::move(swAuditMetadata.getValue());

    const auto statusClientMetadata =
        ClientMetadataIsMasterState::readFromMetadata(opCtx, clientElem);
    if (!statusClientMetadata.isOK()) {
        return statusClientMetadata;
    }

    auto configServerMetadata = ConfigServerMetadata::readFromMetadata(configSvrElem);
    if (!configServerMetadata.isOK()) {
        return configServerMetadata.getStatus();
    }
    ConfigServerMetadata::get(opCtx) = std::move(configServerMetadata.getValue());

    auto trackingMetadata = TrackingMetadata::readFromMetadata(trackingElem);
    if (!trackingMetadata.isOK()) {
        return trackingMetadata.getStatus();
    }
    TrackingMetadata::get(opCtx) = std::move(trackingMetadata.getValue());

    auto logicalClock = LogicalClock::get(opCtx);
    if (logicalClock) {
        auto logicalTimeMetadata = LogicalTimeMetadata::readFromMetadata(logicalTimeElem);
        if (!logicalTimeMetadata.isOK()) {
            return logicalTimeMetadata.getStatus();
        }

        if (isAuthorizedToAdvanceClock(opCtx)) {
            auto advanceClockStatus = logicalClock->advanceClusterTimeFromTrustedSource(
                logicalTimeMetadata.getValue().getSignedTime());

            if (!advanceClockStatus.isOK()) {
                return advanceClockStatus;
            }
        } else {
            auto advanceClockStatus =
                logicalClock->advanceClusterTime(logicalTimeMetadata.getValue().getSignedTime());

            if (!advanceClockStatus.isOK()) {
                return advanceClockStatus;
            }
        }
    }

    return Status::OK();
}

Status writeRequestMetadata(OperationContext* opCtx, BSONObjBuilder* metadataBob) {
    auto ssStatus = ServerSelectionMetadata::get(opCtx).writeToMetadata(metadataBob);
    if (!ssStatus.isOK()) {
        return ssStatus;
    }
    return Status::OK();
}

StatusWith<CommandAndMetadata> upconvertRequestMetadata(BSONObj legacyCmdObj, int queryFlags) {
    // We can reuse the same metadata BOB for every upconvert call, but we need to keep
    // making new command BOBs as each metadata bob will need to remove fields. We can not use
    // mutablebson here because the ServerSelectionMetadata upconvert routine performs
    // manipulations (replacing a root with its child) that mutablebson doesn't
    // support.
    BSONObjBuilder metadataBob;

    // Ordering is important here - ServerSelectionMetadata must be upconverted
    // first, then AuditMetadata.
    BSONObjBuilder ssmCommandBob;
    auto upconvertStatus =
        ServerSelectionMetadata::upconvert(legacyCmdObj, queryFlags, &ssmCommandBob, &metadataBob);
    if (!upconvertStatus.isOK()) {
        return upconvertStatus;
    }


    BSONObjBuilder auditCommandBob;
    upconvertStatus =
        AuditMetadata::upconvert(ssmCommandBob.done(), queryFlags, &auditCommandBob, &metadataBob);

    if (!upconvertStatus.isOK()) {
        return upconvertStatus;
    }


    return std::make_tuple(auditCommandBob.obj(), metadataBob.obj());
}

StatusWith<LegacyCommandAndFlags> downconvertRequestMetadata(BSONObj cmdObj, BSONObj metadata) {
    int legacyQueryFlags = 0;
    BSONObjBuilder auditCommandBob;
    // Ordering is important here - AuditingMetadata must be downconverted first,
    // then ServerSelectionMetadata.
    auto downconvertStatus =
        AuditMetadata::downconvert(cmdObj, metadata, &auditCommandBob, &legacyQueryFlags);

    if (!downconvertStatus.isOK()) {
        return downconvertStatus;
    }


    BSONObjBuilder ssmCommandBob;
    downconvertStatus = ServerSelectionMetadata::downconvert(
        auditCommandBob.done(), metadata, &ssmCommandBob, &legacyQueryFlags);
    if (!downconvertStatus.isOK()) {
        return downconvertStatus;
    }


    return std::make_tuple(ssmCommandBob.obj(), std::move(legacyQueryFlags));
}

StatusWith<CommandReplyWithMetadata> upconvertReplyMetadata(const BSONObj& legacyReply) {
    BSONObjBuilder commandReplyBob;
    BSONObjBuilder metadataBob;

    auto upconvertStatus = ShardingMetadata::upconvert(legacyReply, &commandReplyBob, &metadataBob);
    if (!upconvertStatus.isOK()) {
        return upconvertStatus;
    }

    return std::make_tuple(commandReplyBob.obj(), metadataBob.obj());
}

StatusWith<BSONObj> downconvertReplyMetadata(const BSONObj& commandReply,
                                             const BSONObj& replyMetadata) {
    BSONObjBuilder legacyCommandReplyBob;

    auto downconvertStatus =
        ShardingMetadata::downconvert(commandReply, replyMetadata, &legacyCommandReplyBob);
    if (!downconvertStatus.isOK()) {
        return downconvertStatus;
    }

    return legacyCommandReplyBob.obj();
}

}  // namespace rpc
}  // namespace mongo
