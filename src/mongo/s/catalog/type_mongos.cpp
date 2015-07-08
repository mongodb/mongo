/**
 *    Copyright (C) 2012 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */
#include "mongo/s/catalog/type_mongos.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
const std::string MongosType::ConfigNS = "config.mongos";

const BSONField<std::string> MongosType::name("_id");
const BSONField<Date_t> MongosType::ping("ping");
const BSONField<long long> MongosType::uptime("up");
const BSONField<bool> MongosType::waiting("waiting");
const BSONField<std::string> MongosType::mongoVersion("mongoVersion");
const BSONField<long long> MongosType::configVersion("configVersion");

StatusWith<MongosType> MongosType::fromBSON(const BSONObj& source) {
    MongosType mt;

    {
        std::string mtName;
        Status status = bsonExtractStringField(source, name.name(), &mtName);
        if (!status.isOK())
            return status;
        mt._name = mtName;
    }

    {
        BSONElement mtPingElem;
        Status status = bsonExtractTypedField(source, ping.name(), BSONType::Date, &mtPingElem);
        if (!status.isOK())
            return status;
        mt._ping = mtPingElem.date();
    }

    {
        long long mtUptime;
        Status status = bsonExtractIntegerField(source, uptime.name(), &mtUptime);
        if (!status.isOK())
            return status;
        mt._uptime = mtUptime;
    }

    {
        bool mtWaiting;
        Status status = bsonExtractBooleanField(source, waiting.name(), &mtWaiting);
        if (!status.isOK())
            return status;
        mt._waiting = mtWaiting;
    }

    if (source.hasField(mongoVersion.name())) {
        std::string mtMongoVersion;
        Status status = bsonExtractStringField(source, mongoVersion.name(), &mtMongoVersion);
        if (!status.isOK())
            return status;
        mt._mongoVersion = mtMongoVersion;
    }

    if (source.hasField(configVersion.name())) {
        long long mtConfigVersion;
        Status status = bsonExtractIntegerField(source, configVersion.name(), &mtConfigVersion);
        if (!status.isOK())
            return status;
        mt._configVersion = mtConfigVersion;
    }

    return mt;
}

Status MongosType::validate() const {
    if (!_name.is_initialized() || _name->empty()) {
        return {ErrorCodes::NoSuchKey, str::stream() << "missing " << name.name() << " field"};
    }

    if (!_ping.is_initialized()) {
        return {ErrorCodes::NoSuchKey, str::stream() << "missing " << ping.name() << " field"};
    }

    if (!_uptime.is_initialized()) {
        return {ErrorCodes::NoSuchKey, str::stream() << "missing " << uptime.name() << " field"};
    }

    if (!_waiting.is_initialized()) {
        return {ErrorCodes::NoSuchKey, str::stream() << "missing " << waiting.name() << " field"};
    }

    return Status::OK();
}

BSONObj MongosType::toBSON() const {
    BSONObjBuilder builder;

    if (_name)
        builder.append(name.name(), getName());
    if (_ping)
        builder.append(ping.name(), getPing());
    if (_uptime)
        builder.append(uptime.name(), getUptime());
    if (_waiting)
        builder.append(waiting.name(), getWaiting());
    if (_mongoVersion)
        builder.append(mongoVersion.name(), getMongoVersion());
    if (_configVersion)
        builder.append(configVersion.name(), getConfigVersion());

    return builder.obj();
}

void MongosType::setName(const std::string& name) {
    invariant(!name.empty());
    _name = name;
}

void MongosType::setPing(const Date_t& ping) {
    _ping = ping;
}

void MongosType::setUptime(long long uptime) {
    _uptime = uptime;
}

void MongosType::setWaiting(bool waiting) {
    _waiting = waiting;
}

void MongosType::setMongoVersion(const std::string& mongoVersion) {
    invariant(!mongoVersion.empty());
    _mongoVersion = mongoVersion;
}

void MongosType::setConfigVersion(const long long configVersion) {
    _configVersion = configVersion;
}

std::string MongosType::toString() const {
    return toBSON().toString();
}

}  // namespace mongo
