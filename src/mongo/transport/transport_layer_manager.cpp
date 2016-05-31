/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/transport/transport_layer_manager.h"

#include "mongo/base/status.h"
#include "mongo/stdx/memory.h"
#include "mongo/transport/session.h"
#include "mongo/util/time_support.h"
#include <limits>

#include <iostream>

namespace mongo {
namespace transport {

TransportLayerManager::TransportLayerManager() = default;

Ticket TransportLayerManager::sourceMessage(const Session& session,
                                            Message* message,
                                            Date_t expiration) {
    return session.getTransportLayer()->sourceMessage(session, message, expiration);
}

Ticket TransportLayerManager::sinkMessage(const Session& session,
                                          const Message& message,
                                          Date_t expiration) {
    return session.getTransportLayer()->sinkMessage(session, message, expiration);
}

Status TransportLayerManager::wait(Ticket&& ticket) {
    return getTicketTransportLayer(ticket)->wait(std::move(ticket));
}

void TransportLayerManager::asyncWait(Ticket&& ticket, TicketCallback callback) {
    return getTicketTransportLayer(ticket)->asyncWait(std::move(ticket), std::move(callback));
}

std::string TransportLayerManager::getX509SubjectName(const Session& session) {
    return session.getX509SubjectName();
}

template <typename Callable>
void TransportLayerManager::_foreach(Callable&& cb) {
    {
        stdx::lock_guard<stdx::mutex> lk(_tlsMutex);
        for (auto&& tl : _tls) {
            cb(tl.get());
        }
    }
}

TransportLayer::Stats TransportLayerManager::sessionStats() {
    Stats stats;

    _foreach([&](TransportLayer* tl) {
        Stats s = tl->sessionStats();

        stats.numOpenSessions += s.numOpenSessions;
        stats.numCreatedSessions += s.numCreatedSessions;
        if (std::numeric_limits<size_t>::max() - stats.numAvailableSessions <
            s.numAvailableSessions) {
            stats.numAvailableSessions = std::numeric_limits<size_t>::max();
        } else {
            stats.numAvailableSessions += s.numAvailableSessions;
        }
    });

    return stats;
}

void TransportLayerManager::registerTags(const Session& session) {
    session.getTransportLayer()->registerTags(session);
}

void TransportLayerManager::end(const Session& session) {
    session.getTransportLayer()->end(session);
}

void TransportLayerManager::endAllSessions(Session::TagMask tags) {
    _foreach([&tags](TransportLayer* tl) { tl->endAllSessions(tags); });
}

Status TransportLayerManager::start() {
    return Status::OK();
}

void TransportLayerManager::shutdown() {
    _foreach([](TransportLayer* tl) { tl->shutdown(); });
}

Status TransportLayerManager::addAndStartTransportLayer(std::unique_ptr<TransportLayer> tl) {
    auto ptr = tl.get();
    {
        stdx::lock_guard<stdx::mutex> lk(_tlsMutex);
        _tls.emplace_back(std::move(tl));
    }
    return ptr->start();
}

}  // namespace transport
}  // namespace mongo
