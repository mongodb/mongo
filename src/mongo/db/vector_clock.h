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

/**
 * The VectorClock service provides a collection of cluster-wide logical clocks (including the
 * clusterTime), that are used to provide causal-consistency to various other services.
 */
class VectorClock {
protected:
    enum class Component : uint8_t {
        ClusterTime = 0,
        ConfigTime = 1,
        TopologyTime = 2,
        _kNumComponents = 3,
    };

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

    using LogicalTimeArray = ComponentArray<LogicalTime>;

    struct component_comparator {
        bool operator()(const Component& c0, const Component& c1) const {
            return static_cast<uint8_t>(c0) < static_cast<uint8_t>(c1);
        }
    };

    using ComponentSet = std::set<Component, component_comparator>;

public:
    class VectorTime {
    public:
        explicit VectorTime(LogicalTimeArray time) : _time(std::move(time)) {}
        VectorTime() = delete;

        LogicalTime clusterTime() const& {
            return _time[Component::ClusterTime];
        }

        LogicalTime configTime() const& {
            return _time[Component::ConfigTime];
        }

        LogicalTime topologyTime() const& {
            return _time[Component::TopologyTime];
        }

        LogicalTime clusterTime() const&& = delete;
        LogicalTime configTime() const&& = delete;
        LogicalTime topologyTime() const&& = delete;

        LogicalTime operator[](Component component) const {
            return _time[component];
        }

    private:
        friend class VectorClock;

        LogicalTimeArray _time;
    };

    // There is a special logic in the storage engine which fixes up Timestamp(0, 0) to the latest
    // available time on the node. Because of this, we should never gossip or have a VectorClock
    // initialised with a value of Timestamp(0, 0), because that would cause the checkpointed value
    // to move forward in time.
    static const LogicalTime kInitialComponentTime;

    /**
     * Returns true if the passed LogicalTime is set to a value higher than kInitialComponentTime,
     * false otherwise.
     */
    static bool isValidComponentTime(const LogicalTime& time) {
        return time > kInitialComponentTime;
    }

    static constexpr char kClusterTimeFieldName[] = "$clusterTime";
    static constexpr char kConfigTimeFieldName[] = "$configTime";
    static constexpr char kTopologyTimeFieldName[] = "$topologyTime";

    // Decorate ServiceContext with VectorClock* which points to the actual vector clock
    // implementation.
    static VectorClock* get(ServiceContext* service);
    static VectorClock* get(OperationContext* ctx);

    static const VectorClock* get(const ServiceContext* service);
    static const VectorClock* get(const OperationContext* ctx);

    static void registerVectorClockOnServiceContext(ServiceContext* service,
                                                    VectorClock* vectorClock);

    /**
     * Returns an instantaneous snapshot of the current time of all components.
     */
    VectorTime getTime() const;

    /**
     * Adds the necessary fields to outMessage to gossip the current time to another node, taking
     * into account if the gossiping is to an internal or external client (based on the session
     * tags).  Returns true if the ClusterTime was output into outMessage, or false otherwise.
     */
    bool gossipOut(OperationContext* opCtx,
                   BSONObjBuilder* outMessage,
                   bool forceInternal = false) const;

    /**
     * Read the necessary fields from inMessage in order to update the current time, based on this
     * message received from another node, taking into account if the gossiping is from an internal
     * or external client (based on the session tags).
     */
    void gossipIn(OperationContext* opCtx,
                  const BSONObj& inMessage,
                  bool couldBeUnauthenticated,
                  transport::Session::TagMask defaultClientSessionTags = 0);

    /**
     * Returns true if the clock is enabled and can be used. Defaults to true.
     */
    bool isEnabled() const;

    ///////////////////////////////////////////////////////////////////////////////////////////////
    // The group of methods below is only used for unit-testing
    ///////////////////////////////////////////////////////////////////////////////////////////////

    void advanceClusterTime_forTest(LogicalTime newTime) {
        _advanceTime_forTest(Component::ClusterTime, newTime);
    }

    void advanceConfigTime_forTest(LogicalTime newTime) {
        _advanceTime_forTest(Component::ConfigTime, newTime);
    }

    void advanceTopologyTime_forTest(LogicalTime newTime) {
        _advanceTime_forTest(Component::TopologyTime, newTime);
    }

    void resetVectorClock_forTest();

protected:
    class ComponentFormat {
    public:
        ComponentFormat(std::string fieldName) : _fieldName(std::move(fieldName)) {}
        virtual ~ComponentFormat() = default;

        // Returns true if the time was output, false otherwise.
        virtual bool out(ServiceContext* service,
                         OperationContext* opCtx,
                         bool permitRefresh,
                         BSONObjBuilder* out,
                         LogicalTime time,
                         Component component) const = 0;
        virtual LogicalTime in(ServiceContext* service,
                               OperationContext* opCtx,
                               const BSONObj& in,
                               bool couldBeUnauthenticated,
                               Component component) const = 0;

