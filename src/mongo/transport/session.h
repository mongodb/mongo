/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <memory>

#include "mongo/db/baton.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/message.h"
#include "mongo/transport/session_id.h"
#include "mongo/util/decorable.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
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
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

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

    virtual ~Session() = default;

    Id id() const {
        return _id;
    }

    virtual TransportLayer* getTransportLayer() const = 0;

    /**
     * Ends this Session.
     *
     * Operations on this Session that have already been started via wait() or asyncWait() will
     * complete, but may return a failed Status.  Future operations on this Session will fail. If
     * this TransportLayer implementation is networked, any connections for this Session will be
     * closed.
     *
     * This method is idempotent and synchronous.
     *
     * Destructors of derived classes will close the session automatically if needed. This method
     * should only be called explicitly if the session should be closed separately from destruction,
     * eg due to some outside event.
     */
    virtual void end() = 0;

    /**
     * Source (receive) a new Message from the remote host for this Session.
     */
    virtual StatusWith<Message> sourceMessage() = 0;
    virtual Future<Message> asyncSourceMessage(const BatonHandle& handle = nullptr) = 0;

    /**
     * Sink (send) a Message to the remote host for this Session.
     *
     * Async version will keep the buffer alive until the operation completes.
     */
    virtual Status sinkMessage(Message message) = 0;
    virtual Future<void> asyncSinkMessage(Message message, const BatonHandle& handle = nullptr) = 0;

    /**
     * Cancel any outstanding async operations. There is no way to cancel synchronous calls.
     * Futures will finish with an ErrorCodes::CallbackCancelled error if they haven't already
     * completed.
     */
    virtual void cancelAsyncOperations(const BatonHandle& handle = nullptr) = 0;

    /**
    * This should only be used to detect when the remote host has disappeared without
    * notice. It does NOT work correctly for ensuring that operations complete or fail
    * by some deadline.
    *
    * This timeout will only effect calls sourceMessage()/sinkMessage(). Async operations do not
    * currently support timeouts.
    */
    virtual void setTimeout(boost::optional<Milliseconds> timeout) = 0;

    /**
     * This will return whether calling sourceMessage()/sinkMessage() will fail with an EOF error.
     *
     * Implementations may actually perform some I/O or call syscalls to determine this, rather
     * than just checking a flag.
     *
     * This must not be called while the session is currently sourcing or sinking a message.
     */
    virtual bool isConnected() = 0;

    virtual const HostAndPort& remote() const = 0;
    virtual const HostAndPort& local() const = 0;

    /**
     * Atomically set all of the session tags specified in the 'tagsToSet' bit field. If the
     * 'kPending' tag is set, indicating that no tags have yet been specified for the session, this
     * function also clears that tag as part of the same atomic operation.
     *
     * The 'kPending' tag is only for new sessions; callers should not set it directly.
     */
    void setTags(TagMask tagsToSet);

    /**
     * Atomically clears all of the session tags specified in the 'tagsToUnset' bit field. If the
     * 'kPending' tag is set, indicating that no tags have yet been specified for the session, this
     * function also clears that tag as part of the same atomic operation.
     */
    void unsetTags(TagMask tagsToUnset);

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
    void mutateTags(const stdx::function<TagMask(TagMask)>& mutateFunc);

    TagMask getTags() const;

protected:
    Session();

private:
    const Id _id;

    AtomicWord<TagMask> _tags;
};

}  // namespace transport
}  // namespace mongo
