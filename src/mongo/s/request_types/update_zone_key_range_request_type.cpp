/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/s/request_types/update_zone_key_range_request_type.h"

#include "mongo/bson/bson_field.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/write_concern_options.h"

namespace mongo {

using std::string;

namespace {

const char kMongosUpdateZoneKeyRange[] = "updateZoneKeyRange";
const char kConfigsvrUpdateZoneKeyRange[] = "_configsvrUpdateZoneKeyRange";
const char kZoneName[] = "zone";

}  // unnamed namespace

StatusWith<UpdateZoneKeyRangeRequest> UpdateZoneKeyRangeRequest::parseFromMongosCommand(
    const BSONObj& cmdObj) {
    return _parseFromCommand(cmdObj, true);
}

StatusWith<UpdateZoneKeyRangeRequest> UpdateZoneKeyRangeRequest::parseFromConfigCommand(
    const BSONObj& cmdObj) {
    return _parseFromCommand(cmdObj, false);
}

void UpdateZoneKeyRangeRequest::appendAsConfigCommand(BSONObjBuilder* cmdBuilder) {
    cmdBuilder->append(kConfigsvrUpdateZoneKeyRange, _ns.ns());
    _range.append(cmdBuilder);

    if (_isRemove) {
        cmdBuilder->appendNull(kZoneName);
    } else {
        cmdBuilder->append(kZoneName, _zoneName);
    }
}

StatusWith<UpdateZoneKeyRangeRequest> UpdateZoneKeyRangeRequest::_parseFromCommand(
    const BSONObj& cmdObj, bool forMongos) {
    string rawNS;
    auto parseNamespaceStatus = bsonExtractStringField(
        cmdObj, (forMongos ? kMongosUpdateZoneKeyRange : kConfigsvrUpdateZoneKeyRange), &rawNS);

    if (!parseNamespaceStatus.isOK()) {
        return parseNamespaceStatus;
    }

    NamespaceString ns(rawNS);

    if (!ns.isValid()) {
        return {ErrorCodes::InvalidNamespace,
                str::stream() << rawNS << " is not a valid namespace"};
    }

    auto parseRangeStatus = ChunkRange::fromBSON(cmdObj);
    if (!parseRangeStatus.isOK()) {
        return parseRangeStatus.getStatus();
    }

    BSONElement zoneElem;

    auto parseZoneNameStatus = bsonExtractField(cmdObj, kZoneName, &zoneElem);

    if (!parseZoneNameStatus.isOK()) {
        return parseZoneNameStatus;
    }

    bool isRemove = false;
    string zoneName;
    if (zoneElem.type() == String) {
        zoneName = zoneElem.str();
    } else if (zoneElem.isNull()) {
        isRemove = true;
    } else {
        return {ErrorCodes::TypeMismatch,
                mongoutils::str::stream() << "\"" << kZoneName << "\" had the wrong type. Expected "
                                          << typeName(String)
                                          << " or "
                                          << typeName(jstNULL)
                                          << ", found "
                                          << typeName(zoneElem.type())};
    }

    if (isRemove) {
        return UpdateZoneKeyRangeRequest(std::move(ns), std::move(parseRangeStatus.getValue()));
    }

    return UpdateZoneKeyRangeRequest(
        std::move(ns), std::move(parseRangeStatus.getValue()), std::move(zoneName));
}

const NamespaceString& UpdateZoneKeyRangeRequest::getNS() const {
    return _ns;
}

const ChunkRange& UpdateZoneKeyRangeRequest::getRange() const {
    return _range;
}

bool UpdateZoneKeyRangeRequest::isRemove() const {
    return _isRemove;
}

const string& UpdateZoneKeyRangeRequest::getZoneName() const {
    invariant(!_isRemove);
    return _zoneName;
}

UpdateZoneKeyRangeRequest::UpdateZoneKeyRangeRequest(NamespaceString ns, ChunkRange range)
    : _ns(std::move(ns)), _range(std::move(range)), _isRemove(true) {}

UpdateZoneKeyRangeRequest::UpdateZoneKeyRangeRequest(NamespaceString ns,
                                                     ChunkRange range,
                                                     std::string zoneName)
    : _ns(std::move(ns)),
      _range(std::move(range)),
      _isRemove(false),
      _zoneName(std::move(zoneName)) {}

}  // namespace mongo
