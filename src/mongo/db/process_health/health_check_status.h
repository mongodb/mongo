// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/process_health/fault_manager_config.h"
#include "mongo/util/modules.h"

#include <memory>
#include <ostream>
#include <string_view>

namespace mongo {
namespace process_health {
using namespace std::literals::string_view_literals;

enum class [[MONGO_MOD_PUBLIC]] Severity { kOk, kFailure };
static const std::string_view SeverityStrings[] = {"kOk", "kFailure"};

inline StringBuilder& operator<<(StringBuilder& s, const Severity& sev) {
    return s << SeverityStrings[static_cast<int>(sev)];
}

inline std::ostream& operator<<(std::ostream& s, const Severity& sev) {
    return s << SeverityStrings[static_cast<int>(sev)];
}
/**
 * Immutable class representing current status of an ongoing fault tracked by facet.
 */
class [[MONGO_MOD_PUBLIC]] HealthCheckStatus {
public:
    HealthCheckStatus(FaultFacetType type, Severity severity, std::string_view description)
        : _type(type), _severity(severity), _description(description) {}

    // Constructs a resolved status (no fault detected).
    explicit HealthCheckStatus(FaultFacetType type)
        : _type(type), _severity(Severity::kOk), _description("resolved"sv) {}

    explicit HealthCheckStatus(HealthObserverTypeEnum type)
        : _type(toFaultFacetType(type)), _severity(Severity::kOk), _description("resolved"sv) {}

    HealthCheckStatus(const HealthCheckStatus&) = default;
    HealthCheckStatus& operator=(const HealthCheckStatus&) = default;
    HealthCheckStatus(HealthCheckStatus&&) = default;
    HealthCheckStatus& operator=(HealthCheckStatus&&) = default;

    /**
     * @return FaultFacetType of this status.
     */
    FaultFacetType getType() const {
        return _type;
    }

    /**
     * The fault severity value if any.
     *
     * @return Current fault severity
     */
    Severity getSeverity() const {
        return _severity;
    }

    std::string_view getShortDescription() const {
        return _description;
    }

    void appendDescription(BSONObjBuilder* builder) const;

    BSONObj toBSON() const;

    std::string toString() const;

    // Helpers for severity levels.

    static bool isResolved(Severity severity) {
        return severity == Severity::kOk;
    }

    static bool isActiveFault(Severity severity) {
        return severity == Severity::kFailure;
    }

    bool isActiveFault() const {
        return isActiveFault(getSeverity());
    }

private:
    friend std::ostream& operator<<(std::ostream&, const HealthCheckStatus&);
    friend StringBuilder& operator<<(StringBuilder& s, const HealthCheckStatus& hcs);


    FaultFacetType _type;
    Severity _severity;
    std::string _description;
};

inline void HealthCheckStatus::appendDescription(BSONObjBuilder* builder) const {
    builder->append("type", _type);
    builder->append("description", _description);
    builder->append("severity", _severity);
}

inline StringBuilder& operator<<(StringBuilder& s, const HealthCheckStatus& hcs) {
    BSONObjBuilder bob;
    hcs.appendDescription(&bob);
    return s << bob.obj();
}

inline std::ostream& operator<<(std::ostream& os, const HealthCheckStatus& hcs) {
    BSONObjBuilder bob;
    hcs.appendDescription(&bob);
    return os << bob.obj();
}

inline BSONObj HealthCheckStatus::toBSON() const {
    BSONObjBuilder bob;
    appendDescription(&bob);
    return bob.obj();
}

inline std::string HealthCheckStatus::toString() const {
    BSONObjBuilder bob;
    appendDescription(&bob);
    return bob.obj().toString();
}

}  // namespace process_health
}  // namespace mongo
