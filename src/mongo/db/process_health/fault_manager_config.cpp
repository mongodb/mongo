/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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


#include "mongo/db/process_health/fault_manager_config.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kProcessHealth


namespace mongo {
namespace process_health {

namespace {
constexpr auto inline kDefaultObserverInterval = Milliseconds{10000};
constexpr auto inline kDefaultLdapObserverInterval = Milliseconds{30000};
constexpr auto inline kDefaultConfigServerObserverInterval = Milliseconds{30000};
constexpr auto inline kDefaultDNSObserverInterval = Milliseconds{30000};
constexpr auto inline kDefaultTestObserverInterval = Milliseconds{1000};
}  // namespace

Milliseconds FaultManagerConfig::_getDefaultObserverInterval(FaultFacetType type) {
    switch (type) {
        case FaultFacetType::kLdap:
            return kDefaultLdapObserverInterval;
        case FaultFacetType::kConfigServer:
            return kDefaultConfigServerObserverInterval;
        case FaultFacetType::kDns:
            return kDefaultDNSObserverInterval;
        case FaultFacetType::kMock1:
        case FaultFacetType::kMock2:
        case FaultFacetType::kTestObserver:
            return kDefaultTestObserverInterval;
        default:
            return kDefaultObserverInterval;
    }
}

StringBuilder& operator<<(StringBuilder& s, const FaultState& state) {
    switch (state) {
        case FaultState::kOk:
            return s << "Ok"_sd;
        case FaultState::kStartupCheck:
            return s << "StartupCheck"_sd;
        case FaultState::kTransientFault:
            return s << "TransientFault"_sd;
        case FaultState::kActiveFault:
            return s << "ActiveFault"_sd;
        default:
            MONGO_UNREACHABLE;
    }
}

std::ostream& operator<<(std::ostream& os, const FaultState& state) {
    StringBuilder sb;
    sb << state;
    return os << sb.stringData();
}

// TODO(SERVER-62125): remove this conversion and use idl type everywhere
FaultFacetType toFaultFacetType(HealthObserverTypeEnum type) {
    switch (type) {
        case HealthObserverTypeEnum::kLdap:
            return FaultFacetType::kLdap;
        case HealthObserverTypeEnum::kDns:
            return FaultFacetType::kDns;
        case HealthObserverTypeEnum::kTest:
            return FaultFacetType::kTestObserver;
        case HealthObserverTypeEnum::kConfigServer:
            return FaultFacetType::kConfigServer;
        default:
            MONGO_UNREACHABLE;
    }
}

}  // namespace process_health
}  // namespace mongo
