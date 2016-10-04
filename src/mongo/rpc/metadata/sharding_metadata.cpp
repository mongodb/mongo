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

#include "mongo/rpc/metadata/sharding_metadata.h"

#include <utility>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/bson_extract_optime.h"
#include "mongo/db/repl/optime.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace rpc {

namespace {

const char kGLEStatsFieldName[] = "$gleStats";
const char kGLEStatsLastOpTimeFieldName[] = "lastOpTime";
const char kGLEStatsElectionIdFieldName[] = "electionId";

}  // namespace

StatusWith<ShardingMetadata> ShardingMetadata::readFromMetadata(const BSONObj& metadataObj) {
    BSONElement smElem;
    auto smExtractStatus =
        bsonExtractTypedField(metadataObj, kGLEStatsFieldName, mongo::Object, &smElem);
    if (!smExtractStatus.isOK()) {
        return smExtractStatus;
    }

    if (smElem.embeddedObject().nFields() != 2) {
        return Status(ErrorCodes::InvalidOptions,
                      str::stream() << "The $gleStats object can only have 2 fields, but got "
                                    << smElem.embeddedObject().toString());
    }

    repl::OpTime opTime;
    const BSONElement opTimeElement = smElem.embeddedObject()[kGLEStatsLastOpTimeFieldName];
    if (opTimeElement.eoo()) {
        return Status(ErrorCodes::NoSuchKey, "lastOpTime field missing");
    } else if (opTimeElement.type() == bsonTimestamp) {
        opTime = repl::OpTime(opTimeElement.timestamp(), repl::OpTime::kUninitializedTerm);
    } else if (opTimeElement.type() == Date) {
        opTime = repl::OpTime(Timestamp(opTimeElement.date()), repl::OpTime::kUninitializedTerm);
    } else if (opTimeElement.type() == Object) {
        Status status =
            bsonExtractOpTimeField(smElem.embeddedObject(), kGLEStatsLastOpTimeFieldName, &opTime);
        if (!status.isOK()) {
            return status;
        }
    } else {
        return Status(ErrorCodes::TypeMismatch,
                      str::stream() << "Expected \"" << kGLEStatsLastOpTimeFieldName
                                    << "\" field in response to replSetHeartbeat "
                                       "command to have type Date or Timestamp, but found type "
                                    << typeName(opTimeElement.type()));
    }

    BSONElement lastElectionIdElem;
    auto lastElectionIdExtractStatus = bsonExtractTypedField(
        smElem.embeddedObject(), kGLEStatsElectionIdFieldName, mongo::jstOID, &lastElectionIdElem);
    if (!lastElectionIdExtractStatus.isOK()) {
        return lastElectionIdExtractStatus;
    }

    return ShardingMetadata(opTime, lastElectionIdElem.OID());
}

Status ShardingMetadata::writeToMetadata(BSONObjBuilder* metadataBob) const {
    BSONObjBuilder subobj(metadataBob->subobjStart(kGLEStatsFieldName));
    if (getLastOpTime().getTerm() > repl::OpTime::kUninitializedTerm) {
        getLastOpTime().append(&subobj, kGLEStatsLastOpTimeFieldName);
    } else {
        subobj.append(kGLEStatsLastOpTimeFieldName, getLastOpTime().getTimestamp());
    }
    subobj.append(kGLEStatsElectionIdFieldName, getLastElectionId());
    return Status::OK();
}

Status ShardingMetadata::downconvert(const BSONObj& commandReply,
                                     const BSONObj& replyMetadata,
                                     BSONObjBuilder* legacyCommandReplyBob) {
    legacyCommandReplyBob->appendElements(commandReply);

    auto swShardingMetadata = readFromMetadata(replyMetadata);
    if (swShardingMetadata.isOK()) {
        // We can reuse the same logic to write the sharding metadata out to the legacy
        // command as the element has the same format whether it is there or on the metadata
        // object.
        swShardingMetadata.getValue().writeToMetadata(legacyCommandReplyBob);
    } else if (swShardingMetadata.getStatus() == ErrorCodes::NoSuchKey) {
        // It is valid to not have a $gleStats field.
    } else {
        return swShardingMetadata.getStatus();
    }
    return Status::OK();
}

Status ShardingMetadata::upconvert(const BSONObj& legacyCommand,
                                   BSONObjBuilder* commandBob,
                                   BSONObjBuilder* metadataBob) {
    // We can reuse the same logic to read the sharding metadata out from the legacy command
    // as it has the same format whether it is there or on the metadata object.
    auto swShardingMetadata = readFromMetadata(legacyCommand);
    if (swShardingMetadata.isOK()) {
        swShardingMetadata.getValue().writeToMetadata(metadataBob);

        // Write out the command excluding the $gleStats subobject.
        for (const auto& elem : legacyCommand) {
            if (elem.fieldNameStringData() != StringData(kGLEStatsFieldName)) {
                commandBob->append(elem);
            }
        }
    } else if (swShardingMetadata.getStatus() == ErrorCodes::NoSuchKey) {
        // it is valid to not have a $gleStats field
        commandBob->appendElements(legacyCommand);
    } else {
        return swShardingMetadata.getStatus();
    }
    return Status::OK();
}

ShardingMetadata::ShardingMetadata(repl::OpTime lastOpTime, OID lastElectionId)
    : _lastOpTime(std::move(lastOpTime)), _lastElectionId(std::move(lastElectionId)) {}

const repl::OpTime& ShardingMetadata::getLastOpTime() const {
    return _lastOpTime;
}

const OID& ShardingMetadata::getLastElectionId() const {
    return _lastElectionId;
}

}  // namespace rpc
}  // namespace mongo
