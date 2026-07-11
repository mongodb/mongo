// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/process_health/fault_manager_config.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kProcessHealth


namespace mongo {
namespace process_health {
using namespace std::literals::string_view_literals;

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
            return s << "Ok"sv;
        case FaultState::kStartupCheck:
            return s << "StartupCheck"sv;
        case FaultState::kTransientFault:
            return s << "TransientFault"sv;
        case FaultState::kActiveFault:
            return s << "ActiveFault"sv;
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
