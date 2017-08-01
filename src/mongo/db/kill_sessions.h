/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include <tuple>

#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/kill_sessions_gen.h"
#include "mongo/db/logical_session_id.h"

namespace mongo {

class OperationContext;
class ServiceContext;

struct KillAllSessionsByPatternHash {
    std::size_t operator()(const KillAllSessionsByPattern& pattern) const {
        if (pattern.getLsid()) {
            return lsidHasher(*pattern.getLsid());
        } else if (pattern.getUid()) {
            return uidHasher(*pattern.getUid());
        }

        // fall through for killAll
        return 0;
    }

    LogicalSessionIdHash lsidHasher;
    SHA256Block::Hash uidHasher;
};

/**
 * Patterns are specifically equal if they differ only by impersonate data.
 */
inline bool operator==(const KillAllSessionsByPattern& lhs, const KillAllSessionsByPattern& rhs) {
    auto makeEqualityLens = [](const auto& pattern) {
        return std::tie(pattern.getLsid(), pattern.getUid());
    };

    return makeEqualityLens(lhs) == makeEqualityLens(rhs);
}

inline bool operator!=(const KillAllSessionsByPattern& lhs, const KillAllSessionsByPattern& rhs) {
    return !(lhs == rhs);
}

using KillAllSessionsByPatternSet =
    stdx::unordered_set<KillAllSessionsByPattern, KillAllSessionsByPatternHash>;

std::tuple<std::vector<UserName>, std::vector<RoleName>> getKillAllSessionsByPatternImpersonateData(
    const KillAllSessionsByPattern& pattern);

/**
 * Note: All three of the below makeKillAllSessionsByPattern helpers take opCtx to inline the
 * required impersonate data
 */

/**
 * Constructs a kill sessions pattern which kills all sessions
 */
KillAllSessionsByPattern makeKillAllSessionsByPattern(OperationContext* opCtx);

/**
 * Constructs a kill sessions pattern for a particular user
 */
KillAllSessionsByPattern makeKillAllSessionsByPattern(OperationContext* opCtx,
                                                      const KillAllSessionsUser& user);

/**
 * Constructs a kill sessions pattern for a particular logical session
 */
KillAllSessionsByPattern makeKillAllSessionsByPattern(OperationContext* opCtx,
                                                      const LogicalSessionId& lsid);

}  // namespace mongo