        const std::string _fieldName;
    };

    VectorClock();
    virtual ~VectorClock();

    /**
     * The maximum permissible value for each part of a LogicalTime's Timestamp (ie. "secs" and
     * "inc").
     */
    static constexpr uint32_t kMaxValue = std::numeric_limits<int32_t>::max();

    /**
     * The "name" of the given component, for user-facing error messages. The name used is the
     * field name used when gossiping.
     */
    static std::string _componentName(Component component);

    /**
     * Disables the clock. A disabled clock won't process logical times and can't be re-enabled.
     */
    void _disable();

    /**
     * "Rate limiter" for advancing logical times. Rejects newTime if any of its Components have a
     * seconds value that's more than gMaxAcceptableLogicalClockDriftSecs ahead of this node's wall
     * clock.
     */
    static void _ensurePassesRateLimiter(ServiceContext* service, const LogicalTimeArray& newTime);

    /**
     * Used to ensure that gossiped or ticked times never overflow the maximum possible LogicalTime.
     */
    static bool _lessThanOrEqualToMaxPossibleTime(LogicalTime time, uint64_t nTicks);

    /**
     * Returns the set of components that need to be gossiped to a node internal to the cluster.
     */
    virtual ComponentSet _gossipOutInternal() const = 0;

    /**
     * As for _gossipOutInternal, except for the components to be sent to a client external to the
     * cluster, eg. a driver or user client. By default, just the ClusterTime is gossiped, although
     * it is disabled in some cases, e.g. when a node is in an unreadable state.
     */
    virtual ComponentSet _gossipOutExternal() const {
        return _permitGossipClusterTimeWithExternalClients() ? ComponentSet{Component::ClusterTime}
                                                             : ComponentSet{};
    }

    /**
     * Returns the set of components that should be processed during gossiping in of messages from
     * internal clients.
     */
    virtual ComponentSet _gossipInInternal() const = 0;

    /**
     * As for _gossipInInternal, except from a client external to the cluster, eg. a driver or user
     * client. By default, just the ClusterTime is gossiped, although it is disabled in some cases,
     * e.g. when a node is in an unreadable state.
     */
    virtual ComponentSet _gossipInExternal() const {
        return _permitGossipClusterTimeWithExternalClients() ? ComponentSet{Component::ClusterTime}
                                                             : ComponentSet{};
    }

    /**
     * Whether or not it's permissable to refresh external state (eg. updating gossip signing keys)
     * during gossip out.
     */
    virtual bool _permitRefreshDuringGossipOut() const = 0;

    /**
     * For each component in the LogicalTimeArray, sets the current time to newTime if the newTime >
     * current time and it passes the rate check.  If any component fails the rate check, then this
     * function uasserts on the first such component (without setting any current times).
     */
    void _advanceTime(LogicalTimeArray&& newTime);

    ///////////////////////////////////////////////////////////////////////////////////////////////
    // The group of methods below is only used for unit-testing
    ///////////////////////////////////////////////////////////////////////////////////////////////

    void _advanceTime_forTest(Component component, LogicalTime newTime);

    // Initialised only once, when the specific vector clock instance gets instantiated on the
    // service context
    ServiceContext* _service{nullptr};

    // Protects the fields below
    //
    // Note that ConfigTime is advanced under the ReplicationCoordinator mutex, so to avoid
    // potential deadlocks the ReplicationCoordator mutex should never be acquired whilst the
    // VectorClock mutex is held.
    mutable Mutex _mutex = MONGO_MAKE_LATCH("VectorClock::_mutex");

    bool _isEnabled{true};

    LogicalTimeArray _vectorTime = {
        kInitialComponentTime, kInitialComponentTime, kInitialComponentTime};

private:
    class PlainComponentFormat;
    class SignedComponentFormat;

    /**
     * Called to determine if the cluster time component should be gossiped in and out to external
     * clients. In some circumstances such gossiping is disabled, e.g. for replica set nodes in
     * unreadable states.
     */
    virtual bool _permitGossipClusterTimeWithExternalClients() const = 0;

    /**
     * Called in order to output a Component time to the passed BSONObjBuilder, using the
     * appropriate field name and representation for that Component.
     *
     * Returns true if the component is ClusterTime and it was output, or false otherwise.
     */
    bool _gossipOutComponent(OperationContext* opCtx,
                             BSONObjBuilder* out,
                             const LogicalTimeArray& time,
                             Component component) const;

    /**
     * Called in order to input a Component time into the given LogicalTimeArray from the given
     * BSONObj, using the appropriate field name and representation for that Component.
     */
    void _gossipInComponent(OperationContext* opCtx,
                            const BSONObj& in,
                            bool couldBeUnauthenticated,
                            LogicalTimeArray* newTime,
                            Component component);

    static const ComponentArray<std::unique_ptr<ComponentFormat>> _gossipFormatters;
};

}  // namespace mongo
