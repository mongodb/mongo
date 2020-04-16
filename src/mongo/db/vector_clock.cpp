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

#include "mongo/db/vector_clock.h"

namespace mongo {

namespace {

const auto vectorClockDecoration = ServiceContext::declareDecoration<VectorClock*>();

}  // namespace

VectorClock* VectorClock::get(ServiceContext* service) {
    return vectorClockDecoration(service);
}

VectorClock* VectorClock::get(OperationContext* ctx) {
    return get(ctx->getClient()->getServiceContext());
}

VectorClock::VectorClock() = default;

VectorClock::~VectorClock() = default;

void VectorClock::registerVectorClockOnServiceContext(ServiceContext* service,
                                                      VectorClock* vectorClock) {
    invariant(!vectorClock->_service);
    vectorClock->_service = service;
    auto& clock = vectorClockDecoration(service);
    invariant(!clock);
    clock = std::move(vectorClock);
}

VectorClock::VectorTime VectorClock::getTime() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return VectorTime(_vectorTime);
}

void VectorClock::_advanceTime(LogicalTimeArray&& newTime) {
    stdx::lock_guard<Latch> lock(_mutex);
    auto it = _vectorTime.begin();
    auto newIt = newTime.begin();
    for (; it != _vectorTime.end() && newIt != newTime.end(); ++it, ++newIt) {
        if (*newIt > *it) {
            *it = std::move(*newIt);
        }
    }
}

class VectorClock::GossipFormat {
public:
    class Plain;
    class Signed;

    static const ComponentArray<std::unique_ptr<GossipFormat>> _formatters;

    GossipFormat(std::string fieldName) : _fieldName(fieldName) {}
    virtual ~GossipFormat() = default;

    virtual void out(BSONObjBuilder* out, LogicalTime time, Component component) const = 0;
    virtual LogicalTime in(const BSONObj& in, Component component) const = 0;

    const std::string _fieldName;
};

class VectorClock::GossipFormat::Plain : public VectorClock::GossipFormat {
public:
    using GossipFormat::GossipFormat;
    virtual ~Plain() = default;

    void out(BSONObjBuilder* out, LogicalTime time, Component component) const override {
        out->append(_fieldName, time.asTimestamp());
    }

    LogicalTime in(const BSONObj& in, Component component) const override {
        const auto componentElem(in[_fieldName]);
        if (componentElem.eoo()) {
            // Nothing to gossip in.
            return LogicalTime();
        }
        uassert(ErrorCodes::BadValue,
                str::stream() << _fieldName << " is not a Timestamp",
                componentElem.type() == bsonTimestamp);
        return LogicalTime(componentElem.timestamp());
    }
};

class VectorClock::GossipFormat::Signed : public VectorClock::GossipFormat {
public:
    using GossipFormat::GossipFormat;
    virtual ~Signed() = default;

    void out(BSONObjBuilder* out, LogicalTime time, Component component) const override {
        // TODO SERVER-47914: make this do the actual proper signing
        BSONObjBuilder bob;
        bob.append("time", time.asTimestamp());
        bob.append("signature", 0);
        out->append(_fieldName, bob.done());
    }

    LogicalTime in(const BSONObj& in, Component component) const override {
        // TODO SERVER-47914: make this do the actual proper signing
        const auto componentElem(in[_fieldName]);
        if (componentElem.eoo()) {
            // Nothing to gossip in.
            return LogicalTime();
        }
        uassert(ErrorCodes::BadValue,
                str::stream() << _fieldName << " is not a sub-object",
                componentElem.isABSONObj());
        const auto subobj = componentElem.embeddedObject();
        const auto timeElem(subobj["time"]);
        uassert(ErrorCodes::FailedToParse, "No time found", !timeElem.eoo());
        uassert(ErrorCodes::BadValue,
                str::stream() << "time is not a Timestamp",
                timeElem.type() == bsonTimestamp);
        return LogicalTime(timeElem.timestamp());
    }
};

// TODO SERVER-47914: update $clusterTimeNew to $clusterTime once LogicalClock is migrated into
// VectorClock.
const VectorClock::ComponentArray<std::unique_ptr<VectorClock::GossipFormat>>
    VectorClock::GossipFormat::_formatters{
        std::make_unique<VectorClock::GossipFormat::Signed>("$clusterTimeNew"),
        std::make_unique<VectorClock::GossipFormat::Plain>("$configTime")};

void VectorClock::gossipOut(BSONObjBuilder* outMessage,
                            const transport::Session::TagMask clientSessionTags) const {
    if (clientSessionTags & transport::Session::kInternalClient) {
        _gossipOutInternal(outMessage);
    } else {
        _gossipOutExternal(outMessage);
    }
}

void VectorClock::gossipIn(const BSONObj& inMessage,
                           const transport::Session::TagMask clientSessionTags) {
    if (clientSessionTags & transport::Session::kInternalClient) {
        _advanceTime(_gossipInInternal(inMessage));
    } else {
        _advanceTime(_gossipInExternal(inMessage));
    }
}

void VectorClock::_gossipOutComponent(BSONObjBuilder* out,
                                      VectorTime time,
                                      Component component) const {
    GossipFormat::_formatters[component]->out(out, time[component], component);
}

void VectorClock::_gossipInComponent(const BSONObj& in,
                                     LogicalTimeArray* newTime,
                                     Component component) {
    (*newTime)[component] = GossipFormat::_formatters[component]->in(in, component);
}

std::string VectorClock::_componentName(Component component) {
    return GossipFormat::_formatters[component]->_fieldName;
}

bool VectorClock::isEnabled() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _isEnabled;
}

void VectorClock::disable() {
    stdx::lock_guard<Latch> lock(_mutex);
    _isEnabled = false;
}

}  // namespace mongo
