// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/vector_clock/vector_clock.h"
#include "mongo/db/topology/vector_clock/vector_clock_mutable.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/future.h"

#include <cstdint>
#include <string>
#include <utility>

namespace mongo {
namespace {

/**
 * Vector clock implementation for non-distributed environments (embedded, some unittests).
 */
class VectorClockTrivial : public VectorClockMutable {
    VectorClockTrivial(const VectorClockTrivial&) = delete;
    VectorClockTrivial& operator=(const VectorClockTrivial&) = delete;

public:
    VectorClockTrivial();
    ~VectorClockTrivial() override;

private:
    // VectorClock methods implementation

    ComponentSet _getGossipInternalComponents() const override;
    ComponentSet _getGossipExternalComponents() const override;

    // VectorClockMutable methods implementation

    SharedSemiFuture<void> waitForDurableConfigTime() override {
        // VectorClockTrivial does not support persistence
        MONGO_UNIMPLEMENTED_TASSERT(10083537);
    }

    SharedSemiFuture<void> waitForDurableTopologyTime() override {
        // VectorClockTrivial does not support persistence
        MONGO_UNIMPLEMENTED_TASSERT(10083538);
    }

    SharedSemiFuture<void> waitForDurable() override {
        // VectorClockTrivial does not support persistence
        MONGO_UNIMPLEMENTED_TASSERT(10083539);
    }

    VectorClock::VectorTime recoverDirect(OperationContext* opCtx) override {
        // VectorClockTrivial does not support persistence
        MONGO_UNIMPLEMENTED_TASSERT(10083540);
    }

    LogicalTime _tick(Component component, uint64_t nTicks) override;
    void _tickTo(Component component, LogicalTime newTime) override;
};

const auto vectorClockTrivialDecoration = ServiceContext::declareDecoration<VectorClockTrivial>();

ServiceContext::ConstructorActionRegisterer vectorClockTrivialRegisterer(
    "VectorClockTrivial", {"VectorClock"}, [](ServiceContext* service) {
        VectorClockTrivial::registerVectorClockOnServiceContext(
            service, &vectorClockTrivialDecoration(service));
    });

VectorClockTrivial::VectorClockTrivial() = default;

VectorClockTrivial::~VectorClockTrivial() = default;

VectorClock::ComponentSet VectorClockTrivial::_getGossipInternalComponents() const {
    // Clocks are not gossipped in trivial (non-distributed) environments.
    return ComponentSet{};
}

VectorClock::ComponentSet VectorClockTrivial::_getGossipExternalComponents() const {
    // Clocks are not gossipped in trivial (non-distributed) environments.
    return ComponentSet{};
}

LogicalTime VectorClockTrivial::_tick(Component component, uint64_t nTicks) {
    return _advanceComponentTimeByTicks(component, nTicks);
}

void VectorClockTrivial::_tickTo(Component component, LogicalTime newTime) {
    _advanceComponentTimeTo(component, std::move(newTime));
}

}  // namespace
}  // namespace mongo
