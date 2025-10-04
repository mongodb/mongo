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
#include "mongo/db/global_catalog/type_mongos.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
const NamespaceString MongosType::ConfigNS(NamespaceString::kConfigMongosNamespace);

const BSONField<std::string> MongosType::name("_id");
const BSONField<Date_t> MongosType::created("created");
const BSONField<Date_t> MongosType::ping("ping");
const BSONField<long long> MongosType::uptime("up");
const BSONField<bool> MongosType::waiting("waiting");
const BSONField<std::string> MongosType::mongoVersion("mongoVersion");
const BSONField<long long> MongosType::configVersion("configVersion");
const BSONField<BSONArray> MongosType::advisoryHostFQDNs("advisoryHostFQDNs");

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
        Status status = bsonExtractTypedField(source, ping.name(), BSONType::date, &mtPingElem);
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

    if (source.hasField(created.name())) {
        BSONElement mtCreatedElem;
        Status status =
            bsonExtractTypedField(source, created.name(), BSONType::date, &mtCreatedElem);
        if (!status.isOK())
            return status;
        mt._created = mtCreatedElem.date();
    }

    if (source.hasField(configVersion.name())) {
        long long mtConfigVersion;
        Status status = bsonExtractIntegerField(source, configVersion.name(), &mtConfigVersion);
        if (!status.isOK())
            return status;
        mt._configVersion = mtConfigVersion;
    }

    if (source.hasField(advisoryHostFQDNs.name())) {
        mt._advisoryHostFQDNs = std::vector<std::string>();
        BSONElement array;
        Status status =
            bsonExtractTypedField(source, advisoryHostFQDNs.name(), BSONType::array, &array);
        if (!status.isOK())
            return status;

        BSONObjIterator it(array.Obj());
        while (it.more()) {
            BSONElement arrayElement = it.next();
            if (arrayElement.type() != BSONType::string) {
                return Status(ErrorCodes::TypeMismatch,
                              str::stream() << "Elements in \"" << advisoryHostFQDNs.name()
                                            << "\" array must be strings but found "
                                            << typeName(arrayElement.type()));
            }
            mt._advisoryHostFQDNs->push_back(arrayElement.String());
        }
    }

    return mt;
}

Status MongosType::validate() const {
    if (!_name.has_value() || _name->empty()) {
        return {ErrorCodes::NoSuchKey, str::stream() << "missing " << name.name() << " field"};
    }

    if (!_ping.has_value()) {
        return {ErrorCodes::NoSuchKey, str::stream() << "missing " << ping.name() << " field"};
    }

    if (!_uptime.has_value()) {
        return {ErrorCodes::NoSuchKey, str::stream() << "missing " << uptime.name() << " field"};
    }

    if (!_waiting.has_value()) {
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
    if (_created)
        builder.append(created.name(), getCreated());
    if (_configVersion)
        builder.append(configVersion.name(), getConfigVersion());
    if (_advisoryHostFQDNs)
        builder.append(advisoryHostFQDNs.name(), getAdvisoryHostFQDNs());

    return builder.obj();
}

void MongosType::setName(const std::string& name) {
    invariant(!name.empty());
    _name = name;
}

void MongosType::setCreated(const Date_t& created) {
    _created = created;
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

void MongosType::setAdvisoryHostFQDNs(const std::vector<std::string>& advisoryHostFQDNs) {
    _advisoryHostFQDNs = advisoryHostFQDNs;
}

std::string MongosType::toString() const {
    return toBSON().toString();
}

}  // namespace mongo
