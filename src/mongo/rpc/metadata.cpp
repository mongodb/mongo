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

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/rpc/metadata/audit_metadata.h"
#include "mongo/rpc/metadata/client_metadata_ismaster.h"
#include "mongo/rpc/metadata/config_server_metadata.h"
#include "mongo/rpc/metadata/logical_time_metadata.h"
#include "mongo/rpc/metadata/sharding_metadata.h"
#include "mongo/rpc/metadata/tracking_metadata.h"

namespace mongo {
namespace rpc {

BSONObj makeEmptyMetadata() {
    return BSONObj();
}

void readRequestMetadata(OperationContext* opCtx, const BSONObj& metadataObj) {
    BSONElement readPreferenceElem;
    BSONElement auditElem;
    BSONElement configSvrElem;
    BSONElement trackingElem;
    BSONElement clientElem;
    BSONElement logicalTimeElem;

    for (const auto& metadataElem : metadataObj) {
        auto fieldName = metadataElem.fieldNameStringData();
        if (fieldName == "$readPreference") {
            readPreferenceElem = metadataElem;
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

    if (readPreferenceElem) {
        ReadPreferenceSetting::get(opCtx) =
            uassertStatusOK(ReadPreferenceSetting::fromInnerBSON(readPreferenceElem));
    }

    AuditMetadata::get(opCtx) = uassertStatusOK(AuditMetadata::readFromMetadata(auditElem));

    uassertStatusOK(ClientMetadataIsMasterState::readFromMetadata(opCtx, clientElem));

    ConfigServerMetadata::get(opCtx) =
        uassertStatusOK(ConfigServerMetadata::readFromMetadata(configSvrElem));

    TrackingMetadata::get(opCtx) =
        uassertStatusOK(TrackingMetadata::readFromMetadata(trackingElem));

    auto logicalClock = LogicalClock::get(opCtx);
    if (logicalClock) {
        auto logicalTimeMetadata =
            uassertStatusOK(rpc::LogicalTimeMetadata::readFromMetadata(logicalTimeElem));

        auto& signedTime = logicalTimeMetadata.getSignedTime();
        // LogicalTimeMetadata is default constructed if no logical time metadata was sent, so a
        // default constructed SignedLogicalTime should be ignored.
        if (signedTime.getTime() != LogicalTime::kUninitialized) {
            auto logicalTimeValidator = LogicalTimeValidator::get(opCtx);
            if (!LogicalTimeValidator::isAuthorizedToAdvanceClock(opCtx)) {
                if (!logicalTimeValidator) {
                    uasserted(ErrorCodes::CannotVerifyAndSignLogicalTime,
                              "Cannot accept logicalTime: " + signedTime.getTime().toString() +
                                  ". May not be a part of a sharded cluster");
                } else {
                    uassertStatusOK(logicalTimeValidator->validate(opCtx, signedTime));
                }
            }

            uassertStatusOK(logicalClock->advanceClusterTime(signedTime.getTime()));
        }
    }
}

CommandAndMetadata upconvertRequestMetadata(BSONObj legacyCmdObj, int queryFlags) {
    // We can reuse the same metadata BOB for every upconvert call, but we need to keep
    // making new command BOBs as each metadata bob will need to remove fields. We can not use
    // mutablebson here because the ReadPreference upconvert routine performs
    // manipulations (replacing a root with its child) that mutablebson doesn't
    // support.

    auto readPrefContainer = BSONObj();
    const StringData firstFieldName = legacyCmdObj.firstElementFieldName();
    if (firstFieldName == "$query" || firstFieldName == "query") {
        // Commands sent over OP_QUERY specify read preference by putting it at the top level and
        // putting the command in a nested field called either query or $query.

        // Check if legacyCommand has an invalid $maxTimeMS option.
        uassert(ErrorCodes::InvalidOptions,
                "cannot use $maxTimeMS query option with commands; use maxTimeMS command option "
                "instead",
                !legacyCmdObj.hasField("$maxTimeMS"));
        readPrefContainer = legacyCmdObj;
        legacyCmdObj = legacyCmdObj.firstElement().Obj().getOwned();
    } else if (auto queryOptions = legacyCmdObj["$queryOptions"]) {
        // Mongos rewrites commands with $readPreference to put it in a field nested inside of
        // $queryOptions. Its command implementations often forward commands in that format to
        // shards. This function is responsible for rewriting it to a format that the shards
        // understand.
        readPrefContainer = queryOptions.Obj().getOwned();
        legacyCmdObj = legacyCmdObj.removeField("$queryOptions");
    }

    BSONObjBuilder metadataBob;
    if (auto readPref = readPrefContainer["$readPreference"]) {
        metadataBob.append(readPref);
    } else if (queryFlags & QueryOption_SlaveOk) {
        ReadPreferenceSetting(ReadPreference::SecondaryPreferred).toContainingBSON(&metadataBob);
    }

    BSONObjBuilder logicalTimeCommandBob;
    for (auto elem : legacyCmdObj) {
        if (elem.fieldNameStringData() == LogicalTimeMetadata::fieldName()) {
            metadataBob.append(elem);
        } else {
            logicalTimeCommandBob.append(elem);
        }
    }

    return std::make_tuple(logicalTimeCommandBob.obj(), metadataBob.obj());
}

CommandReplyWithMetadata upconvertReplyMetadata(const BSONObj& legacyReply) {
    BSONObjBuilder commandReplyBob;
    BSONObjBuilder metadataBob;

    uassertStatusOK(ShardingMetadata::upconvert(legacyReply, &commandReplyBob, &metadataBob));
    return std::make_tuple(commandReplyBob.obj(), metadataBob.obj());
}

}  // namespace rpc
}  // namespace mongo
