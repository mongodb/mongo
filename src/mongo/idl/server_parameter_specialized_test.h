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

#pragma once

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/logical_time.h"
#include "mongo/stdx/mutex.h"

namespace mongo {
namespace test {

struct ExtraDataForServerParameter {
    std::string value = "start value";
    bool flag = true;
};

class SpecializedClusterServerParameterData {
public:
    SpecializedClusterServerParameterData()
        : _clusterParameterTime(), _strData("default"), _intData(30){};

    SpecializedClusterServerParameterData(const std::string& newStrData, std::uint32_t newIntData)
        : _strData(newStrData), _intData(newIntData) {}

    LogicalTime getClusterParameterTime() const {
        stdx::lock_guard<Latch> lg(_mutex);
        return _clusterParameterTime;
    }

    StringData getStrData() const {
        stdx::lock_guard<Latch> lg(_mutex);
        return _strData;
    }

    std::uint32_t getIntData() const {
        stdx::lock_guard<Latch> lg(_mutex);
        return _intData;
    }

    void setClusterParameterTime(const LogicalTime& clusterParameterTime) {
        stdx::lock_guard<Latch> lg(_mutex);
        _clusterParameterTime = clusterParameterTime;
    }

    void setStrData(const std::string& strData) {
        stdx::lock_guard<Latch> lg(_mutex);
        _strData = strData;
    }

    void setIntData(std::int32_t intData) {
        stdx::lock_guard<Latch> lg(_mutex);
        _intData = intData;
    }

    void setId(StringData id) {
        stdx::lock_guard<Latch> lg(_mutex);
        _id = id.toString();
    }

    void parse(const BSONObj& updatedObj) {
        stdx::lock_guard<Latch> lg(_mutex);
        _clusterParameterTime = LogicalTime(updatedObj["clusterParameterTime"].timestamp());
        _strData = updatedObj["strData"].String();
        _intData = updatedObj["intData"].Int();
    }

    void serialize(BSONObjBuilder* builder) const {
        stdx::lock_guard<Latch> lg(_mutex);
        if (_id.is_initialized()) {
            builder->append("_id"_sd, _id.get());
        }
        builder->append("clusterParameterTime"_sd, _clusterParameterTime.asTimestamp());
        builder->append("strData"_sd, _strData);
        builder->append("intData"_sd, _intData);
    }

    BSONObj toBSON() const {
        BSONObjBuilder builder;
        serialize(&builder);
        return builder.obj();
    }

    void reset() {
        stdx::lock_guard<Latch> lg(_mutex);
        _clusterParameterTime = LogicalTime();
        _strData = "default";
        _intData = 30;
        _id = boost::none;
    }

private:
    boost::optional<std::string> _id;
    LogicalTime _clusterParameterTime;
    std::string _strData;
    std::int32_t _intData;

    mutable Mutex _mutex = MONGO_MAKE_LATCH("SpecializedClusterServerParameterStorage::_mutex");
};

}  // namespace test
}  // namespace mongo
