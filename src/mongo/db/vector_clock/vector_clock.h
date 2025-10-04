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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/vector_clock/vector_clock_document_gen.h"
#include "mongo/db/vector_clock/vector_clock_gen.h"
#include "mongo/stdx/mutex.h"
#include "mongo/transport/session.h"
#include "mongo/util/assert_util.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <utility>

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

    /**
     * Bare-bones std::set like class to hold a set of Components without memory allocation.
     */
    class ComponentSet {
    public:
        ComponentSet() = default;
        ComponentSet(Component c) {
            insert(c);
        }

        void insert(Component c) {
            _components.set(static_cast<std::size_t>(c));
        }

        /**
         * Iterator that implements LegacyForwardIterator.
         */
        class iterator {
            friend class ComponentSet;

        public:
            Component operator*() const {
                return static_cast<Component>(_pos);
            }

            iterator operator++() {
                // Advance one if not at the end and then find a true bit if the current bit is not
                // true.
                if (_pos < _set->_components.size()) {
                    _pos++;
                    advanceIfNeeded();
                }
                return *this;
            }

            bool operator==(const iterator& right) const {
                return this->_pos == right._pos;
            }

            bool operator!=(const iterator& right) const {
                return this->_pos != right._pos;
            }

        private:
            iterator(const ComponentSet* set) : _set(set), _pos(0) {
                advanceIfNeeded();
            }

            iterator(const ComponentSet* set, std::size_t pos) : _set(set), _pos(pos) {}

            // advance if the current bit is not true until the next valid bit or reach the end of
            // the bit set
            void advanceIfNeeded() {
                for (; _pos < _set->_components.size(); ++_pos) {
                    if (_set->_components.test(_pos)) {
                        break;
                    }
                }
            }

        private:
            const ComponentSet* _set;
            std::size_t _pos;
        };

        iterator begin() const {
            return iterator(this);
        }

        iterator end() const {
            return iterator(this, _components.size());
        }

    private:
        std::bitset<static_cast<std::size_t>(Component::_kNumComponents)> _components;
    };

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

    VectorClock() = default;
    virtual ~VectorClock() = default;

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
     * or external client.
     */
    void gossipIn(OperationContext* opCtx,
                  const GossipedVectorClockComponents& timepoints,
                  bool couldBeUnauthenticated,
                  bool defaultIsInternalClient = false);

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
                         BSONObjBuilder* out,
                         LogicalTime time,
                         Component component,
                         bool isInternal) const = 0;
        virtual LogicalTime in(ServiceContext* service,
                               OperationContext* opCtx,
                               const GossipedVectorClockComponents& timepoints,
                               bool couldBeUnauthenticated,
                               Component component) const = 0;

        const std::string _fieldName;
    };

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
    virtual ComponentSet _getGossipInternalComponents() const {
        VectorClock::ComponentSet toGossip{Component::ClusterTime};
        if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer) ||
            serverGlobalParams.clusterRole.has(ClusterRole::RouterServer)) {
            toGossip.insert(Component::ConfigTime);
            toGossip.insert(Component::TopologyTime);
        }
        return toGossip;
    }

    /**
     * Returns the set of components that need to be gossiped to the external clients, eg. a driver
     * or user client. By default, just the ClusterTime is gossiped, although it is disabled in some
     * cases, e.g. when a node is in an unreadable state.
     */
    virtual ComponentSet _getGossipExternalComponents() const {
        return _permitGossipClusterTimeWithExternalClients() ? ComponentSet{Component::ClusterTime}
                                                             : ComponentSet{};
    }

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
    mutable stdx::mutex _mutex;

    AtomicWord<bool> _isEnabled{true};

    LogicalTimeArray _vectorTime = {
        kInitialComponentTime, kInitialComponentTime, kInitialComponentTime};

private:
    class PlainComponentFormat;
    class SignedComponentFormat;
    class ConfigTimeComponent;
    class TopologyTimeComponent;
    class ClusterTimeComponent;

    /**
     * Called to determine if the cluster time component should be gossiped in and out to external
     * clients. In some circumstances such gossiping is disabled, e.g. for replica set nodes in
     * unreadable states.
     */
    bool _permitGossipClusterTimeWithExternalClients() const;

    /**
     * Called in order to output a Component time to the passed BSONObjBuilder, using the
     * appropriate field name and representation for that Component.
     *
     * Returns true if the component is ClusterTime and it was output, or false otherwise.
     */
    bool _gossipOutComponent(OperationContext* opCtx,
                             BSONObjBuilder* out,
                             const LogicalTimeArray& time,
                             Component component,
                             bool isInternal) const;

    /**
     * Called in order to input a Component time into the given LogicalTimeArray from the given
     * BSONObj, using the appropriate field name and representation for that Component.
     */
    void _gossipInComponent(OperationContext* opCtx,
                            const GossipedVectorClockComponents& timepoints,
                            bool couldBeUnauthenticated,
                            LogicalTimeArray* newTime,
                            Component component);

    static const ComponentArray<std::unique_ptr<ComponentFormat>> _gossipFormatters;
};

}  // namespace mongo
