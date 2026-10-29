// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/ddl/remove_shard_from_zone_request_type.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"

#include <utility>

#include <boost/move/utility_core.hpp>

namespace mongo {

using std::string;

namespace {

const char kMongosRemoveShardFromZone[] = "removeShardFromZone";
const char kConfigsvrRemoveShardFromZone[] = "_configsvrRemoveShardFromZone";
const char kZoneName[] = "zone";

}  // unnamed namespace

StatusWith<RemoveShardFromZoneRequest> RemoveShardFromZoneRequest::parseFromMongosCommand(
    const BSONObj& cmdObj) {
    return _parseFromCommand(cmdObj, true);
}

StatusWith<RemoveShardFromZoneRequest> RemoveShardFromZoneRequest::parseFromConfigCommand(
    const BSONObj& cmdObj) {
    return _parseFromCommand(cmdObj, false);
}

void RemoveShardFromZoneRequest::appendAsConfigCommand(BSONObjBuilder* cmdBuilder) {
    cmdBuilder->append(kConfigsvrRemoveShardFromZone, _shardName);
    cmdBuilder->append(kZoneName, _zoneName);
}

RemoveShardFromZoneRequest::RemoveShardFromZoneRequest(string shardName, string zoneName)
    : _shardName(std::move(shardName)), _zoneName(std::move(zoneName)) {}

StatusWith<RemoveShardFromZoneRequest> RemoveShardFromZoneRequest::_parseFromCommand(
    const BSONObj& cmdObj, bool forMongos) {
    string shardName;
    auto parseShardNameStatus = bsonExtractStringField(
        cmdObj,
        (forMongos ? kMongosRemoveShardFromZone : kConfigsvrRemoveShardFromZone),
        &shardName);

    if (!parseShardNameStatus.isOK()) {
        return parseShardNameStatus;
    }

    string zoneName;
    auto parseZoneNameStatus = bsonExtractStringField(cmdObj, kZoneName, &zoneName);

    if (!parseZoneNameStatus.isOK()) {
        return parseZoneNameStatus;
    }

    return RemoveShardFromZoneRequest(std::move(shardName), std::move(zoneName));
}

const string& RemoveShardFromZoneRequest::getShardName() const {
    return _shardName;
}

const string& RemoveShardFromZoneRequest::getZoneName() const {
    return _zoneName;
}

}  // namespace mongo
