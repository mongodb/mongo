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

#include "mongo/rpc/metadata/oplog_query_metadata.h"

#include "mongo/bson/util/bson_check.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/bson_extract_optime.h"
#include "mongo/rpc/metadata.h"

namespace mongo {
namespace rpc {

using repl::OpTime;
using repl::OpTimeAndWallTime;

const char kOplogQueryMetadataFieldName[] = "$oplogQueryData";

namespace {

const char kLastOpCommittedFieldName[] = "lastOpCommitted";
const char kLastCommittedWallFieldName[] = "lastCommittedWall";
const char kLastOpAppliedFieldName[] = "lastOpApplied";
const char kPrimaryIndexFieldName[] = "primaryIndex";
const char kSyncSourceIndexFieldName[] = "syncSourceIndex";
const char kSyncSourceHostFieldName[] = "syncSourceHost";
const char kRBIDFieldName[] = "rbid";

}  // unnamed namespace

const int OplogQueryMetadata::kNoPrimary;

OplogQueryMetadata::OplogQueryMetadata(OpTimeAndWallTime lastOpCommitted,
                                       OpTime lastOpApplied,
                                       int rbid,
                                       int currentPrimaryIndex,
                                       int currentSyncSourceIndex,
                                       std::string currentSyncSourceHost)
    : _lastOpCommitted(std::move(lastOpCommitted)),
      _lastOpApplied(std::move(lastOpApplied)),
      _rbid(rbid),
      _currentPrimaryIndex(currentPrimaryIndex),
      _currentSyncSourceIndex(currentSyncSourceIndex),
      _currentSyncSourceHost(currentSyncSourceHost) {}

StatusWith<OplogQueryMetadata> OplogQueryMetadata::readFromMetadata(const BSONObj& metadataObj,
                                                                    bool requireWallTime) {
    BSONElement oqMetadataElement;

    Status status = bsonExtractTypedField(
        metadataObj, rpc::kOplogQueryMetadataFieldName, Object, &oqMetadataElement);
    if (!status.isOK())
        return status;
    BSONObj oqMetadataObj = oqMetadataElement.Obj();

    long long primaryIndex;
    status = bsonExtractIntegerField(oqMetadataObj, kPrimaryIndexFieldName, &primaryIndex);
    if (!status.isOK())
        return status;

    long long syncSourceIndex;
    status = bsonExtractIntegerField(oqMetadataObj, kSyncSourceIndexFieldName, &syncSourceIndex);
    if (!status.isOK())
        return status;

    std::string syncSourceHost;
    status = bsonExtractStringField(oqMetadataObj, kSyncSourceHostFieldName, &syncSourceHost);
    // SyncSourceHost might not be set in older versions, checking NoSuchKey error
    // for backward compatibility.
    // TODO SERVER-59732: Remove the compatibility check once 6.0 is released.
    if (!status.isOK() && status.code() != ErrorCodes::NoSuchKey)
        return status;

    long long rbid;
    status = bsonExtractIntegerField(oqMetadataObj, kRBIDFieldName, &rbid);
    if (!status.isOK())
        return status;

    repl::OpTimeAndWallTime lastOpCommitted;
    status =
        bsonExtractOpTimeField(oqMetadataObj, kLastOpCommittedFieldName, &(lastOpCommitted.opTime));
    if (!status.isOK())
        return status;

    BSONElement wallClockTimeElement;
    status = bsonExtractTypedField(
        oqMetadataObj, kLastCommittedWallFieldName, BSONType::Date, &wallClockTimeElement);
    if (!status.isOK() && (status != ErrorCodes::NoSuchKey || requireWallTime))
        return status;
    if (status.isOK())
        lastOpCommitted.wallTime = wallClockTimeElement.Date();

    repl::OpTime lastOpApplied;
    status = bsonExtractOpTimeField(oqMetadataObj, kLastOpAppliedFieldName, &lastOpApplied);
    if (!status.isOK())
        return status;

    return OplogQueryMetadata(
        lastOpCommitted, lastOpApplied, rbid, primaryIndex, syncSourceIndex, syncSourceHost);
}

Status OplogQueryMetadata::writeToMetadata(BSONObjBuilder* builder) const {
    BSONObjBuilder oqMetadataBuilder(builder->subobjStart(kOplogQueryMetadataFieldName));
    _lastOpCommitted.opTime.append(&oqMetadataBuilder, kLastOpCommittedFieldName);
    oqMetadataBuilder.appendDate(kLastCommittedWallFieldName, _lastOpCommitted.wallTime);
    _lastOpApplied.append(&oqMetadataBuilder, kLastOpAppliedFieldName);
    oqMetadataBuilder.append(kRBIDFieldName, _rbid);
    oqMetadataBuilder.append(kPrimaryIndexFieldName, _currentPrimaryIndex);
    oqMetadataBuilder.append(kSyncSourceIndexFieldName, _currentSyncSourceIndex);
    oqMetadataBuilder.append(kSyncSourceHostFieldName, _currentSyncSourceHost);
    oqMetadataBuilder.doneFast();

    return Status::OK();
}

std::string OplogQueryMetadata::toString() const {
    str::stream output;
    output << "OplogQueryMetadata";
    output << " Primary Index: " << _currentPrimaryIndex;
    output << " Sync Source Index: " << _currentSyncSourceIndex;
    output << " Sync Source Host: " << _currentSyncSourceHost;
    output << " RBID: " << _rbid;
    output << " Last Op Committed: " << _lastOpCommitted.toString();
    output << " Last Op Applied: " << _lastOpApplied.toString();
    return output;
}

}  // namespace rpc
}  // namespace mongo
