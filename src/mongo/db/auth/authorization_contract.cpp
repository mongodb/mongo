// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/auth/authorization_contract.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/access_checks_gen.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/action_type_gen.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/logv2/log.h"
#include "mongo/util/debug_util.h"

#include <cstddef>
#include <mutex>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl


namespace mongo {

void AuthorizationContract::enterCommandScope() {
    std::lock_guard<std::mutex> lck(_mutex);

    // Only clear checks when its a top level command.
    if (_commandDepth == 0) {
        clear(lck);
    }
    _commandDepth++;
}

void AuthorizationContract::exitCommandScope() {
    std::lock_guard<std::mutex> lck(_mutex);
    invariant(_commandDepth > 0);
    _commandDepth--;
}

void AuthorizationContract::clear() {
    std::lock_guard<std::mutex> lck(_mutex);
    clear(lck);
}

void AuthorizationContract::clear(WithLock lk) {
    _checks.reset();
    for (size_t i = 0; i < _privilegeChecks.size(); ++i) {
        _privilegeChecks[i].removeAllActions();
    }

    _isPermissionChecked = false;
}

void AuthorizationContract::addAccessCheck(AccessCheckEnum check) {
    if (!_isTestModeEnabled) {
        return;
    }

    std::lock_guard<std::mutex> lck(_mutex);
    if (_commandDepth > 1) {
        return;
    }

    _checks.set(static_cast<size_t>(check), true);

    if (!isCommonAccessCheck(check)) {
        _isPermissionChecked = true;
    }
}

bool AuthorizationContract::hasAccessCheck(AccessCheckEnum check) const {
    std::lock_guard<std::mutex> lck(_mutex);

    return _checks.test(static_cast<size_t>(check));
}

void AuthorizationContract::addPrivilege(const Privilege& p) {
    if (!_isTestModeEnabled) {
        return;
    }

    std::lock_guard<std::mutex> lck(_mutex);
    if (_commandDepth > 1) {
        return;
    }

    auto matchType = p.getResourcePattern().matchType();

    _privilegeChecks[static_cast<size_t>(matchType)].addAllActionsFromSet(p.getActions());

    if (!isCommonPrivilege(p)) {
        _isPermissionChecked = true;
    }
}

bool AuthorizationContract::hasPrivileges(const Privilege& p) const {
    std::lock_guard<std::mutex> lck(_mutex);

    auto matchType = p.getResourcePattern().matchType();

    return _privilegeChecks[static_cast<size_t>(matchType)].contains(p.getActions());
}

bool AuthorizationContract::contains(const AuthorizationContract& other) const {

    if (this == &other) {
        return true;  // this and other are same - so contains is necessarily true
    }

    std::scoped_lock<std::mutex, std::mutex> lk(_mutex, other._mutex);

    if ((_checks | other._checks) != _checks) {
        if (kDebugBuild) {
            auto missingChecks = (_checks ^ other._checks) & other._checks;

            BSONArrayBuilder builder;
            for (size_t i = 0; i < missingChecks.size(); i++) {
                if (missingChecks.test(i)) {
                    builder.append(idl::serialize(static_cast<AccessCheckEnum>(i)));
                }
            }

            LOGV2(5452402, "Missing Auth Checks", "checks"_attr = builder.arr());
        }

        return false;
    }

    for (size_t i = 0; i < _privilegeChecks.size(); ++i) {
        if (!_privilegeChecks[i].contains(other._privilegeChecks[i])) {
            if (kDebugBuild) {
                BSONArrayBuilder builder;

                for (size_t k = 0; k < kNumActionTypes; k++) {
                    auto at = static_cast<ActionTypeEnum>(k);
                    if (other._privilegeChecks[i].contains(at) &&
                        !_privilegeChecks[i].contains(at)) {
                        builder.append(idl::serialize(at));
                    }
                }

                LOGV2(5452403,
                      "Missing Action Types for resource",
                      "resource"_attr = idl::serialize(static_cast<MatchTypeEnum>(i)),
                      "actions"_attr = builder.arr());
            }

            return false;
        }
    }

    return true;
}

bool AuthorizationContract::isPermissionChecked() const {
    std::lock_guard<std::mutex> lck(_mutex);

    return _isPermissionChecked;
}

bool AuthorizationContract::isCommonAccessCheck(AccessCheckEnum check) const {
    const auto& commonChecks = getCommonAccessChecks();
    return std::find(commonChecks.begin(), commonChecks.end(), check) != commonChecks.end();
}

bool AuthorizationContract::isCommonPrivilege(const Privilege& p) const {
    const auto& commonPrivileges = getCommonPrivileges();
    return std::find(commonPrivileges.begin(), commonPrivileges.end(), p) != commonPrivileges.end();
}

}  // namespace mongo
