/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
