/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/client/sdam/server_description.h"
#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include <algorithm>
#include <boost/algorithm/string.hpp>
#include <boost/optional.hpp>
#include <set>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/client/sdam/sdam_datatypes.h"
#include "mongo/util/duration.h"
#include "mongo/util/log.h"


namespace mongo::sdam {

namespace {

std::set<ServerType> kDataServerTypes{
    ServerType::kMongos, ServerType::kRSPrimary, ServerType::kRSSecondary, ServerType::kStandalone};

}  // namespace

ServerDescription::ServerDescription(ClockSource* clockSource,
                                     const IsMasterOutcome& isMasterOutcome,
                                     boost::optional<IsMasterRTT> lastRtt)
    : ServerDescription(isMasterOutcome.getServer()) {
    if (isMasterOutcome.isSuccess()) {
        const auto response = *isMasterOutcome.getResponse();

        // type must be parsed before RTT is calculated.
        parseTypeFromIsMaster(response);
        calculateRtt(*isMasterOutcome.getRtt(), lastRtt);

        _lastUpdateTime = clockSource->now();
        _minWireVersion = response["minWireVersion"].numberInt();
        _maxWireVersion = response["maxWireVersion"].numberInt();

        saveLastWriteInfo(response.getObjectField("lastWrite"));
        saveHosts(response);
        saveTags(response.getObjectField("tags"));
        saveElectionId(response.getField("electionId"));

        auto lsTimeoutField = response.getField("logicalSessionTimeoutMinutes");
        if (lsTimeoutField.type() == BSONType::NumberInt) {
            _logicalSessionTimeoutMinutes = lsTimeoutField.numberInt();
        }

        auto setVersionField = response.getField("setVersion");
        if (setVersionField.type() == BSONType::NumberInt) {
            _setVersion = response["setVersion"].numberInt();
        }

        auto setNameField = response.getField("setName");
        if (setNameField.type() == BSONType::String) {
            _setName = response["setName"].str();
        }

        auto primaryField = response.getField("primary");
        if (primaryField.type() == BSONType::String) {
            _primary = response.getStringField("primary");
        }
    } else {
        _error = isMasterOutcome.getErrorMsg();
    }
}

void ServerDescription::storeHostListIfPresent(const std::string key,
                                               const BSONObj response,
                                               std::set<ServerAddress>& destination) {
    if (response.hasField(key)) {
        auto hostsBsonArray = response[key].Array();
        std::transform(hostsBsonArray.begin(),
                       hostsBsonArray.end(),
                       std::inserter(destination, destination.begin()),
                       [](const BSONElement e) { return boost::to_lower_copy(e.String()); });
    }
}

void ServerDescription::saveHosts(const BSONObj response) {
    if (response.hasField("me")) {
        auto me = response.getField("me");
        _me = boost::to_lower_copy(me.str());
    }

    storeHostListIfPresent("hosts", response, _hosts);
    storeHostListIfPresent("passives", response, _passives);
    storeHostListIfPresent("arbiters", response, _arbiters);
}

void ServerDescription::saveTags(BSONObj tagsObj) {
    const auto keys = tagsObj.getFieldNames<std::set<std::string>>();
    for (const auto key : keys) {
        _tags[key] = tagsObj.getStringField(key);
    }
}

void ServerDescription::saveElectionId(BSONElement electionId) {
    if (electionId.type() == jstOID) {
        _electionId = electionId.OID();
    }
}

void ServerDescription::calculateRtt(const IsMasterRTT currentRtt,
                                     const boost::optional<IsMasterRTT> lastRtt) {
    if (getType() == ServerType::kUnknown) {
        // if a server's type is Unknown, it's RTT is null
        // see:
        // https://github.com/mongodb/specifications/blob/master/source/server-discovery-and-monitoring/server-discovery-and-monitoring.rst#roundtriptime
        return;
    }

    if (lastRtt) {
        // new_rtt = alpha * x + (1 - alpha) * old_rtt
        _rtt = IsMasterRTT(static_cast<IsMasterRTT::rep>(kRttAlpha * currentRtt.count() +
                                                         (1 - kRttAlpha) * lastRtt.get().count()));
    } else {
        _rtt = currentRtt;
    }
}

void ServerDescription::saveLastWriteInfo(BSONObj lastWriteBson) {
    const auto lastWriteDateField = lastWriteBson.getField("lastWriteDate");
    if (lastWriteDateField.type() == BSONType::Date) {
        _lastWriteDate = lastWriteDateField.date();
    }

    const auto opTimeParse =
        repl::OpTime::parseFromOplogEntry(lastWriteBson.getObjectField("opTime"));
    if (opTimeParse.isOK()) {
        _opTime = opTimeParse.getValue();
    }
}

void ServerDescription::parseTypeFromIsMaster(const BSONObj isMaster) {
    ServerType t;
    bool hasSetName = isMaster.hasField("setName");

    if (isMaster.getField("ok").numberInt() != 1) {
        t = ServerType::kUnknown;
    } else if (!hasSetName && !isMaster.hasField("msg") && !isMaster.getBoolField("isreplicaset")) {
        t = ServerType::kStandalone;
    } else if (kIsDbGrid == isMaster.getStringField("msg")) {
        t = ServerType::kMongos;
    } else if (hasSetName && isMaster.getBoolField("hidden")) {
        t = ServerType::kRSOther;
    } else if (hasSetName && isMaster.getBoolField("ismaster")) {
        t = ServerType::kRSPrimary;
    } else if (hasSetName && isMaster.getBoolField("secondary")) {
        t = ServerType::kRSSecondary;
    } else if (hasSetName && isMaster.getBoolField("arbiterOnly")) {
        t = ServerType::kRSArbiter;
    } else if (hasSetName) {
        t = ServerType::kRSOther;
    } else if (isMaster.getBoolField("isreplicaset")) {
        t = ServerType::kRSGhost;
    } else {
        error() << "unknown server type from successful ismaster reply: " << isMaster.toString();
        t = ServerType::kUnknown;
    }
    _type = t;
}

const ServerAddress& ServerDescription::getAddress() const {
    return _address;
}

const boost::optional<std::string>& ServerDescription::getError() const {
    return _error;
}

const boost::optional<IsMasterRTT>& ServerDescription::getRtt() const {
    return _rtt;
}

const boost::optional<mongo::Date_t>& ServerDescription::getLastWriteDate() const {
    return _lastWriteDate;
}

const boost::optional<repl::OpTime>& ServerDescription::getOpTime() const {
    return _opTime;
}

ServerType ServerDescription::getType() const {
    return _type;
}

const boost::optional<ServerAddress>& ServerDescription::getMe() const {
    return _me;
}

const std::set<ServerAddress>& ServerDescription::getHosts() const {
    return _hosts;
}

const std::set<ServerAddress>& ServerDescription::getPassives() const {
    return _passives;
}

const std::set<ServerAddress>& ServerDescription::getArbiters() const {
    return _arbiters;
}

const std::map<std::string, std::string>& ServerDescription::getTags() const {
    return _tags;
}

const boost::optional<std::string>& ServerDescription::getSetName() const {
    return _setName;
}

const boost::optional<int>& ServerDescription::getSetVersion() const {
    return _setVersion;
}

const boost::optional<mongo::OID>& ServerDescription::getElectionId() const {
    return _electionId;
}

const boost::optional<ServerAddress>& ServerDescription::getPrimary() const {
    return _primary;
}

const mongo::Date_t ServerDescription::getLastUpdateTime() const {
    return *_lastUpdateTime;
}

const boost::optional<int>& ServerDescription::getLogicalSessionTimeoutMinutes() const {
    return _logicalSessionTimeoutMinutes;
}

bool ServerDescription::isEquivalent(const ServerDescription& other) const {
    auto otherValues = std::tie(other._type,
                                other._minWireVersion,
                                other._maxWireVersion,
                                other._me,
                                other._hosts,
                                other._passives,
                                other._arbiters,
                                other._tags,
                                other._setName,
                                other._setVersion,
                                other._electionId,
                                other._primary,
                                other._logicalSessionTimeoutMinutes);
    auto thisValues = std::tie(_type,
                               _minWireVersion,
                               _maxWireVersion,
                               _me,
                               _hosts,
                               _passives,
                               _arbiters,
                               _tags,
                               _setName,
                               _setVersion,
                               _electionId,
                               _primary,
                               _logicalSessionTimeoutMinutes);
    return thisValues == otherValues;
}

bool ServerDescription::isDataBearingServer() const {
    return kDataServerTypes.find(_type) != kDataServerTypes.end();
}

// output server description to bson. This is primarily used for debugging.
BSONObj ServerDescription::toBson() const {
    BSONObjBuilder bson;
    bson.append("address", _address);
    if (_rtt) {
        bson.append("roundTripTime", durationCount<Microseconds>(*_rtt));
    }

    if (_lastWriteDate) {
        bson.appendDate("lastWriteDate", *_lastWriteDate);
    }

    if (_opTime) {
        bson.append("opTime", _opTime->toBSON());
    }

    {
        using mongo::sdam::toString;
        bson.append("type", toString(_type));
    }

    bson.append("minWireVersion", _minWireVersion);
    bson.append("maxWireVersion", _maxWireVersion);
    if (_me) {
        bson.append("me", *_me);
    }
    if (_setName) {
        bson.append("setName", *_setName);
    }
    if (_setVersion) {
        bson.append("setVersion", *_setVersion);
    }
    if (_electionId) {
        bson.append("electionId", *_electionId);
    }
    if (_primary) {
        bson.append("primary", *_primary);
    }
    if (_lastUpdateTime) {
        bson.append("lastUpdateTime", *_lastUpdateTime);
    }
    if (_logicalSessionTimeoutMinutes) {
        bson.append("logicalSessionTimeoutMinutes", *_logicalSessionTimeoutMinutes);
    }

    bson.append("hosts", _hosts);
    bson.append("arbiters", _arbiters);
    bson.append("passives", _passives);

    return bson.obj();
}

int ServerDescription::getMinWireVersion() const {
    return _minWireVersion;
}

int ServerDescription::getMaxWireVersion() const {
    return _maxWireVersion;
}

std::string ServerDescription::toString() const {
    return toBson().toString();
}


bool operator==(const mongo::sdam::ServerDescription& a, const mongo::sdam::ServerDescription& b) {
    return a.isEquivalent(b);
}

bool operator!=(const mongo::sdam::ServerDescription& a, const mongo::sdam::ServerDescription& b) {
    return !(a == b);
}

std::ostream& operator<<(std::ostream& os, const ServerDescription& description) {
    BSONObj obj = description.toBson();
    os << obj.toString();
    return os;
}

std::ostream& operator<<(std::ostream& os, const ServerDescriptionPtr& description) {
    os << *description;
    return os;
}
};  // namespace mongo::sdam
