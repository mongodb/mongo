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

#include <memory>

#include "mongo/base/disallow_copying.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/transport/session_id.h"
#include "mongo/transport/ticket.h"
#include "mongo/util/decorable.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/message.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace transport {

class TransportLayer;
class Session;

using SessionHandle = std::shared_ptr<Session>;
using ConstSessionHandle = std::shared_ptr<const Session>;

/**
 * This type contains data needed to associate Messages with connections
 * (on the transport side) and Messages with Client objects (on the database side).
 */
class Session : public std::enable_shared_from_this<Session>, public Decorable<Session> {
    MONGO_DISALLOW_COPYING(Session);

public:
    /**
     * Type to indicate the internal id for this session.
     */
    using Id = SessionId;

    /**
     * Tags for groups of connections.
     */
    using TagMask = uint32_t;

    static const Status ClosedStatus;

    static constexpr TagMask kEmptyTagMask = 0;
    static constexpr TagMask kKeepOpen = 1;
    static constexpr TagMask kInternalClient = 2;
    static constexpr TagMask kLatestVersionInternalClientKeepOpen = 4;
    static constexpr TagMask kExternalClientKeepOpen = 8;
    static constexpr TagMask kPending = 1 << 31;

    /**
     * Destroys a session, calling end() for this session in its TransportLayer.
     */
    virtual ~Session() = default;

    /**
     * Return the id for this session.
     */
    Id id() const {
        return _id;
    }

    /**
     * The TransportLayer for this Session.
     */
    virtual TransportLayer* getTransportLayer() const = 0;

    /**
     * Source (receive) a new Message for this Session.
     *
     * This method will forward to sourceMessage on this Session's transport layer.
     */
    virtual Ticket sourceMessage(Message* message, Date_t expiration = Ticket::kNoExpirationDate);

    /**
     * Sink (send) a new Message for this Session. This method should be used
     * to send replies to a given host.
     *
     * This method will forward to sinkMessage on this Session's transport layer.
     */
    virtual Ticket sinkMessage(const Message& message,
                               Date_t expiration = Ticket::kNoExpirationDate);

    /**
     * Return the remote host for this session.
     */
    virtual const HostAndPort& remote() const = 0;

    /**
     * Return the local host information for this session.
     */
    virtual const HostAndPort& local() const = 0;

    /**
     * Atomically set all of the session tags specified in the 'tagsToSet' bit field. If the
     * 'kPending' tag is set, indicating that no tags have yet been specified for the session, this
     * function also clears that tag as part of the same atomic operation.
     *
     * The 'kPending' tag is only for new sessions; callers should not set it directly.
     */
    virtual void setTags(TagMask tagsToSet);

    /**
     * Atomically clears all of the session tags specified in the 'tagsToUnset' bit field. If the
     * 'kPending' tag is set, indicating that no tags have yet been specified for the session, this
     * function also clears that tag as part of the same atomic operation.
     */
    virtual void unsetTags(TagMask tagsToUnset);

    /**
     * Loads the session tags, passes them to 'mutateFunc' and then stores the result of that call
     * as the new session tags, all in one atomic operation.
     *
     * In order to ensure atomicity, 'mutateFunc' may get called multiple times, so it should not
     * perform expensive computations or operations with side effects.
     *
     * If the 'kPending' tag is set originally, mutateTags() will unset it regardless of the result
     * of the 'mutateFunc' call. The 'kPending' tag is only for new sessions; callers should never
     * try to set it.
     */
    virtual void mutateTags(const stdx::function<TagMask(TagMask)>& mutateFunc);

    /**
     * Get this session's tags.
     */
    virtual TagMask getTags() const;

protected:
    /**
     * Construct a new session.
     */
    Session();

    /**
     * Construct a new session with an ID
     *
     * This is used by TransportLayerLegacy to make sure session ID's match the connection ID of
     * their MessagingPort.
     *
     * TransportLayers must ensure that session Id's remain unique across all TransportLayers.
     */
    explicit Session(Id id);

private:
    const Id _id;

    AtomicWord<TagMask> _tags;
};

}  // namespace transport
}  // namespace mongo
