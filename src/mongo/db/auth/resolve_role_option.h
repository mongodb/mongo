// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * Options for what data resolveRoles() should mine from the role tree.
 *
 * kRoles():    Collect RoleNames in the "roles" field in each role document for
 * subordinates.
 *
 * kPrivileges():   Examine the "privileges" field in each role document and merge
 * "actions" for identical "resource" patterns.
 *
 * kRestrictions(): Collect the "authenticationRestrictions" field in each role document.
 *
 * kAllInfo():  A convenient combination of the above three options
 *
 * setDirectOnly(bool shouldEnable):    If shouldEnable is true, only the RoleNames explicitly
 * supplied to resolveRoles() will be examined. If shouldEnable is false or this method is never
 * called, then resolveRoles() will continue examining all subordinate roles until the tree has
 * been exhausted.
 *
 * setIgnoreUnknown(bool shouldEnable): If shouldEnable is true, warning logs will not be
 * emitted for roles that do not exist. If shouldEnable is false or this method is never called,
 * then warning logs will be emitted for any roles that do not exist.
 *
 */

#pragma once

#include "mongo/util/modules.h"

#include <cstdint>

namespace mongo::auth {
class ResolveRoleOption {
private:
    enum ResolveRoleEnum : std::uint8_t {

        kRolesFlag = 0x01,
        kPrivilegesFlag = 0x02,
        kRestrictionsFlag = 0x04,
        kAllInfoMask = kRolesFlag | kPrivilegesFlag | kRestrictionsFlag,

        // Only collect from the first pass.
        kDirectOnlyFlag = 0x10,

        // Omit warning logs for unknown direct roles during resolution.
        kIgnoreUnknownFlag = 0x20,
    };

    constexpr ResolveRoleOption(ResolveRoleEnum value) : _value(value) {}

    ResolveRoleOption& _setFlag(ResolveRoleEnum flag, bool shouldEnable) {
        if (shouldEnable) {
            _value = static_cast<ResolveRoleEnum>(_value | flag);
        } else {
            _value = static_cast<ResolveRoleEnum>(_value & ~flag);
        }

        return *this;
    }

    bool _contains(ResolveRoleEnum flag) const {
        return _value & flag;
    }

    ResolveRoleEnum _value;

public:
    ResolveRoleOption() = delete;

    /**
     * Constants -> these should be used to retrieve the different types of masks for role
     * resolution.
     */
    static constexpr auto kRoles() {
        return ResolveRoleOption(ResolveRoleOption::kRolesFlag);
    }

    static constexpr auto kPrivileges() {
        return ResolveRoleOption(ResolveRoleOption::kPrivilegesFlag);
    }

    static constexpr auto kRestrictions() {
        return ResolveRoleOption(ResolveRoleOption::kRestrictionsFlag);
    }

    static constexpr auto kAllInfo() {
        return ResolveRoleOption(ResolveRoleOption::kAllInfoMask);
    }

    /**
     * Mutators -> these can be used to toggle individual flags on and off.
     */
    ResolveRoleOption& setRoles(bool shouldEnable) {
        return _setFlag(ResolveRoleEnum::kRolesFlag, shouldEnable);
    }

    ResolveRoleOption& setPrivileges(bool shouldEnable) {
        return _setFlag(ResolveRoleEnum::kPrivilegesFlag, shouldEnable);
    }

    ResolveRoleOption& setRestrictions(bool shouldEnable) {
        return _setFlag(ResolveRoleEnum::kRestrictionsFlag, shouldEnable);
    }

    ResolveRoleOption& setDirectOnly(bool shouldEnable) {
        return _setFlag(ResolveRoleEnum::kDirectOnlyFlag, shouldEnable);
    }

    ResolveRoleOption& setIgnoreUnknown(bool shouldEnable) {
        return _setFlag(ResolveRoleEnum::kIgnoreUnknownFlag, shouldEnable);
    }

    /**
     * Comparison methods -> these can be used to check whether a specific option is enabled.
     */
    bool shouldMineRoles() const {
        return _contains(ResolveRoleEnum::kRolesFlag);
    }

    bool shouldMinePrivileges() const {
        return _contains(ResolveRoleEnum::kPrivilegesFlag);
    }

    bool shouldMineRestrictions() const {
        return _contains(ResolveRoleEnum::kRestrictionsFlag);
    }

    bool shouldMineDirectOnly() const {
        return _contains(ResolveRoleEnum::kDirectOnlyFlag);
    }

    bool shouldIgnoreUnknown() const {
        return _contains(ResolveRoleEnum::kIgnoreUnknownFlag);
    }
};
}  // namespace mongo::auth
