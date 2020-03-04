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
#include "mongo/client/sdam/topology_listener.h"

namespace mongo::sdam {

void TopologyEventsPublisher::registerListener(TopologyListenerPtr listener) {
    stdx::lock_guard lock(_mutex);
    _listeners.push_back(listener);
}

void TopologyEventsPublisher::removeListener(TopologyListenerPtr listener) {
    stdx::lock_guard lock(_mutex);
    _listeners.erase(std::remove(_listeners.begin(), _listeners.end(), listener), _listeners.end());
}

void TopologyEventsPublisher::close() {
    stdx::lock_guard lock(_mutex);
    _listeners.clear();
    _isClosed = true;
}

void TopologyEventsPublisher::onTopologyDescriptionChangedEvent(
    UUID topologyId,
    TopologyDescriptionPtr previousDescription,
    TopologyDescriptionPtr newDescription) {
    {
        stdx::lock_guard lock(_eventQueueMutex);
        EventPtr event = std::make_unique<Event>();
        event->type = EventType::TOPOLOGY_DESCRIPTION_CHANGED;
        event->topologyId = std::move(topologyId);
        event->previousDescription = previousDescription;
        event->newDescription = newDescription;
        _eventQueue.push_back(std::move(event));
    }
    _scheduleNextDelivery();
}

void TopologyEventsPublisher::onServerHandshakeCompleteEvent(IsMasterRTT durationMs,
                                                             const sdam::ServerAddress& address,
                                                             const BSONObj reply) {
    {
        stdx::lock_guard<Mutex> lock(_eventQueueMutex);
        EventPtr event = std::make_unique<Event>();
        event->type = EventType::HANDSHAKE_COMPLETE;
        event->duration = duration_cast<IsMasterRTT>(durationMs);
        event->hostAndPort = address;
        event->reply = reply;
        _eventQueue.push_back(std::move(event));
    }
    _scheduleNextDelivery();
}

void TopologyEventsPublisher::onServerHandshakeFailedEvent(const sdam::ServerAddress& address,
                                                           const Status& status,
                                                           const BSONObj reply) {
    {
        stdx::lock_guard<Mutex> lock(_eventQueueMutex);
        EventPtr event = std::make_unique<Event>();
        event->type = EventType::HANDSHAKE_FAILURE;
        event->hostAndPort = address;
        event->reply = reply;
        event->status = status;
        _eventQueue.push_back(std::move(event));
    }
    _scheduleNextDelivery();
}

void TopologyEventsPublisher::onServerHeartbeatSucceededEvent(const ServerAddress& hostAndPort,
                                                              const BSONObj reply) {
    {
        stdx::lock_guard lock(_eventQueueMutex);
        EventPtr event = std::make_unique<Event>();
        event->type = EventType::HEARTBEAT_SUCCESS;
        event->hostAndPort = hostAndPort;
        event->reply = reply;
        _eventQueue.push_back(std::move(event));
    }
    _scheduleNextDelivery();
}

void TopologyEventsPublisher::onServerHeartbeatFailureEvent(Status errorStatus,
                                                            const ServerAddress& hostAndPort,
                                                            const BSONObj reply) {
    {
        stdx::lock_guard lock(_eventQueueMutex);
        EventPtr event = std::make_unique<Event>();
        event->type = EventType::HEARTBEAT_FAILURE;
        event->hostAndPort = hostAndPort;
        event->reply = reply;
        event->status = errorStatus;
        _eventQueue.push_back(std::move(event));
    }
    _scheduleNextDelivery();
}

void TopologyEventsPublisher::_scheduleNextDelivery() {
    // run nextDelivery async
    _executor->schedule(
        [self = shared_from_this()](const Status& status) { self->_nextDelivery(); });
}

void TopologyEventsPublisher::onServerPingFailedEvent(const ServerAddress& hostAndPort,
                                                      const Status& status) {
    {
        stdx::lock_guard lock(_eventQueueMutex);
        EventPtr event = std::make_unique<Event>();
        event->type = EventType::PING_FAILURE;
        event->hostAndPort = hostAndPort;
        event->status = status;
        _eventQueue.push_back(std::move(event));
    }
    _scheduleNextDelivery();
}

void TopologyEventsPublisher::onServerPingSucceededEvent(IsMasterRTT durationMS,
                                                         const ServerAddress& hostAndPort) {
    {
        stdx::lock_guard lock(_eventQueueMutex);
        EventPtr event = std::make_unique<Event>();
        event->type = EventType::PING_SUCCESS;
        event->duration = duration_cast<IsMasterRTT>(durationMS);
        event->hostAndPort = hostAndPort;
        _eventQueue.push_back(std::move(event));
    }
    _scheduleNextDelivery();
}

// note that this could be done in batches if it is a bottleneck.
void TopologyEventsPublisher::_nextDelivery() {
    // get the next event to send
    EventPtr nextEvent;
    {
        stdx::lock_guard lock(_eventQueueMutex);
        if (!_eventQueue.size()) {
            return;
        }
        nextEvent = std::move(_eventQueue.front());
        _eventQueue.pop_front();
    }

    // release the lock before sending to avoid deadlock in the case there
    // are events generated by sending the current one.
    std::vector<TopologyListenerPtr> listeners;
    {
        stdx::lock_guard lock(_mutex);
        if (_isClosed) {
            return;
        }
        listeners = _listeners;
    }

    // send to the listeners outside of the lock.
    for (auto listener : listeners) {
        _sendEvent(listener, *nextEvent);
    }
}

void TopologyEventsPublisher::_sendEvent(TopologyListenerPtr listener, const Event& event) {
    switch (event.type) {
        case EventType::HEARTBEAT_SUCCESS:
            listener->onServerHeartbeatSucceededEvent(event.hostAndPort, event.reply);
            break;
        case EventType::HEARTBEAT_FAILURE:
            listener->onServerHeartbeatFailureEvent(event.status, event.hostAndPort, event.reply);
            break;
        case EventType::TOPOLOGY_DESCRIPTION_CHANGED:
            // TODO SERVER-46497: fix uuid or just remove
            listener->onTopologyDescriptionChangedEvent(
                UUID::gen(), event.previousDescription, event.newDescription);
            break;
        case EventType::HANDSHAKE_COMPLETE:
            listener->onServerHandshakeCompleteEvent(
                sdam::IsMasterRTT(duration_cast<Milliseconds>(event.duration)),
                event.hostAndPort,
                event.reply);
            break;
        case EventType::PING_SUCCESS:
            listener->onServerPingSucceededEvent(duration_cast<IsMasterRTT>(event.duration),
                                                 event.hostAndPort);
            break;
        case EventType::PING_FAILURE:
            listener->onServerPingFailedEvent(event.hostAndPort, event.status);
            break;
        case EventType::HANDSHAKE_FAILURE:
            listener->onServerHandshakeFailedEvent(event.hostAndPort, event.status, event.reply);
            break;
        default:
            MONGO_UNREACHABLE;
    }
}
};  // namespace mongo::sdam
