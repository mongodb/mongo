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
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include "mongo/client/sdam/topology_listener.h"
#include "mongo/logv2/log.h"

namespace mongo::sdam {

void TopologyEventsPublisher::registerListener(TopologyListenerPtr listener) {
    auto locked_listener = listener.lock();
    if (!locked_listener) {
        LOGV2_WARNING(5148001, "Trying to register an empty listener with TopologyEventsPublisher");
        return;
    }
    stdx::lock_guard lock(_mutex);
    if (std::find_if(_listeners.begin(),
                     _listeners.end(),
                     [&locked_listener](const TopologyListenerPtr& ptr) {
                         return ptr.lock() == locked_listener;
                     }) == std::end(_listeners)) {
        _listeners.push_back(listener);
    }
}

void TopologyEventsPublisher::removeListener(TopologyListenerPtr listener) {
    auto locked_listener = listener.lock();
    if (!locked_listener) {
        LOGV2_WARNING(5148002,
                      "Trying to unregister an empty listener from TopologyEventsPublisher");
        return;
    }
    stdx::lock_guard lock(_mutex);
    _listeners.erase(std::remove_if(_listeners.begin(),
                                    _listeners.end(),
                                    [&locked_listener](const TopologyListenerPtr& ptr) {
                                        return ptr.lock() == locked_listener;
                                    }),
                     _listeners.end());
}

void TopologyEventsPublisher::close() {
    stdx::lock_guard lock(_mutex);
    _listeners.clear();
    _isClosed = true;
}

void TopologyEventsPublisher::onTopologyDescriptionChangedEvent(
    TopologyDescriptionPtr previousDescription, TopologyDescriptionPtr newDescription) {
    {
        stdx::lock_guard lock(_eventQueueMutex);
        EventPtr event = std::make_unique<Event>();
        event->type = EventType::TOPOLOGY_DESCRIPTION_CHANGED;
        event->previousDescription = previousDescription;
        event->newDescription = newDescription;
        _eventQueue.push_back(std::move(event));
    }
    _scheduleNextDelivery();
}

void TopologyEventsPublisher::onServerHandshakeCompleteEvent(HelloRTT duration,
                                                             const HostAndPort& address,
                                                             const BSONObj reply) {
    {
        stdx::lock_guard<Mutex> lock(_eventQueueMutex);
        EventPtr event = std::make_unique<Event>();
        event->type = EventType::HANDSHAKE_COMPLETE;
        event->duration = duration;
        event->hostAndPort = address;
        event->reply = reply;
        _eventQueue.push_back(std::move(event));
    }
    _scheduleNextDelivery();
}

void TopologyEventsPublisher::onServerHandshakeFailedEvent(const HostAndPort& address,
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

void TopologyEventsPublisher::onServerHeartbeatSucceededEvent(const HostAndPort& hostAndPort,
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
                                                            const HostAndPort& hostAndPort,
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

void TopologyEventsPublisher::onServerPingFailedEvent(const HostAndPort& hostAndPort,
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

void TopologyEventsPublisher::onServerPingSucceededEvent(HelloRTT duration,
                                                         const HostAndPort& hostAndPort) {
    {
        stdx::lock_guard lock(_eventQueueMutex);
        EventPtr event = std::make_unique<Event>();
        event->type = EventType::PING_SUCCESS;
        event->duration = duration;
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
    std::vector<std::shared_ptr<TopologyListener>> listeners;
    {
        stdx::lock_guard lock(_mutex);
        if (_isClosed) {
            return;
        }
        listeners.reserve(_listeners.size());
        // Helps to purge empty elements when a weak_ptr points to an element removed elsewhere.
        // We take advantage of the fact that we are scanning the container anyway.
        _listeners.erase(std::remove_if(_listeners.begin(),
                                        _listeners.end(),
                                        [this, &listeners](const TopologyListenerPtr& ptr) {
                                            auto p = ptr.lock();
                                            if (p) {
                                                // Makes a copy of non-empty elements in
                                                // 'listeners'.
                                                listeners.push_back(p);
                                                return false;
                                            }
                                            return true;
                                        }),
                         _listeners.end());
    }

    // send to the listeners outside of the lock.
    for (auto listener : listeners) {
        // The copy logic above guaranteed that only non-empty elements are in the vector.
        _sendEvent(listener.get(), *nextEvent);
    }
}

void TopologyEventsPublisher::_sendEvent(TopologyListener* listener, const Event& event) {
    switch (event.type) {
        case EventType::HEARTBEAT_SUCCESS:
            listener->onServerHeartbeatSucceededEvent(event.hostAndPort, event.reply);
            break;
        case EventType::HEARTBEAT_FAILURE:
            listener->onServerHeartbeatFailureEvent(event.status, event.hostAndPort, event.reply);
            break;
        case EventType::TOPOLOGY_DESCRIPTION_CHANGED:
            listener->onTopologyDescriptionChangedEvent(event.previousDescription,
                                                        event.newDescription);
            break;
        case EventType::HANDSHAKE_COMPLETE:
            listener->onServerHandshakeCompleteEvent(
                event.duration, event.hostAndPort, event.reply);
            break;
        case EventType::PING_SUCCESS:
            listener->onServerPingSucceededEvent(event.duration, event.hostAndPort);
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
