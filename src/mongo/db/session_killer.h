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

#include <boost/optional.hpp>
#include <functional>
#include <memory>
#include <random>
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/db/kill_sessions.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/hierarchical_acquisition.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

/**
 * The SessionKiller enforces a single thread for session killing for any given ServiceContext.
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

    class Matcher {
    public:
        Matcher(KillAllSessionsByPatternSet&& patterns);

        const KillAllSessionsByPatternSet& getPatterns() const;

        const KillAllSessionsByPattern* match(const LogicalSessionId& lsid) const;

    private:
        KillAllSessionsByPatternSet _patterns;
        LogicalSessionIdMap<const KillAllSessionsByPattern*> _lsids;
        stdx::unordered_map<SHA256Block, const KillAllSessionsByPattern*, SHA256Block::Hash> _uids;
        const KillAllSessionsByPattern* _killAll = nullptr;
    };

    /**
     * A process specific kill function (we have a different impl in mongos versus mongod).
     */
    using KillFunc =
        std::function<Result(OperationContext*, const Matcher&, UniformRandomBitGenerator* urbg)>;

    /**
     * The killer lives as a decoration on the service context.
     */
    static SessionKiller* get(ServiceContext* service);
    static SessionKiller* get(OperationContext* opCtx);

    /**
     * This method binds the SessionKiller to the ServiceContext.
     */
    static void set(ServiceContext* ctx, std::shared_ptr<SessionKiller> sk);

    static void shutdown(ServiceContext* ctx);

    explicit SessionKiller(ServiceContext* sc, KillFunc killer);
    ~SessionKiller();

    /**
     * This is the api for killSessions commands to invoke the killer.  It blocks until the kill is
     * finished, or until it fails (times out on all nodes in mongos).
     */
    std::shared_ptr<Result> kill(OperationContext* opCtx,
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

    void _periodicKill(OperationContext* opCtx, stdx::unique_lock<Latch>& lk);

    KillFunc _killFunc;

    stdx::thread _thread;

    Mutex _mutex = MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(2), "SessionKiller::_mutex");
    stdx::condition_variable _callerCV;
    stdx::condition_variable _killerCV;

    UniformRandomBitGenerator _urbg;

    ReapResult _reapResults;
    KillAllSessionsByPatternSet _nextToReap;

    bool _inShutdown = false;
};

}  // namespace mongo
