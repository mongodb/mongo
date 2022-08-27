/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/util/latch_analyzer.h"

#include <boost/iterator/transform_iterator.hpp>
#include <deque>

#include <fmt/format.h>

#include "mongo/base/init.h"
#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/hierarchical_acquisition.h"
#include "mongo/util/latch_analyzer.h"
#include "mongo/util/testing_proctor.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {

using namespace fmt::literals;

namespace {

auto kLatchAnalysisName = "latchAnalysis"_sd;
auto kLatchViolationKey = "hierarchicalAcquisitionLevelViolations"_sd;

// LatchAnalyzer Decoration getter
const auto getLatchAnalyzer = ServiceContext::declareDecoration<LatchAnalyzer>();

/**
 * DiagnosticListener sub-class to implement updating set in LatchSetState
 */
class DiagnosticListener : public latch_detail::DiagnosticListener {
public:
    void onContendedLock(const Identity& id) override {
        if (auto client = Client::getCurrent()) {
            LatchAnalyzer::get(client).onContention(id);
        }
    }

    void onQuickLock(const Identity& id) override {
        if (auto client = Client::getCurrent()) {
            LatchAnalyzer::get(client).onAcquire(id);
        }
    }

    void onSlowLock(const Identity& id) override {
        if (auto client = Client::getCurrent()) {
            LatchAnalyzer::get(client).onAcquire(id);
        }
    }

    void onUnlock(const Identity& id) override {
        if (auto client = Client::getCurrent()) {
            LatchAnalyzer::get(client).onRelease(id);
        }
    }
};

// Register our DiagnosticListener
MONGO_INITIALIZER_GENERAL(LatchAnalysis, (/* NO PREREQS */), ("FinalizeDiagnosticListeners"))
(InitializerContext* context) {
    latch_detail::installDiagnosticListener<DiagnosticListener>();
}

// Create a FailPoint to analyze latches more seriously for diagnostic purposes. This can be used
// with a new set of test suites to verify our lock hierarchies.
MONGO_FAIL_POINT_DEFINE(enableLatchAnalysis);

bool shouldAnalyzeLatches() {
    return enableLatchAnalysis.shouldFail();
}

// Define a new serverStatus section "latchAnalysis"
class LatchAnalysisSection final : public ServerStatusSection {
public:
    LatchAnalysisSection() : ServerStatusSection(kLatchAnalysisName.toString()) {}

    bool includeByDefault() const override {
        return false;
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement&) const override {
        BSONObjBuilder analysis;
        LatchAnalyzer::get(opCtx->getClient()).appendToBSON(analysis);
        return analysis.obj();
    };
} gLatchAnalysisSection;

// Latching state object to pin onto the Client (i.e. thread)
struct LatchSetState {
    using LatchIdentitySet = std::deque<const latch_detail::Identity*>;

    LatchSetState() {
        if (TestingProctor::instance().isEnabled()) {
            identities = std::make_unique<LatchIdentitySet>();
        }
    }

    HierarchicalAcquisitionSet levelsHeld;

    // This is a set of latches by unique Identity alone. It is not and cannot be in order of
    // acquisition or release. We only populate this when shouldAnalyzeLatches() is true.
    stdx::unordered_set<const latch_detail::Identity*> latchesHeld;

    // This is an ordered list of latch Identities. Each acquired Latch will add itself to the end
    // of this list and each released Latch will remove itself from the end. This is populated when
    // TestingProctor::instance().isEnabled() is true, i.e. in a testing environment.
    std::unique_ptr<LatchIdentitySet> identities;
};

const auto getLatchSetState = Client::declareDecoration<LatchSetState>();
}  // namespace

void LatchAnalyzer::setAllowExitOnViolation(bool allowExitOnViolation) {
    _allowExitOnViolation.store(allowExitOnViolation);
}

bool LatchAnalyzer::allowExitOnViolation() {
    return _allowExitOnViolation.load() && TestingProctor::instance().isEnabled();
}

LatchAnalyzer& LatchAnalyzer::get(ServiceContext* serviceContext) {
    return getLatchAnalyzer(serviceContext);
}

LatchAnalyzer& LatchAnalyzer::get(Client* client) {
    return get(client->getServiceContext());
}

