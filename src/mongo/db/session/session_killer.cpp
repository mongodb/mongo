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

#include <absl/container/node_hash_map.h>
#include <absl/container/node_hash_set.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "cxxabi.h"
#include <iterator>
#include <utility>

#include "mongo/base/error_codes.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/session/session_killer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/destructor_guard.h"

namespace mongo {

namespace {
const auto getSessionKiller = Service::declareDecoration<std::shared_ptr<SessionKiller>>();
}  // namespace

SessionKiller::SessionKiller(Service* service, KillFunc killer)
    : _killFunc(std::move(killer)), _urbg(std::random_device{}()), _reapResults() {
    _thread = stdx::thread([this, service] {
        // This is the background killing thread

        ThreadClient tc("SessionKiller", service);

        // TODO(SERVER-74658): Please revisit if this thread could be made killable.
        {
            stdx::lock_guard<Client> lk(*tc.get());
            tc.get()->setSystemOperationUnkillableByStepdown(lk);
        }

        stdx::unique_lock<Latch> lk(_mutex);

        // While we're not in shutdown
        while (!_inShutdown) {
            // Wait until we're woken up, and should either shutdown, or have something new to reap.
            _killerCV.wait(lk, [&] { return _inShutdown || !_nextToReap.empty(); });

            // If we're in shutdown we're done
            if (_inShutdown) {
                return;
            }

            // Otherwise make an opctx and head into kill.
            //
            // We must unlock around making an operation context, because making an opCtx can block
            // waiting for all old opCtx's to be killed, and elsewhere we hold this mutex while we
            // have an opCtx.
            lk.unlock();
            auto opCtx = cc().makeOperationContext();
            lk.lock();

            // Double-check shutdown since we released the lock.  We don't have to worry about
            // _nextToReap becoming empty because only this thread can empty it.
            if (_inShutdown) {
                return;
            }
            _periodicKill(opCtx.get(), lk);
        }
    });
}

SessionKiller::~SessionKiller() {
    DESTRUCTOR_GUARD([&] {
        {
            stdx::lock_guard<Latch> lk(_mutex);
            _inShutdown = true;
        }
        _killerCV.notify_one();
        _callerCV.notify_all();
        _thread.join();
    }());
}

SessionKiller::ReapResult::ReapResult() : result(std::make_shared<boost::optional<Result>>()) {}

SessionKiller::Matcher::Matcher(KillAllSessionsByPatternSet&& patterns)
    : _patterns(std::move(patterns)) {
    for (const auto& item : _patterns) {
        auto& pattern = item.pattern;
        if (pattern.getUid()) {
            _uids.emplace(pattern.getUid().value(), &pattern);
        } else if (pattern.getLsid()) {
            _lsids.emplace(pattern.getLsid().value(), &pattern);
        } else {
            // If we're killing everything, it's the only pattern we care about.
            decltype(_patterns) onlyKillAll{{item}};
            using std::swap;
            swap(_patterns, onlyKillAll);

            _killAll = &(_patterns.begin()->pattern);
            break;
        }
    }
}

const KillAllSessionsByPatternSet& SessionKiller::Matcher::getPatterns() const {
    return _patterns;
}

const KillAllSessionsByPattern* SessionKiller::Matcher::match(const LogicalSessionId& lsid) const {
    if (_killAll) {
        return _killAll;
    }

    // Since child and parent sessions are logically related, by default, we will convert any child
    // lsid to its corresponding parent lsid and match based on the converted lsid.
    auto parentSessionId = castToParentSessionId(lsid);

    {
        auto iter = _lsids.find(parentSessionId);
        if (iter != _lsids.end()) {
            return iter->second;
        }
    }

    {
        auto iter = _uids.find(parentSessionId.getUid());
        if (iter != _uids.end()) {
            return iter->second;
        }
    }

    return nullptr;
}

SessionKiller* SessionKiller::get(Service* service) {
    return getSessionKiller(service).get();
}

SessionKiller* SessionKiller::get(OperationContext* ctx) {
    return get(ctx->getService());
}

std::shared_ptr<SessionKiller::Result> SessionKiller::kill(
    OperationContext* opCtx, const KillAllSessionsByPatternSet& toKill) {
    stdx::unique_lock<Latch> lk(_mutex);

    // Save a shared_ptr to the current reapResults (I.e. the next thing to get killed).
    auto reapResults = _reapResults;

    // Dump all your lsids in.
    for (const auto& item : toKill) {
        _nextToReap.emplace(item);
    }

    // Wake up the killer.
    _killerCV.notify_one();

    // Wait until our results are there, or the killer is shutting down.
    opCtx->waitForConditionOrInterrupt(
        _callerCV, lk, [&] { return reapResults.result->is_initialized() || _inShutdown; });

    // If the killer is shutting down, throw.
    uassert(ErrorCodes::ShutdownInProgress, "SessionKiller shutting down", !_inShutdown);

    // Otherwise, alias (via the aliasing ctor of shared_ptr) a shared_ptr to the actual results
    // (inside the optional) to keep our contract.  That ctor form returns a shared_ptr which
    // returns one type, while keeping a refcount on a control block from a different type.
    return {reapResults.result, reapResults.result->get_ptr()};
}

void SessionKiller::_periodicKill(OperationContext* opCtx, stdx::unique_lock<Latch>& lk) {
    // Pull our current workload onto the stack.  Swap it for empties.
    decltype(_nextToReap) nextToReap;
    decltype(_reapResults) reapResults;
    {
        using std::swap;
        swap(nextToReap, _nextToReap);
        swap(reapResults, _reapResults);
    }

    // Drop the lock and run the killer.
    lk.unlock();

    // Group patterns with equal API parameters into sets.
    stdx::unordered_map<APIParameters, KillAllSessionsByPatternSet, APIParameters::Hash> sets;
    for (auto& item : nextToReap) {
        sets[item.apiParameters].insert(std::move(item));
    }

    // Use the API parameters included in each KillAllSessionsByPattern struct.
    IgnoreAPIParametersBlock ignoreApiParametersBlock(opCtx);
    Result finalResults{std::vector<HostAndPort>{}};
    for (auto& [apiParameters, items] : sets) {
        APIParameters::get(opCtx) = apiParameters;
        Matcher matcher(std::move(items));
        boost::optional<Result> results;
        try {
            results.emplace(_killFunc(opCtx, matcher, &_urbg));
        } catch (...) {
            results.emplace(exceptionToStatus());
        }
        invariant(results);
        if (!results->isOK()) {
            finalResults = *results;
            break;
        }

        finalResults.getValue().insert(finalResults.getValue().end(),
                                       std::make_move_iterator(results->getValue().begin()),
                                       std::make_move_iterator(results->getValue().end()));
    }

    // Expose the results and notify callers
    lk.lock();
    *(reapResults.result) = std::move(finalResults);
    _callerCV.notify_all();
};

void SessionKiller::set(Service* service, std::shared_ptr<SessionKiller> sk) {
    getSessionKiller(service) = sk;
}


void SessionKiller::shutdown(Service* service) {
    auto shared = getSessionKiller(service);
    getSessionKiller(service).reset();

    // Nuke
    shared.reset();
}

}  // namespace mongo
