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

#pragma once

#include <vector>

#include "mongo/base/status.h"
#include "mongo/stdx/mutex.h"
#include "mongo/transport/session.h"
#include "mongo/transport/ticket.h"
#include "mongo/transport/ticket_impl.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/net/message.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace transport {

/**
 * This TransportLayerManager is a TransportLayer implementation that holds other
 * TransportLayers. Mongod and Mongos can treat this like the "only" TransportLayer
 * and not be concerned with which other TransportLayer implementations it holds
 * underneath.
 */
class TransportLayerManager final : public TransportLayer {
    MONGO_DISALLOW_COPYING(TransportLayerManager);

public:
    TransportLayerManager();

    Ticket sourceMessage(const Session& session,
                         Message* message,
                         Date_t expiration = Ticket::kNoExpirationDate) override;
    Ticket sinkMessage(const Session& session,
                       const Message& message,
                       Date_t expiration = Ticket::kNoExpirationDate) override;

    Status wait(Ticket&& ticket) override;
    void asyncWait(Ticket&& ticket, TicketCallback callback) override;

    std::string getX509SubjectName(const Session& session) override;
    void registerTags(const Session& session) override;

    Stats sessionStats() override;

    void end(const Session& session) override;
    void endAllSessions(Session::TagMask tags = Session::kEmptyTagMask) override;

    Status start() override;

    void shutdown() override;

    Status addAndStartTransportLayer(std::unique_ptr<TransportLayer> tl);

private:
    template <typename Callable>
    void _foreach(Callable&& cb);

    stdx::mutex _tlsMutex;
    std::vector<std::unique_ptr<TransportLayer>> _tls;
};

}  // namespace transport
}  // namespace mongo
