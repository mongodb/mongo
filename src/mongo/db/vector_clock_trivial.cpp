/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/vector_clock_mutable.h"

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
    virtual ~VectorClockTrivial();

private:
    // VectorClock methods implementation

    bool _gossipOutInternal(OperationContext* opCtx,
                            BSONObjBuilder* out,
                            const LogicalTimeArray& time) const override;
    bool _gossipOutExternal(OperationContext* opCtx,
                            BSONObjBuilder* out,
                            const LogicalTimeArray& time) const override;
    LogicalTimeArray _gossipInInternal(OperationContext* opCtx,
                                       const BSONObj& in,
                                       bool couldBeUnauthenticated) override;
    LogicalTimeArray _gossipInExternal(OperationContext* opCtx,
                                       const BSONObj& in,
                                       bool couldBeUnauthenticated) override;
    bool _permitRefreshDuringGossipOut() const override {
        return false;
    }

    // VectorClockMutable methods implementation

    LogicalTime _tick(Component component, uint64_t nTicks) override;
    void _tickTo(Component component, LogicalTime newTime) override;
};

const auto vectorClockTrivialDecoration = ServiceContext::declareDecoration<VectorClockTrivial>();

ServiceContext::ConstructorActionRegisterer vectorClockTrivialRegisterer(
    "VectorClockTrivial-VectorClockRegistration",
    {},
    [](ServiceContext* service) {
        VectorClockTrivial::registerVectorClockOnServiceContext(
            service, &vectorClockTrivialDecoration(service));
    },
    {});

VectorClockTrivial::VectorClockTrivial() = default;

VectorClockTrivial::~VectorClockTrivial() = default;

bool VectorClockTrivial::_gossipOutInternal(OperationContext* opCtx,
                                            BSONObjBuilder* out,
                                            const LogicalTimeArray& time) const {
    // Clocks are not gossipped in trivial (non-distributed) environments.
    return false;
}

bool VectorClockTrivial::_gossipOutExternal(OperationContext* opCtx,
                                            BSONObjBuilder* out,
                                            const LogicalTimeArray& time) const {
    // Clocks are not gossipped in trivial (non-distributed) environments.
    return false;
}

VectorClock::LogicalTimeArray VectorClockTrivial::_gossipInInternal(OperationContext* opCtx,
                                                                    const BSONObj& in,
                                                                    bool couldBeUnauthenticated) {
    // Clocks are not gossipped in trivial (non-distributed) environments.
    return {};
}

VectorClock::LogicalTimeArray VectorClockTrivial::_gossipInExternal(OperationContext* opCtx,
                                                                    const BSONObj& in,
                                                                    bool couldBeUnauthenticated) {
    // Clocks are not gossipped in trivial (non-distributed) environments.
    return {};
}

LogicalTime VectorClockTrivial::_tick(Component component, uint64_t nTicks) {
    return _advanceComponentTimeByTicks(component, nTicks);
}

void VectorClockTrivial::_tickTo(Component component, LogicalTime newTime) {
    _advanceComponentTimeTo(component, std::move(newTime));
}

}  // namespace
}  // namespace mongo
