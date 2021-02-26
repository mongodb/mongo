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

#include "mongo/db/auth/authorization_contract.h"
#include "mongo/db/auth/privilege.h"

namespace mongo {

void AuthorizationContract::addAccessCheck(AccessCheckEnum check) {
    _checks.set(static_cast<size_t>(check), true);
}

bool AuthorizationContract::hasAccessCheck(AccessCheckEnum check) const {
    return _checks.test(static_cast<size_t>(check));
}

void AuthorizationContract::addPrivilege(const Privilege& p) {
    auto matchType = p.getResourcePattern().matchType();

    _privilegeChecks[static_cast<size_t>(matchType)].addAllActionsFromSet(p.getActions());
}

bool AuthorizationContract::hasPrivileges(const Privilege& p) const {
    auto matchType = p.getResourcePattern().matchType();

    return _privilegeChecks[static_cast<size_t>(matchType)].contains(p.getActions());
}

bool AuthorizationContract::contains(const AuthorizationContract& other) const {

    if ((_checks | other._checks) != _checks) {
        return false;
    }

    for (size_t i = 0; i < _privilegeChecks.size(); ++i) {
        if (!_privilegeChecks[i].contains(other._privilegeChecks[i])) {
            return false;
        }
    }

    return true;
}

}  // namespace mongo
