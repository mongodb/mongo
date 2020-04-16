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

#pragma once

#include <array>

#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/mutex.h"
#include "mongo/transport/session.h"

namespace mongo {

class VectorClockMutable;

/**
 * The VectorClock service provides a collection of cluster-wide logical clocks (including the
 * clusterTime), that are used to provide causal-consistency to various other services.
 */
class VectorClock {
public:
    enum class Component : uint8_t {
        ClusterTime = 0,
        ConfigTime = 1,
        _kNumComponents = 2,
    };

private:
    template <typename T>
    class ComponentArray
        : public std::array<T, static_cast<unsigned long>(Component::_kNumComponents)> {
    public:
        const T& operator[](Component component) const {
            invariant(component != Component::_kNumComponents);
            return std::array<T, static_cast<unsigned long>(Component::_kNumComponents)>::
            operator[](static_cast<unsigned long>(component));
        }

        T& operator[](Component component) {
            invariant(component != Component::_kNumComponents);
            return std::array<T, static_cast<unsigned long>(Component::_kNumComponents)>::
            operator[](static_cast<unsigned long>(component));
        }

    private:
        const T& operator[](unsigned long i) const;
        T& operator[](unsigned long i);
    };

protected:
    using LogicalTimeArray = ComponentArray<LogicalTime>;

public:
    class VectorTime {
    public:
        LogicalTime operator[](Component component) const {
            return _time[component];
        }

    private:
        friend class VectorClock;

        explicit VectorTime(LogicalTimeArray time) : _time(time) {}

        const LogicalTimeArray _time;
    };

    // Decorate ServiceContext with VectorClock* which points to the actual vector clock
    // implementation.
    static VectorClock* get(ServiceContext* service);
    static VectorClock* get(OperationContext* ctx);

    static void registerVectorClockOnServiceContext(ServiceContext* service,
                                                    VectorClock* vectorClock);

    VectorTime getTime() const;

    // Gossipping
    void gossipOut(BSONObjBuilder* outMessage,
                   const transport::Session::TagMask clientSessionTags) const;
    void gossipIn(const BSONObj& inMessage, const transport::Session::TagMask clientSessionTags);

    bool isEnabled() const;
    void disable();

protected:
    VectorClock();
    virtual ~VectorClock();

    static std::string _componentName(Component component);

    // Internal Gossipping API
    virtual void _gossipOutInternal(BSONObjBuilder* out) const = 0;
    virtual void _gossipOutExternal(BSONObjBuilder* out) const = 0;
    virtual LogicalTimeArray _gossipInInternal(const BSONObj& in) = 0;
    virtual LogicalTimeArray _gossipInExternal(const BSONObj& in) = 0;

    void _gossipOutComponent(BSONObjBuilder* out, VectorTime time, Component component) const;
    void _gossipInComponent(const BSONObj& in, LogicalTimeArray* newTime, Component component);

    // Used to atomically advance the time of several components (eg. during gossip-in).
    void _advanceTime(LogicalTimeArray&& newTime);

    ServiceContext* _service{nullptr};

    // The mutex protects _vectorTime and _isEnabled.
    //
    // Note that ConfigTime is advanced under the ReplicationCoordinator mutex, so to avoid
    // potential deadlocks the ReplicationCoordator mutex should never be acquired whilst the
    // VectorClock mutex is held.
    mutable Mutex _mutex = MONGO_MAKE_LATCH("VectorClock::_mutex");

    LogicalTimeArray _vectorTime;
    bool _isEnabled{true};

private:
    class GossipFormat;
};

}  // namespace mongo
