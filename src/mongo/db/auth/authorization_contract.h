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

#pragma once

#include "mongo/db/auth/access_checks_gen.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/action_type_gen.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/with_lock.h"

#include <array>
#include <bitset>
#include <initializer_list>

namespace mongo {

/**
 * Contains an authorization contract which is the set of authorization checks a command is
 * permitted to make against AuthorizationSession.
 *
 * There are two types of authorization checks:
 * 1. Checks for privileges
 * 2. Other forms of checks, simply called access check. These are generally boolean functions on
 * AuthorizationSession that check criteria like isAuthorized().
 *
 * This class is a lossy set.
 * 1. It does not record a count of times a check has been performed.
 * 2. It does not record which namespace a check is performed against.
 *
 * When commands execute other commands via DBDirectClient, we only want
 * the top-level command to accumulate authorization checks.
 */
class AuthorizationContract {
public:
    AuthorizationContract() = default;
    AuthorizationContract(bool isTestModeEnabled) : _isTestModeEnabled(isTestModeEnabled) {};

    template <typename Checks, typename Privileges>
    AuthorizationContract(const Checks& checks, const Privileges& privileges) {
        for (const auto check : checks) {
            addAccessCheck(check);
        }
        for (const auto& p : privileges) {
            addPrivilege(p);
        }
    }

    AuthorizationContract(const AuthorizationContract& other) {
        stdx::lock_guard<stdx::mutex> lck(other._mutex);
        _checks = other._checks;
        _privilegeChecks = other._privilegeChecks;
        _isPermissionChecked.storeRelaxed(other._isPermissionChecked.loadRelaxed());
        _commandDepth = other._commandDepth;
    }

    /**
     * Start tracking permissions and privileges in the authorization contract.
     * Will only track top level commands to prevent nested direct client operations from polluting
     * the parent command's authorization contract.
     */
    void enterCommandScope();

    /**
     * Stops tracking the contract and reduces the command depth counter.
     */
    void exitCommandScope();

    /**
     * Clear the authorization contract
     */
    void clear();

    /**
     * Add an access check to the contract.
     */
    void addAccessCheck(AccessCheckEnum check);

    /**
     * Add a privilege and all actions contained in the privilege to the contract.
     */
    void addPrivilege(const Privilege& p);

    /**
     * Check if the contract contains a given access check.
     */
    bool hasAccessCheck(AccessCheckEnum check) const;

    /**
     * Check if the contract contains a privilege include all actions in the privilege.
     */
    bool hasPrivileges(const Privilege& p) const;

    /**
     * Check if this contract contains all the checks of the other contract.
     */
    bool contains(const AuthorizationContract& other) const;

    /**
     * Return true if PermissionCheckStatus is Checked
     */
    bool isPermissionChecked() const {
        return _isPermissionChecked.loadRelaxed();
    }

private:
    // Clear the authorization contract.
    void clear(WithLock lk);

    mutable stdx::mutex _mutex;

    // Set of access checks performed
    std::bitset<idlEnumCount<AccessCheckEnum>> _checks;

    // Set of privileges performed per resource pattern type
    std::array<ActionSet, idlEnumCount<MatchTypeEnum>> _privilegeChecks;

    // Current status of permission check, updated on added access check, added privilege, or clear
    Atomic<bool> _isPermissionChecked{false};

    // If false accounting and mutex guards are disabled
    bool _isTestModeEnabled{true};

    // Depth of commands running as DBDirectClient.
    int _commandDepth{0};
};

}  // namespace mongo
