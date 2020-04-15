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

#include "mongo/rpc/metadata/repl_set_metadata.h"

#include "mongo/bson/util/bson_check.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/bson_extract_optime.h"
#include "mongo/rpc/metadata.h"

namespace mongo {
namespace rpc {

using repl::OpTime;
using repl::OpTimeAndWallTime;

const char kReplSetMetadataFieldName[] = "$replData";

namespace {

const char kLastOpCommittedFieldName[] = "lastOpCommitted";
const char kLastCommittedWallFieldName[] = "lastCommittedWall";
const char kLastOpVisibleFieldName[] = "lastOpVisible";
const char kConfigVersionFieldName[] = "configVersion";
const char kConfigTermFieldName[] = "configTerm";
const char kReplicaSetIdFieldName[] = "replicaSetId";
const char kPrimaryIndexFieldName[] = "primaryIndex";
const char kSyncSourceIndexFieldName[] = "syncSourceIndex";
const char kTermFieldName[] = "term";
const char kIsPrimaryFieldName[] = "isPrimary";

}  // unnamed namespace

ReplSetMetadata::ReplSetMetadata(long long term,
                                 OpTimeAndWallTime committedOpTime,
                                 OpTime visibleOpTime,
                                 long long configVersion,
                                 long long configTerm,
                                 OID id,
                                 int currentSyncSourceIndex,
                                 bool isPrimary)
    : _lastOpCommitted(std::move(committedOpTime)),
      _lastOpVisible(std::move(visibleOpTime)),
      _currentTerm(term),
      _configVersion(configVersion),
      _configTerm(configTerm),
      _replicaSetId(id),
      _currentSyncSourceIndex(currentSyncSourceIndex),
      _isPrimary(isPrimary) {}

StatusWith<ReplSetMetadata> ReplSetMetadata::readFromMetadata(const BSONObj& metadataObj) {
    BSONElement replMetadataElement;

    Status status = bsonExtractTypedField(
        metadataObj, rpc::kReplSetMetadataFieldName, Object, &replMetadataElement);
    if (!status.isOK())
        return status;
    BSONObj replMetadataObj = replMetadataElement.Obj();

    long long configVersion;
    status = bsonExtractIntegerField(replMetadataObj, kConfigVersionFieldName, &configVersion);
    if (!status.isOK())
        return status;

    long long configTerm;
    status = bsonExtractIntegerField(replMetadataObj, kConfigTermFieldName, &configTerm);
    if (!status.isOK())
        return status;

    OID id;
    status = bsonExtractOIDFieldWithDefault(replMetadataObj, kReplicaSetIdFieldName, OID(), &id);
    if (!status.isOK())
        return status;

    // We provide a default because these fields will be removed in SERVER-27668.
    long long primaryIndex;
    status = bsonExtractIntegerFieldWithDefault(
        replMetadataObj, kPrimaryIndexFieldName, -1, &primaryIndex);
    if (!status.isOK())
        return status;

    long long syncSourceIndex;
    status = bsonExtractIntegerFieldWithDefault(
        replMetadataObj, kSyncSourceIndexFieldName, -1, &syncSourceIndex);
    if (!status.isOK())
        return status;

    bool isPrimary;
    status = bsonExtractBooleanField(replMetadataObj, kIsPrimaryFieldName, &isPrimary);
    if (!status.isOK())
        return status;

    long long term;
    status = bsonExtractIntegerField(replMetadataObj, kTermFieldName, &term);
    if (!status.isOK())
        return status;

    repl::OpTimeAndWallTime lastOpCommitted;
    auto lastCommittedStatus = bsonExtractOpTimeField(
        replMetadataObj, kLastOpCommittedFieldName, &(lastOpCommitted.opTime));
    // We check for NoSuchKey because these fields will be removed in SERVER-27668.
    if (!lastCommittedStatus.isOK() && lastCommittedStatus != ErrorCodes::NoSuchKey)
        return lastCommittedStatus;

    repl::OpTime lastOpVisible;
    status = bsonExtractOpTimeField(replMetadataObj, kLastOpVisibleFieldName, &lastOpVisible);
    if (!status.isOK() && status != ErrorCodes::NoSuchKey)
        return status;

    BSONElement wallClockTimeElement;
    status = bsonExtractTypedField(
        replMetadataObj, kLastCommittedWallFieldName, BSONType::Date, &wallClockTimeElement);

    if (!status.isOK()) {
        return status;
    }

    lastOpCommitted.wallTime = wallClockTimeElement.Date();
    return ReplSetMetadata(term,
                           lastOpCommitted,
                           lastOpVisible,
                           configVersion,
                           configTerm,
                           id,
                           syncSourceIndex,
                           isPrimary);
}

Status ReplSetMetadata::writeToMetadata(BSONObjBuilder* builder) const {
    BSONObjBuilder replMetadataBuilder(builder->subobjStart(kReplSetMetadataFieldName));
    replMetadataBuilder.append(kTermFieldName, _currentTerm);
    _lastOpCommitted.opTime.append(&replMetadataBuilder, kLastOpCommittedFieldName);
    replMetadataBuilder.appendDate(kLastCommittedWallFieldName, _lastOpCommitted.wallTime);
    _lastOpVisible.append(&replMetadataBuilder, kLastOpVisibleFieldName);
    replMetadataBuilder.append(kConfigVersionFieldName, _configVersion);
    replMetadataBuilder.append(kConfigTermFieldName, _configTerm);
    replMetadataBuilder.append(kReplicaSetIdFieldName, _replicaSetId);
    replMetadataBuilder.append(kSyncSourceIndexFieldName, _currentSyncSourceIndex);
    replMetadataBuilder.append(kIsPrimaryFieldName, _isPrimary);
    replMetadataBuilder.doneFast();

    return Status::OK();
}

std::string ReplSetMetadata::toString() const {
    str::stream output;
    output << "ReplSetMetadata";
    output << " Config Version: " << _configVersion;
    output << " Config Term: " << _configTerm;
    output << " Replicaset ID: " << _replicaSetId;
    output << " Term: " << _currentTerm;
    output << " Sync Source Index: " << _currentSyncSourceIndex;
    output << " Is Primary: " << _isPrimary;
    output << " Last Op Committed: " << _lastOpCommitted.toString();
    output << " Last Op Visible: " << _lastOpVisible.toString();
    return output;
}

}  // namespace rpc
}  // namespace mongo
