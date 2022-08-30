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
#include <deque>
#include <memory>
#include <vector>

#include "mongo/client/sdam/sdam_datatypes.h"
#include "mongo/executor/task_executor.h"

namespace mongo::sdam {

/**
 * An interface for handling topology related events.
 */
class TopologyListener {
public:
    virtual ~TopologyListener() {}

    /**
     * Called when a TopologyDescriptionChangedEvent is published - The TopologyDescription changed
     * and the new TopologyDescription does not match the old.
     */
    virtual void onTopologyDescriptionChangedEvent(TopologyDescriptionPtr previousDescription,
                                                   TopologyDescriptionPtr newDescription){};

    virtual void onServerHeartbeatFailureEvent(Status errorStatus,
                                               const HostAndPort& hostAndPort,
                                               const BSONObj reply){};
    /**
     * Called when a ServerHandshakeCompleteEvent is published - The initial handshake to the server
     * at hostAndPort was successful. duration is the measured RTT (Round Trip Time).
     */
    virtual void onServerHandshakeCompleteEvent(HelloRTT duration,
                                                const HostAndPort& address,
                                                const BSONObj reply = BSONObj()){};

    virtual void onServerHandshakeFailedEvent(const HostAndPort& address,
                                              const Status& status,
                                              const BSONObj reply){};

    /**
     * Called when a ServerHeartBeatSucceededEvent is published - A heartbeat sent to the server at
     * hostAndPort succeeded. duration is the execution time of the event, including the time it
     * took to send the message and receive the reply from the server.
     */
    virtual void onServerHeartbeatSucceededEvent(const HostAndPort& hostAndPort,
                                                 const BSONObj reply){};

    /*
     * Called when a ServerPingFailedEvent is published - A monitoring ping to the server at
     * hostAndPort was not successful.
     */
    virtual void onServerPingFailedEvent(const HostAndPort& hostAndPort, const Status& status){};

    /**
     * Called when a ServerPingSucceededEvent is published - A monitoring ping to the server at
     * hostAndPort was successful. duration is the measured RTT (Round Trip Time).
     */
    virtual void onServerPingSucceededEvent(HelloRTT duration, const HostAndPort& hostAndPort){};
};

/**
 * This class publishes TopologyListener events to a group of registered listeners.
 *
 * To publish an event to all registered listeners call the corresponding event function on the
 * TopologyEventsPublisher instance.
 */
class TopologyEventsPublisher : public TopologyListener,
                                public std::enable_shared_from_this<TopologyEventsPublisher> {
public:
    TopologyEventsPublisher(std::shared_ptr<executor::TaskExecutor> executor)
        : _executor(executor){};
    void registerListener(TopologyListenerPtr listener);
    void removeListener(TopologyListenerPtr listener);
    void close();

    void onTopologyDescriptionChangedEvent(TopologyDescriptionPtr previousDescription,
                                           TopologyDescriptionPtr newDescription) override;
    virtual void onServerHandshakeCompleteEvent(HelloRTT duration,
                                                const HostAndPort& address,
                                                BSONObj reply = BSONObj()) override;

    void onServerHandshakeFailedEvent(const HostAndPort& address,
                                      const Status& status,
                                      BSONObj reply);

    void onServerHeartbeatSucceededEvent(const HostAndPort& hostAndPort, BSONObj reply) override;
    void onServerHeartbeatFailureEvent(Status errorStatus,
                                       const HostAndPort& hostAndPort,
                                       BSONObj reply) override;
    void onServerPingFailedEvent(const HostAndPort& hostAndPort, const Status& status) override;
    void onServerPingSucceededEvent(HelloRTT duration, const HostAndPort& hostAndPort) override;

private:
    enum class EventType {
        HEARTBEAT_SUCCESS,
        HEARTBEAT_FAILURE,
        PING_SUCCESS,
        PING_FAILURE,
        TOPOLOGY_DESCRIPTION_CHANGED,
        HANDSHAKE_COMPLETE,
        HANDSHAKE_FAILURE
    };

    struct Event {
        EventType type;
        HostAndPort hostAndPort;
        HelloRTT duration;
        BSONObj reply;
        TopologyDescriptionPtr previousDescription;
        TopologyDescriptionPtr newDescription;
        Status status = Status::OK();
    };
    using EventPtr = std::unique_ptr<Event>;

    void _sendEvent(TopologyListener* listener, const TopologyEventsPublisher::Event& event);
    void _nextDelivery();
    void _scheduleNextDelivery();

    // Lock acquisition order to avoid deadlock is _eventQueueMutex -> _mutex
    Mutex _eventQueueMutex = MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(6),
                                              "TopologyEventsPublisher::_eventQueueMutex");
    std::deque<EventPtr> _eventQueue;

    Mutex _mutex =
        MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(5), "TopologyEventsPublisher::_mutex");
    bool _isClosed = false;
    std::shared_ptr<executor::TaskExecutor> _executor;
    std::vector<TopologyListenerPtr> _listeners;
};

using TopologyEventsPublisherPtr = std::shared_ptr<TopologyEventsPublisher>;
}  // namespace mongo::sdam
