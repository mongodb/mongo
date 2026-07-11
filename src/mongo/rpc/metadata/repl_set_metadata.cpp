// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/rpc/metadata/repl_set_metadata.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/repl/bson_extract_optime.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <utility>

#include <boost/move/utility_core.hpp>

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

ReplSetMetadata::ReplSetMetadata(std::int64_t term,
                                 OpTimeAndWallTime committedOpTime,
                                 OpTime visibleOpTime,
                                 std::int64_t configVersion,
                                 std::int64_t configTerm,
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
        metadataObj, rpc::kReplSetMetadataFieldName, BSONType::object, &replMetadataElement);
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
    if (BSONElement e = replMetadataObj.getField(kReplicaSetIdFieldName); !e.eoo()) {
        if (e.type() != BSONType::oid)
            return Status(ErrorCodes::TypeMismatch,
                          fmt::format("{:?} had the wrong type. Expected {}, found {}",
                                      kReplicaSetIdFieldName,
                                      BSONType::oid,
                                      typeName(e.type())));
        id = e.OID();
    }

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
        replMetadataObj, kLastCommittedWallFieldName, BSONType::date, &wallClockTimeElement);

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
    _lastOpCommitted.opTime.append(kLastOpCommittedFieldName, &replMetadataBuilder);
    replMetadataBuilder.appendDate(kLastCommittedWallFieldName, _lastOpCommitted.wallTime);
    _lastOpVisible.append(kLastOpVisibleFieldName, &replMetadataBuilder);
    replMetadataBuilder.append(kConfigVersionFieldName, _configVersion);
    replMetadataBuilder.append(kConfigTermFieldName, _configTerm);
    replMetadataBuilder.append(kReplicaSetIdFieldName, _replicaSetId);
    replMetadataBuilder.append(kSyncSourceIndexFieldName, _currentSyncSourceIndex);
    replMetadataBuilder.append(kIsPrimaryFieldName, _isPrimary);
    replMetadataBuilder.doneFast();

    return Status::OK();
}

void ReplSetMetadata::appendTermOnly(BSONObjBuilder* builder, std::int64_t term) {
    BSONObjBuilder replDataBuilder(builder->subobjStart(kReplSetMetadataFieldName));
    replDataBuilder.append(kTermFieldName, term);
    replDataBuilder.doneFast();
}

boost::optional<std::int64_t> ReplSetMetadata::readTermOnly(const BSONObj& reply) {
    auto replDataElem = reply[kReplSetMetadataFieldName];
    if (!replDataElem.isABSONObj()) {
        return boost::none;
    }
    auto termElem = replDataElem.Obj()[kTermFieldName];
    if (!termElem.isNumber()) {
        return boost::none;
    }
    return termElem.safeNumberLong();
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
