// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
#include "mongo/util/modules.h"

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
[[MONGO_MOD_PUBLIC]] inline bool operator==(const KillAllSessionsByPatternItem& lhs,
                                            const KillAllSessionsByPatternItem& rhs) {
    auto makeEqualityLens = [](const auto& item) {
        return std::tie(item.pattern.getLsid(), item.pattern.getUid(), item.apiParameters);
    };

    return makeEqualityLens(lhs) == makeEqualityLens(rhs);
}

[[MONGO_MOD_PUBLIC]] inline bool operator!=(const KillAllSessionsByPatternItem& lhs,
                                            const KillAllSessionsByPatternItem& rhs) {
    return !(lhs == rhs);
}

using KillAllSessionsByPatternSet [[MONGO_MOD_PUBLIC]] =
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
[[MONGO_MOD_PUBLIC]] KillAllSessionsByPatternItem makeKillAllSessionsByPattern(
    OperationContext* opCtx);

/**
 * Constructs a kill sessions pattern for a particular user
 */
[[MONGO_MOD_PUBLIC]] KillAllSessionsByPatternItem makeKillAllSessionsByPattern(
    OperationContext* opCtx, const KillAllSessionsUser& user);

/**
 * Constructs a KillAllSessionsByPatternSet, each element of which matches the UID of a user that is
 * currently authenticated on the given connection.
 */
[[MONGO_MOD_PUBLIC]] KillAllSessionsByPatternSet makeSessionFilterForAuthenticatedUsers(
    OperationContext* opCtx);

/**
 * Constructs a kill sessions pattern for a particular logical session
 */
[[MONGO_MOD_PUBLIC]] KillAllSessionsByPatternItem makeKillAllSessionsByPattern(
    OperationContext* opCtx, const LogicalSessionId& lsid);

}  // namespace mongo
