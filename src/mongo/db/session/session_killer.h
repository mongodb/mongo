// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/kill_sessions.h"
#include "mongo/db/session/kill_sessions_gen.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * The SessionKiller enforces a single thread for session killing for any given Service.
 *
 * The killer owns a background thread which actually does the work, and callers batch their kills
 * together each round, before the killer starts its dispatching, after which they batch up for the
 * next round.
 *
 * The KillFunc's kill function is passed in to its constructor, and parameterizes its behavior
 * depending on context (mongod/mongos).
 */
class SessionKiller {
public:
    /**
     * The Result of a call is either:
     *
     * Status::OK(), empty vector - we killed everything
     * Status::OK(), filled vector - we killed something.  HostAndPort is filled with nodes we
     *                               failed to kill on.
     *
     * !Status::OK() - The kill function itself failed.  I.e. we may have killed nothing.
     *
     * This contract has a helper in kill_sessions_common.h which adapts results for command
     * implementations. (killSessionsCmdHelper)
     */
    using Result = StatusWith<std::vector<HostAndPort>>;
    using UniformRandomBitGenerator = std::minstd_rand;

    /**
     * For regular sessions, the Matcher will directly match an lsid pattern. For internal sessions,
     * the Matcher will return matches on the child sessions of a parent session as well.
     */
    class Matcher {
    public:
        Matcher(KillAllSessionsByPatternSet&& patterns);

        const KillAllSessionsByPatternSet& getPatterns() const;

        const KillAllSessionsByPattern* match(const LogicalSessionId& lsid) const;

    private:
        KillAllSessionsByPatternSet _patterns;
        LogicalSessionIdMap<const KillAllSessionsByPattern*> _lsids;
        stdx::unordered_map<SHA256Block, const KillAllSessionsByPattern*> _uids;
        const KillAllSessionsByPattern* _killAll = nullptr;
    };

    /**
     * A process specific kill function (we have a different impl in mongos versus mongod).
     */
    using KillFunc =
        std::function<Result(OperationContext*, const Matcher&, UniformRandomBitGenerator* urbg)>;

    /**
     * The killer lives as a decoration on the service.
     */
    static SessionKiller* get(Service* service);
    static SessionKiller* get(OperationContext* opCtx);

    /**
     * This method binds the SessionKiller to the Service.
     */
    static void set(Service* service, std::shared_ptr<SessionKiller> sk);

    static void shutdown(Service* service);

    explicit SessionKiller(Service* service, KillFunc killer);
    ~SessionKiller();

    /**
     * This is the api for killSessions commands to invoke the killer.  It blocks until the kill is
     * finished, or until it fails (times out on all nodes in mongos).
     */
    [[MONGO_MOD_PRIVATE]] std::shared_ptr<Result> kill(OperationContext* opCtx,
                                                       const KillAllSessionsByPatternSet& toKill);

private:
    /**
     * This struct is a helper to default fill in the result, which is otherwise a little error
     * prone.
     */
    struct ReapResult {
        ReapResult();

        std::shared_ptr<boost::optional<Result>> result;
    };

    void _periodicKill(OperationContext* opCtx, std::unique_lock<std::mutex>& lk);

    KillFunc _killFunc;

    stdx::thread _thread;

    std::mutex _mutex;
    stdx::condition_variable _callerCV;
    stdx::condition_variable _killerCV;

    UniformRandomBitGenerator _urbg;

    ReapResult _reapResults;
    KillAllSessionsByPatternSet _nextToReap;

    bool _inShutdown = false;
};

}  // namespace mongo