LatchAnalyzer& LatchAnalyzer::get() {
    auto serviceContext = getCurrentServiceContext();
    invariant(serviceContext);
    return get(serviceContext);
}

void LatchAnalyzer::onContention(const latch_detail::Identity&) {
    // Nothing at the moment
}

void LatchAnalyzer::_handleAcquireViolation(ErrorCodes::Error ec,
                                            StringData message,
                                            const latch_detail::Identity& identity,
                                            Client* client) noexcept {

    {
        stdx::lock_guard lk(_mutex);

        auto& violation = _violations[identity.index()];
        ++violation.onAcquire;
    }

    _handleViolation(ec, message, identity, client);
}

void LatchAnalyzer::_handleReleaseViolation(ErrorCodes::Error ec,
                                            StringData message,
                                            const latch_detail::Identity& identity,
                                            Client* client) noexcept {

    {
        stdx::lock_guard lk(_mutex);

        auto& violation = _violations[identity.index()];
        ++violation.onRelease;
    }

    _handleViolation(ec, message, identity, client);
}

void LatchAnalyzer::_handleViolation(ErrorCodes::Error ec,
                                     StringData message,
                                     const latch_detail::Identity& identity,
                                     Client* client) noexcept {
    if (allowExitOnViolation()) {
        auto identities = LatchSetState::LatchIdentitySet{};

        auto& state = getLatchSetState(client);
        if (state.identities) {
            // We're in fatal territory, we can take our Client's list to the local stack.
            identities = std::move(*state.identities);
        }

        const auto derefIdentity = [](const auto& id) -> const latch_detail::Identity& {
            return *id;
        };
        auto begin = boost::make_transform_iterator(identities.begin(), derefIdentity);
        auto end = boost::make_transform_iterator(identities.end(), derefIdentity);

        LOGV2_FATAL_OPTIONS(ec,
                            {logv2::LogTruncation::Disabled},
                            "Theoretical deadlock found on use of latch",
                            "reason"_attr = message,
                            "latch"_attr = identity,
                            "latchesHeld"_attr = logv2::seqLog(begin, end));
    } else {
        LOGV2_WARNING(ec,
                      "Theoretical deadlock found on use of latch",
                      "reason"_attr = message,
                      "latch"_attr = identity);
    }
}

void LatchAnalyzer::onAcquire(const latch_detail::Identity& identity) {
    auto client = Client::getCurrent();
    if (!client) {
        return;
    }

    if (shouldAnalyzeLatches()) {
        // If we should analyze latches, annotate the Client state
        auto& latchSet = getLatchSetState(client).latchesHeld;

        stdx::lock_guard lk(_mutex);
        for (auto otherIdentity : latchSet) {
            auto& stat = _hierarchies[identity.index()][otherIdentity->index()];
            stat.identity = otherIdentity;
            ++stat.acquiredAfter;
        }

        latchSet.insert(&identity);
    }

    if (!identity.level()) {
        // If we weren't given a HierarchicalAcquisitionLevel, don't verify hierarchies
        return;
    }

    auto level = *identity.level();
    auto& handle = getLatchSetState(client);
    auto result = handle.levelsHeld.add(level);
    switch (result) {
        case HierarchicalAcquisitionSet::AddResult::kValidWasAbsent: {
            // The good result. Nothing to do.
        } break;
        case HierarchicalAcquisitionSet::AddResult::kInvalidWasAbsent: {
            _handleAcquireViolation(ErrorCodes::Error{5106800},
                                    "Latch acquired after other latch of lower level"_sd,
                                    identity,
                                    client);
        } break;
        case HierarchicalAcquisitionSet::AddResult::kInvalidWasPresent: {
            _handleAcquireViolation(ErrorCodes::Error{5106801},
                                    "Latch acquired after other latch of same level"_sd,
                                    identity,
                                    client);
        } break;
    };

    if (handle.identities) {
        // Since this latch has a verified Level, we can add it to the stack of identities
        handle.identities->push_back(&identity);
    }
}

