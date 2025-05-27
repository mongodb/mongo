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

#pragma once

#include "mongo/crypto/hash_block.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/session/kill_sessions_gen.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/stdx/unordered_set.h"

#include <cstddef>
#include <tuple>
#include <vector>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class OperationContext;
class ServiceContext;

struct KillAllSessionsByPatternItem {
    KillAllSessionsByPattern pattern;
    APIParameters apiParameters;
};

struct KillAllSessionsByPatternItemHash {
    std::size_t operator()(const KillAllSessionsByPatternItem& item) const {
        auto& pattern = item.pattern;
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
inline bool operator==(const KillAllSessionsByPatternItem& lhs,
                       const KillAllSessionsByPatternItem& rhs) {
    auto makeEqualityLens = [](const auto& item) {
        return std::tie(item.pattern.getLsid(), item.pattern.getUid(), item.apiParameters);
    };

    return makeEqualityLens(lhs) == makeEqualityLens(rhs);
}

inline bool operator!=(const KillAllSessionsByPatternItem& lhs,
                       const KillAllSessionsByPatternItem& rhs) {
    return !(lhs == rhs);
}

using KillAllSessionsByPatternSet =
    stdx::unordered_set<KillAllSessionsByPatternItem, KillAllSessionsByPatternItemHash>;

std::tuple<boost::optional<UserName>, std::vector<RoleName>>
getKillAllSessionsByPatternImpersonateData(const KillAllSessionsByPattern& pattern);

/**
 * Note: All three of the below makeKillAllSessionsByPattern helpers take opCtx to inline the
 * required impersonate data
 */

/**
 * Constructs a kill sessions pattern which kills all sessions
 */
KillAllSessionsByPatternItem makeKillAllSessionsByPattern(OperationContext* opCtx);

/**
 * Constructs a kill sessions pattern for a particular user
 */
KillAllSessionsByPatternItem makeKillAllSessionsByPattern(OperationContext* opCtx,
                                                          const KillAllSessionsUser& user);

/**
 * Constructs a KillAllSessionsByPatternSet, each element of which matches the UID of a user that is
 * currently authenticated on the given connection.
 */
KillAllSessionsByPatternSet makeSessionFilterForAuthenticatedUsers(OperationContext* opCtx);

/**
 * Constructs a kill sessions pattern for a particular logical session
 */
KillAllSessionsByPatternItem makeKillAllSessionsByPattern(OperationContext* opCtx,
                                                          const LogicalSessionId& lsid);

}  // namespace mongo
