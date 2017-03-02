/**
 *    Copyright (C) 2016 BongoDB Inc.
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

#include "bongo/platform/basic.h"

#include "bongo/s/request_types/remove_shard_from_zone_request_type.h"

#include "bongo/bson/bson_field.h"
#include "bongo/bson/util/bson_extract.h"
#include "bongo/db/write_concern_options.h"

namespace bongo {

using std::string;

namespace {

const char kBongosRemoveShardFromZone[] = "removeShardFromZone";
const char kConfigsvrRemoveShardFromZone[] = "_configsvrRemoveShardFromZone";
const char kZoneName[] = "zone";

}  // unnamed namespace

StatusWith<RemoveShardFromZoneRequest> RemoveShardFromZoneRequest::parseFromBongosCommand(
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
    const BSONObj& cmdObj, bool forBongos) {
    string shardName;
    auto parseShardNameStatus = bsonExtractStringField(
        cmdObj,
        (forBongos ? kBongosRemoveShardFromZone : kConfigsvrRemoveShardFromZone),
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

}  // namespace bongo