void LatchAnalyzer::onRelease(const latch_detail::Identity& identity) {
    auto client = Client::getCurrent();
    if (!client) {
        return;
    }

    if (shouldAnalyzeLatches()) {
        // If we should analyze latches, annotate the Client state
        auto& latchSet = getLatchSetState(client).latchesHeld;
        latchSet.erase(&identity);

        stdx::lock_guard lk(_mutex);
        for (auto otherIdentity : latchSet) {
            auto& stat = _hierarchies[identity.index()][otherIdentity->index()];
            stat.identity = otherIdentity;
            ++stat.releasedBefore;
        }
    }

    if (!identity.level()) {
        // If we weren't given a HierarchicalAcquisitionLevel, don't verify hierarchies
        return;
    }

    auto level = *identity.level();
    auto& handle = getLatchSetState(client);
    auto result = handle.levelsHeld.remove(level);
    switch (result) {
        case HierarchicalAcquisitionSet::RemoveResult::kValidWasPresent: {
            // The good result. Nothing to do.
        } break;
        case HierarchicalAcquisitionSet::RemoveResult::kInvalidWasAbsent: {
            _handleReleaseViolation(
                ErrorCodes::Error{5106802},
                "Latch released after other latch of same level (usually the same latch twice)"_sd,
                identity,
                client);
        } break;
        case HierarchicalAcquisitionSet::RemoveResult::kInvalidWasPresent: {
            _handleReleaseViolation(ErrorCodes::Error{5106803},
                                    "Latch released before other latch of lower level"_sd,
                                    identity,
                                    client);
        } break;
    };

    if (handle.identities) {
        // Since this latch has a verified Level, we can remove it from the stack of identities
        handle.identities->pop_back();
    }
}

void LatchAnalyzer::appendToBSON(mongo::BSONObjBuilder& result) const {
    for (auto iter = latch_detail::Catalog::get().iter(); iter.more();) {
        auto data = iter.next();
        if (!data) {
            continue;
        }

        auto& identity = data->identity();

        BSONObjBuilder latchObj = result.subobjStart(identity.name());
        latchObj.append("created", data->counts().created.loadRelaxed());
        latchObj.append("destroyed", data->counts().destroyed.loadRelaxed());
        latchObj.append("acquired", data->counts().acquired.loadRelaxed());
        latchObj.append("released", data->counts().released.loadRelaxed());
        latchObj.append("contended", data->counts().contended.loadRelaxed());

        auto appendViolations = [&] {
            stdx::lock_guard lk(_mutex);
            auto it = _violations.find(identity.index());
            if (it == _violations.end()) {
                return;
            }
            auto& violation = it->second;

            BSONObjBuilder violationObj = latchObj.subobjStart(kLatchViolationKey);
            violationObj.append("onAcquire", violation.onAcquire);
            violationObj.append("onRelease", violation.onRelease);
        };

        appendViolations();

        if (!shouldAnalyzeLatches()) {
            // Only append hierarchical information if we should analyze latches
            continue;
        }

        stdx::lock_guard lk(_mutex);
        auto it = _hierarchies.find(identity.index());
        if (it == _hierarchies.end()) {
            continue;
        }

        auto& latchHierarchy = it->second;
        if (latchHierarchy.empty()) {
            continue;
        }

        {
            BSONObjBuilder acquiredAfterObj = latchObj.subobjStart("acquiredAfter");
            for (auto& [_, stat] : latchHierarchy) {
                auto count = stat.acquiredAfter;
                if (count == 0) {
                    continue;
                }
                acquiredAfterObj.append(stat.identity->name(), count);
            }
        }

        {
            BSONObjBuilder releasedBeforeObj = latchObj.subobjStart("releasedBefore");
            for (auto& [_, stat] : latchHierarchy) {
                auto count = stat.releasedBefore;
                if (count == 0) {
                    continue;
                }
                releasedBeforeObj.append(stat.identity->name(), count);
            }
        }
    }
}

void LatchAnalyzer::dump() {
    if (!shouldAnalyzeLatches()) {
        return;
    }

    BSONObjBuilder bob(1024 * 1024);
    appendToBSON(bob);

    LOGV2_OPTIONS(25003,
                  {logv2::LogTruncation::Disabled},
                  "LatchAnalyzer dump",
                  "latchAnalysis"_attr = bob.done());
}

LatchAnalyzerDisabledBlock::LatchAnalyzerDisabledBlock() {
    LatchAnalyzer::get().setAllowExitOnViolation(false);
}

LatchAnalyzerDisabledBlock::~LatchAnalyzerDisabledBlock() {
    LatchAnalyzer::get().setAllowExitOnViolation(true);
}

}  // namespace mongo
