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

#include "mongo/platform/basic.h"

#include "mongo/db/session_killer.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/destructor_guard.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

namespace {
const auto getSessionKiller = ServiceContext::declareDecoration<std::shared_ptr<SessionKiller>>();
}  // namespace

SessionKiller::SessionKiller(ServiceContext* sc, KillFunc killer)
    : _killFunc(std::move(killer)), _urbg(std::random_device{}()), _reapResults() {
    _thread = stdx::thread([this, sc] {
        // This is the background killing thread

        Client::setCurrent(sc->makeClient("SessionKiller"));

        stdx::unique_lock<stdx::mutex> lk(_mutex);

        // While we're not in shutdown
        while (!_inShutdown) {
            // Wait until we're woken up, and should either shutdown, or have something new to reap.
            _killerCV.wait(lk, [&] { return _inShutdown || _nextToReap.size(); });

            // If we're in shutdown we're done
            if (_inShutdown) {
                return;
            }

            // Otherwise make an opctx and head into kill
            auto opCtx = cc().makeOperationContext();
            _periodicKill(opCtx.get(), lk);
        }
    });
}

SessionKiller::~SessionKiller() {
    DESTRUCTOR_GUARD([&] {
        {
            stdx::lock_guard<stdx::mutex> lk(_mutex);
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
    for (const auto& pattern : _patterns) {
        if (pattern.getUid()) {
            _uids.emplace(pattern.getUid().get(), &pattern);
        } else if (pattern.getLsid()) {
            _lsids.emplace(pattern.getLsid().get(), &pattern);
        } else {
            // If we're killing everything, it's the only pattern we care about.
            decltype(_patterns) onlyKillAll{{pattern}};
            using std::swap;
            swap(_patterns, onlyKillAll);

            _killAll = &(*_patterns.begin());
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

    {
        auto iter = _lsids.find(lsid);
        if (iter != _lsids.end()) {
            return iter->second;
        }
    }

    {
        auto iter = _uids.find(lsid.getUid());
        if (iter != _uids.end()) {
            return iter->second;
        }
    }

    return nullptr;
}

SessionKiller* SessionKiller::get(ServiceContext* service) {
    return getSessionKiller(service).get();
}

SessionKiller* SessionKiller::get(OperationContext* ctx) {
    return get(ctx->getServiceContext());
}

std::shared_ptr<SessionKiller::Result> SessionKiller::kill(
    OperationContext* opCtx, const KillAllSessionsByPatternSet& toKill) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

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

void SessionKiller::_periodicKill(OperationContext* opCtx, stdx::unique_lock<stdx::mutex>& lk) {
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

    Matcher matcher(std::move(nextToReap));
    boost::optional<Result> results;
    try {
        results.emplace(_killFunc(opCtx, matcher, &_urbg));
    } catch (...) {
        results.emplace(exceptionToStatus());
    }
    lk.lock();

    invariant(results);

    // Expose the results and notify callers
    *(reapResults.result) = std::move(results);
    _callerCV.notify_all();
};

void SessionKiller::set(ServiceContext* sc, std::shared_ptr<SessionKiller> sk) {
    getSessionKiller(sc) = sk;
}

}  // namespace mongo
