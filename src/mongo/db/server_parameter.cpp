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

#include "mongo/db/server_parameter.h"

#include <fmt/format.h>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/bson/bsontypes.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {

using SPT = ServerParameterType;

MONGO_INITIALIZER_GROUP(BeginServerParameterRegistration, (), ("EndServerParameterRegistration"))
MONGO_INITIALIZER_GROUP(EndServerParameterRegistration,
                        ("BeginServerParameterRegistration"),
                        ("BeginStartupOptionHandling"))

ServerParameter::ServerParameter(StringData name, ServerParameterType spt)
    : _name{name}, _type(spt) {}

ServerParameter::ServerParameter(const ServerParameter& other) {
    // Since there is a mutex stored as a member variable, we need to explicitly write a copy
    // constructor because a mutex has it's default copy constructor deleted.
    stdx::lock_guard lk(other._mutex);
    this->_name = other._name;
    this->_type = other._type;
    this->_testOnly = other._testOnly;
    this->_redact = other._redact;
    this->_isOmittedInFTDC = other._isOmittedInFTDC;
    this->_featureFlag = other._featureFlag;
    this->_minFCV = other._minFCV;
    this->_disableState = other._disableState;
}

Status ServerParameter::set(const BSONElement& newValueElement,
                            const boost::optional<TenantId>& tenantId) {
    auto validateStatus = validate(newValueElement, tenantId);
    if (!validateStatus.isOK()) {
        return validateStatus;
    }

    auto swValue = _coerceToString(newValueElement);
    if (!swValue.isOK())
        return swValue.getStatus();
    return setFromString(swValue.getValue(), boost::none);
}

ServerParameterSet* ServerParameterSet::getNodeParameterSet() {
    static StaticImmortal obj = [] {
        ServerParameterSet sps;
        sps.setValidate([](const ServerParameter& sp) {
            uassert(6225102,
                    fmt::format(
                        "Registering cluster-wide parameter '{}' as node-local server parameter",
                        sp.name()),
                    sp.isNodeLocal());
        });
        return sps;
    }();
    return &*obj;
}

bool ServerParameter::isEnabled() const {
    const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    return isEnabledOnVersion(
        fcvSnapshot.isVersionInitialized()
            ? fcvSnapshot.getVersion()
            : multiversion::FeatureCompatibilityVersion::kUnsetDefaultLastLTSBehavior);
}

bool ServerParameter::isEnabledOnVersion(
    const multiversion::FeatureCompatibilityVersion& targetFCV) const {
    {
        stdx::lock_guard lk(_mutex);
        if (_disableState != DisableState::Enabled) {
            return false;
        }
    }
    return _isEnabledOnVersion(targetFCV);
}

bool ServerParameter::canBeEnabledOnVersion(
    const multiversion::FeatureCompatibilityVersion& targetFCV) const {
    {
        stdx::lock_guard lk(_mutex);
        if (_disableState == DisableState::PermanentlyDisabled) {
            return false;
        }
    }
    return _isEnabledOnVersion(targetFCV);
}

bool ServerParameter::_isEnabledOnVersion(
    const multiversion::FeatureCompatibilityVersion& targetFCV) const {
    return minFCVIsLessThanOrEqualToVersion(targetFCV) &&
        !featureFlagIsDisabledOnVersion(targetFCV);
}

bool ServerParameter::featureFlagIsDisabledOnVersion(
    const multiversion::FeatureCompatibilityVersion& targetFCV) const {
    stdx::lock_guard lk(_mutex);
    return !_featureFlag.isEnabled(
        [&](auto& fcvGatedFlag) { return fcvGatedFlag.isEnabledOnVersion(targetFCV); });
}

ServerParameterSet* ServerParameterSet::getClusterParameterSet() {
    static StaticImmortal obj = [] {
        ServerParameterSet sps;
        sps.setValidate([](const ServerParameter& sp) {
            uassert(6225103,
                    fmt::format(
                        "Registering node-local parameter '{}' as cluster-wide server parameter",
                        sp.name()),
                    sp.isClusterWide());
        });
        return sps;
    }();
    return &*obj;
}

