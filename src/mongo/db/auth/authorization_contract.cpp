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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl

#include "mongo/db/auth/authorization_contract.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/auth/access_checks_gen.h"
#include "mongo/db/auth/action_type_gen.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/logv2/log.h"
#include "mongo/util/debug_util.h"

namespace mongo {

void AuthorizationContract::clear() {
    stdx::lock_guard<Mutex> lck(_mutex);

    _checks.reset();
    for (size_t i = 0; i < _privilegeChecks.size(); ++i) {
        _privilegeChecks[i].removeAllActions();
    }
}

void AuthorizationContract::addAccessCheck(AccessCheckEnum check) {
    stdx::lock_guard<Mutex> lck(_mutex);

    _checks.set(static_cast<size_t>(check), true);
}

bool AuthorizationContract::hasAccessCheck(AccessCheckEnum check) const {
    stdx::lock_guard<Mutex> lck(_mutex);

    return _checks.test(static_cast<size_t>(check));
}

void AuthorizationContract::addPrivilege(const Privilege& p) {
    stdx::lock_guard<Mutex> lck(_mutex);

    auto matchType = p.getResourcePattern().matchType();

    _privilegeChecks[static_cast<size_t>(matchType)].addAllActionsFromSet(p.getActions());
}

bool AuthorizationContract::hasPrivileges(const Privilege& p) const {
    stdx::lock_guard<Mutex> lck(_mutex);

    auto matchType = p.getResourcePattern().matchType();

    return _privilegeChecks[static_cast<size_t>(matchType)].contains(p.getActions());
}

bool AuthorizationContract::contains(const AuthorizationContract& other) const {
    stdx::lock_guard<Mutex> lck(_mutex);

    if ((_checks | other._checks) != _checks) {
        if (kDebugBuild) {
            auto missingChecks = (_checks ^ other._checks) & other._checks;

            BSONArrayBuilder builder;
            for (size_t i = 0; i < missingChecks.size(); i++) {
                if (missingChecks.test(i)) {
                    builder.append(AccessCheck_serializer(static_cast<AccessCheckEnum>(i)));
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
                        builder.append(ActionType_serializer(at));
                    }
                }

                LOGV2(5452403,
                      "Missing Action Types for resource",
                      "resource"_attr = MatchType_serializer(static_cast<MatchTypeEnum>(i)),
                      "actions"_attr = builder.arr());
            }

            return false;
        }
    }

    return true;
}

}  // namespace mongo
