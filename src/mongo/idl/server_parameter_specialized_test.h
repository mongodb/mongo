// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/logical_time.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace test {
using namespace std::literals::string_view_literals;

const std::string& getGlobalSCSP();
const std::string& getGlobalSWO();

struct ExtraDataForServerParameter {
    std::string value = "start value";
    bool flag = true;
};

class SpecializedClusterServerParameterData {
public:
    SpecializedClusterServerParameterData()
        : _clusterParameterTime(), _strData("default"), _intData(30) {};

    SpecializedClusterServerParameterData(const std::string& newStrData, std::uint32_t newIntData)
        : _strData(newStrData), _intData(newIntData) {}

    LogicalTime getClusterParameterTime() const {
        std::lock_guard<std::mutex> lg(_mutex);
        return _clusterParameterTime;
    }

    std::string_view getStrData() const {
        std::lock_guard<std::mutex> lg(_mutex);
        return _strData;
    }

    std::uint32_t getIntData() const {
        std::lock_guard<std::mutex> lg(_mutex);
        return _intData;
    }

    void setClusterParameterTime(const LogicalTime& clusterParameterTime) {
        std::lock_guard<std::mutex> lg(_mutex);
        _clusterParameterTime = clusterParameterTime;
    }

    void setStrData(const std::string& strData) {
        std::lock_guard<std::mutex> lg(_mutex);
        _strData = strData;
    }

    void setIntData(std::int32_t intData) {
        std::lock_guard<std::mutex> lg(_mutex);
        _intData = intData;
    }

    void setId(std::string_view id) {
        std::lock_guard<std::mutex> lg(_mutex);
        _id = std::string{id};
    }

    void parse(const BSONObj& updatedObj) {
        std::lock_guard<std::mutex> lg(_mutex);
        _clusterParameterTime = LogicalTime(updatedObj["clusterParameterTime"].timestamp());
        _strData = updatedObj["strData"].String();
        _intData = updatedObj["intData"].Int();
    }

    void serialize(BSONObjBuilder* builder) const {
        std::lock_guard<std::mutex> lg(_mutex);
        if (_id.is_initialized()) {
            builder->append("_id"sv, _id.get());
        }
        builder->append("clusterParameterTime"sv, _clusterParameterTime.asTimestamp());
        builder->append("strData"sv, _strData);
        builder->append("intData"sv, _intData);
    }

    BSONObj toBSON() const {
        BSONObjBuilder builder;
        serialize(&builder);
        return builder.obj();
    }

    void reset() {
        std::lock_guard<std::mutex> lg(_mutex);
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

    mutable std::mutex _mutex;
};

}  // namespace test
}  // namespace mongo
