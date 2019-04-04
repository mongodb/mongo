/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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
#include "merizo/s/catalog/type_merizos.h"

#include "merizo/base/status_with.h"
#include "merizo/bson/bsonobj.h"
#include "merizo/bson/bsonobjbuilder.h"
#include "merizo/bson/util/bson_extract.h"
#include "merizo/util/assert_util.h"
#include "merizo/util/merizoutils/str.h"

namespace merizo {
const NamespaceString MerizosType::ConfigNS("config.merizos");

const BSONField<std::string> MerizosType::name("_id");
const BSONField<Date_t> MerizosType::ping("ping");
const BSONField<long long> MerizosType::uptime("up");
const BSONField<bool> MerizosType::waiting("waiting");
const BSONField<std::string> MerizosType::merizoVersion("merizoVersion");
const BSONField<long long> MerizosType::configVersion("configVersion");
const BSONField<BSONArray> MerizosType::advisoryHostFQDNs("advisoryHostFQDNs");

StatusWith<MerizosType> MerizosType::fromBSON(const BSONObj& source) {
    MerizosType mt;

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

    if (source.hasField(merizoVersion.name())) {
        std::string mtMerizoVersion;
        Status status = bsonExtractStringField(source, merizoVersion.name(), &mtMerizoVersion);
        if (!status.isOK())
            return status;
        mt._merizoVersion = mtMerizoVersion;
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
        Status status = bsonExtractTypedField(source, advisoryHostFQDNs.name(), Array, &array);
        if (!status.isOK())
            return status;

        BSONObjIterator it(array.Obj());
        while (it.more()) {
            BSONElement arrayElement = it.next();
            if (arrayElement.type() != String) {
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

Status MerizosType::validate() const {
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

BSONObj MerizosType::toBSON() const {
    BSONObjBuilder builder;

    if (_name)
        builder.append(name.name(), getName());
    if (_ping)
        builder.append(ping.name(), getPing());
    if (_uptime)
        builder.append(uptime.name(), getUptime());
    if (_waiting)
        builder.append(waiting.name(), getWaiting());
    if (_merizoVersion)
        builder.append(merizoVersion.name(), getMerizoVersion());
    if (_configVersion)
        builder.append(configVersion.name(), getConfigVersion());
    if (_advisoryHostFQDNs)
        builder.append(advisoryHostFQDNs.name(), getAdvisoryHostFQDNs());

    return builder.obj();
}

void MerizosType::setName(const std::string& name) {
    invariant(!name.empty());
    _name = name;
}

void MerizosType::setPing(const Date_t& ping) {
    _ping = ping;
}

void MerizosType::setUptime(long long uptime) {
    _uptime = uptime;
}

void MerizosType::setWaiting(bool waiting) {
    _waiting = waiting;
}

void MerizosType::setMerizoVersion(const std::string& merizoVersion) {
    invariant(!merizoVersion.empty());
    _merizoVersion = merizoVersion;
}

void MerizosType::setConfigVersion(const long long configVersion) {
    _configVersion = configVersion;
}

void MerizosType::setAdvisoryHostFQDNs(const std::vector<std::string>& advisoryHostFQDNs) {
    _advisoryHostFQDNs = advisoryHostFQDNs;
}

std::string MerizosType::toString() const {
    return toBSON().toString();
}

}  // namespace merizo
