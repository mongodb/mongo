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
#include "mongo/db/server_options.h"
#include "mongo/rpc/metadata/audit_metadata.h"
#include "mongo/rpc/metadata/client_metadata_ismaster.h"
#include "mongo/rpc/metadata/config_server_metadata.h"
#include "mongo/rpc/metadata/logical_time_metadata.h"
#include "mongo/rpc/metadata/sharding_metadata.h"
#include "mongo/rpc/metadata/tracking_metadata.h"
#include "mongo/util/string_map.h"

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
        // LogicalTimeMetadata is default constructed if no cluster time metadata was sent, so a
        // default constructed SignedLogicalTime should be ignored.
        if (signedTime.getTime() != LogicalTime::kUninitialized) {
            // Cluster times are only sent by sharding aware mongod servers, so this point is only
            // reached in sharded clusters.
            if (serverGlobalParams.featureCompatibility.getVersion() ==
                ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo36) {
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
}

namespace {
const auto docSequenceFieldsForCommands = StringMap<std::string>{
    {"insert", "documents"},  //
    {"update", "updates"},
    {"delete", "deletes"},
};

bool isArrayOfObjects(BSONElement array) {
    if (array.type() != Array)
        return false;

    for (auto elem : array.Obj()) {
        if (elem.type() != Object)
            return false;
    }

    return true;
}
}

OpMsgRequest upconvertRequest(StringData db, BSONObj cmdObj, int queryFlags) {
    cmdObj = cmdObj.getOwned();  // Usually this is a no-op since it is already owned.

    auto readPrefContainer = BSONObj();
    const StringData firstFieldName = cmdObj.firstElementFieldName();
    if (firstFieldName == "$query" || firstFieldName == "query") {
        // Commands sent over OP_QUERY specify read preference by putting it at the top level and
        // putting the command in a nested field called either query or $query.

        // Check if legacyCommand has an invalid $maxTimeMS option.
        uassert(ErrorCodes::InvalidOptions,
                "cannot use $maxTimeMS query option with commands; use maxTimeMS command option "
                "instead",
                !cmdObj.hasField("$maxTimeMS"));

        if (auto readPref = cmdObj["$readPreference"])
            readPrefContainer = readPref.wrap();

        cmdObj = cmdObj.firstElement().Obj().shareOwnershipWith(cmdObj);
    } else if (auto queryOptions = cmdObj["$queryOptions"]) {
        // Mongos rewrites commands with $readPreference to put it in a field nested inside of
        // $queryOptions. Its command implementations often forward commands in that format to
        // shards. This function is responsible for rewriting it to a format that the shards
        // understand.
        readPrefContainer = queryOptions.Obj().shareOwnershipWith(cmdObj);
        cmdObj = cmdObj.removeField("$queryOptions");
    }

    if (!readPrefContainer.isEmpty()) {
        cmdObj = BSONObjBuilder(std::move(cmdObj)).appendElements(readPrefContainer).obj();
    } else if (!cmdObj.hasField("$readPreference") && (queryFlags & QueryOption_SlaveOk)) {
        BSONObjBuilder bodyBuilder(std::move(cmdObj));
        ReadPreferenceSetting(ReadPreference::SecondaryPreferred).toContainingBSON(&bodyBuilder);
        cmdObj = bodyBuilder.obj();
    }

    uassert(40621, "$db is not allowed in OP_QUERY requests", !cmdObj.hasField("$db"));

    // Try to move supported array fields into document sequences.
    auto docSequenceIt = docSequenceFieldsForCommands.find(cmdObj.firstElementFieldName());
    auto docSequenceElem = docSequenceIt == docSequenceFieldsForCommands.end()
        ? BSONElement()
        : cmdObj[docSequenceIt->second];
    if (!isArrayOfObjects(docSequenceElem))
        return OpMsgRequest::fromDBAndBody(db, std::move(cmdObj));

    auto docSequenceName = docSequenceElem.fieldNameStringData();

    // Note: removing field before adding "$db" to avoid the need to copy the potentially large
    // array.
    auto out = OpMsgRequest::fromDBAndBody(db, cmdObj.removeField(docSequenceName));
    out.sequences.push_back({docSequenceName.toString()});
    for (auto elem : docSequenceElem.Obj()) {
        out.sequences[0].objs.push_back(elem.Obj().shareOwnershipWith(cmdObj));
    }
    return out;
}

}  // namespace rpc
}  // namespace mongo