void ServerParameterSet::add(std::unique_ptr<ServerParameter> sp) {
    if (_validate)
        _validate(*sp);
    auto [it, ok] = _map.try_emplace(sp->name(), std::move(sp));
    uassert(23784,
            fmt::format("Duplicate server parameter registration for '{}'",
                        sp->name()),  // NOLINT(bugprone-use-after-move)
            ok);
}

StatusWith<std::string> ServerParameter::_coerceToString(const BSONElement& element) {
    switch (element.type()) {
        case NumberDouble:
            return std::to_string(element.Double());
        case String:
            return element.String();
        case NumberInt:
            return std::to_string(element.Int());
        case NumberLong:
            return std::to_string(element.Long());
        case Date:
            return dateToISOStringLocal(element.Date());
        default:
            std::string diag;
            if (isRedact()) {
                diag = "###";
            } else {
                diag = element.toString();
            }
            return {ErrorCodes::BadValue,
                    str::stream() << "Unsupported type " << typeName(element.type()) << " (value: '"
                                  << diag << "') for setParameter: " << name()};
    }
}

void ServerParameterSet::remove(const std::string& name) {
    invariant(1 == _map.erase(name), fmt::format("Failed to erase key \"{}\"", name));
}

IDLServerParameterDeprecatedAlias::IDLServerParameterDeprecatedAlias(StringData name,
                                                                     ServerParameter* sp)
    : ServerParameter(name, sp->getServerParameterType()), _sp(sp) {
    if (_sp->isTestOnly()) {
        setTestOnly();
    }
}

void IDLServerParameterDeprecatedAlias::append(OperationContext* opCtx,
                                               BSONObjBuilder* b,
                                               StringData fieldName,
                                               const boost::optional<TenantId>& tenantId) {
    std::call_once(_warnOnce, [&] {
        LOGV2_WARNING(636300,
                      "Use of deprecated server parameter name",
                      "deprecatedName"_attr = name(),
                      "canonicalName"_attr = _sp->name());
    });
    _sp->append(opCtx, b, fieldName, tenantId);
}

Status IDLServerParameterDeprecatedAlias::reset(const boost::optional<TenantId>& tenantId) {
    std::call_once(_warnOnce, [&] {
        LOGV2_WARNING(636301,
                      "Use of deprecated server parameter name",
                      "deprecatedName"_attr = name(),
                      "canonicalName"_attr = _sp->name());
    });
    return _sp->reset(tenantId);
}

Status IDLServerParameterDeprecatedAlias::set(const BSONElement& newValueElement,
                                              const boost::optional<TenantId>& tenantId) {
    std::call_once(_warnOnce, [&] {
        LOGV2_WARNING(636302,
                      "Use of deprecated server parameter name",
                      "deprecatedName"_attr = name(),
                      "canonicalName"_attr = _sp->name());
    });
    return _sp->set(newValueElement, tenantId);
}

Status IDLServerParameterDeprecatedAlias::setFromString(StringData str,
                                                        const boost::optional<TenantId>& tenantId) {
    std::call_once(_warnOnce, [&] {
        LOGV2_WARNING(636303,
                      "Use of deprecated server parameter name",
                      "deprecatedName"_attr = name(),
                      "canonicalName"_attr = _sp->name());
    });
    return _sp->setFromString(str, tenantId);
}

void ServerParameterSet::disableTestParameters() {
    for (const auto& [name, sp] : _map) {
        if (sp->isTestOnly()) {
            sp->disable(true /* permanent */);
        }
    }
}

void registerServerParameter(std::unique_ptr<ServerParameter> p) {
    p->onRegistrationWithProcessGlobalParameterList();
    auto spt = p->getServerParameterType();
    ServerParameterSet::getParameterSet(spt)->add(std::move(p));
}

}  // namespace mongo
