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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/util/latch_analyzer.h"

#include <fmt/format.h>

#include "mongo/util/hierarchical_acquisition.h"

#include "mongo/base/init.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/latch_analyzer.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {

auto kLatchAnalysisName = "latchAnalysis"_sd;
auto kLatchViolationKey = "hierarchicalAcquisitionLevelViolations"_sd;

// LatchAnalyzer Decoration getter
const auto getLatchAnalyzer = ServiceContext::declareDecoration<LatchAnalyzer>();

/**
 * LockListener sub-class to implement updating set in LatchSetState
 */
class LockListener : public Mutex::LockListener {
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

// Register our LockListener with the Mutex class
MONGO_INITIALIZER(LatchAnalysis)(InitializerContext* context) {

    // Intentionally leaked, people use Latches in detached threads
    static auto& listener = *new LockListener;
    Mutex::addLockListener(&listener);

    return Status::OK();
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
    HierarchicalAcquisitionSet levelsHeld;
    stdx::unordered_set<const latch_detail::Identity*> latchesHeld;
};

const auto getLatchSetState = Client::declareDecoration<LatchSetState>();

}  // namespace

void LatchAnalyzer::setAllowExitOnViolation(bool allowExitOnViolation) {
    _allowExitOnViolation.store(allowExitOnViolation);
}

bool LatchAnalyzer::allowExitOnViolation() {
    return _allowExitOnViolation.load() && (getTestCommandsEnabled());
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
    if (result != HierarchicalAcquisitionSet::AddResult::kValidWasAbsent) {
        using namespace fmt::literals;

        auto errorMessage =
            "Theoretical deadlock alert - {} latch acquisition at {}:{:d} on latch {}"_format(
                toString(result),
                identity.sourceLocation()->file_name(),
                identity.sourceLocation()->line(),
                identity.name());

        if (allowExitOnViolation()) {
            fassert(31360, Status(ErrorCodes::HierarchicalAcquisitionLevelViolation, errorMessage));
        } else {
            warning() << errorMessage;

            {
                stdx::lock_guard lk(_mutex);

                auto& violation = _violations[identity.index()];
                ++violation.onAcquire;
            }
        }
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
    if (result != HierarchicalAcquisitionSet::RemoveResult::kValidWasPresent) {
        using namespace fmt::literals;

        auto errorMessage =
            "Theoretical deadlock alert - {} latch release at {}:{} on latch {}"_format(
                toString(result),
                identity.sourceLocation()->file_name(),
                identity.sourceLocation()->line(),
                identity.name());

        if (allowExitOnViolation()) {
            fassert(31361, Status(ErrorCodes::HierarchicalAcquisitionLevelViolation, errorMessage));
        } else {
            warning() << errorMessage;

            {
                stdx::lock_guard lk(_mutex);

                auto& violation = _violations[identity.index()];
                ++violation.onRelease;
            }
        }
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
    {
        BSONObjBuilder analysis = bob.subobjStart("latchAnalysis");
        appendToBSON(analysis);
    }

    auto obj = bob.done();
    log().setIsTruncatable(false) << "=====LATCHES=====\n"
                                  << obj.jsonString(JsonStringFormat::LegacyStrict)
                                  << "\n===END LATCHES===";
}

LatchAnalyzerDisabledBlock::LatchAnalyzerDisabledBlock() {
    LatchAnalyzer::get().setAllowExitOnViolation(false);
}

LatchAnalyzerDisabledBlock::~LatchAnalyzerDisabledBlock() {
    LatchAnalyzer::get().setAllowExitOnViolation(true);
}

}  // namespace mongo
