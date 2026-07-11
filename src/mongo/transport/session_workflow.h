// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/session.h"

#include <functional>
#include <memory>
#include <utility>

namespace mongo {
namespace transport {

/*
 * The SessionWorkflow manages the work of a single session and represents the
 * lifecycle of each user request. It is the glue between ServiceEntryPoint and TransportLayer
 * that ties network and database logic together for a user.
 *
 * A `SessionWorkflow` must be managed by a `shared_ptr`, so we force all instances
 * to be created by the static `make` function.
 */
class SessionWorkflow : public std::enable_shared_from_this<SessionWorkflow> {
    struct PassKeyTag {
        explicit PassKeyTag() = default;
    };
    class Impl;
    SessionWorkflow(SessionWorkflow&) = delete;
    SessionWorkflow& operator=(SessionWorkflow&) = delete;

    SessionWorkflow(SessionWorkflow&&) = delete;
    SessionWorkflow& operator=(SessionWorkflow&&) = delete;

public:
    /** Factory function: The only public way to create instances. */
    static std::shared_ptr<SessionWorkflow> make(ServiceContext::UniqueClient client) {
        return std::make_shared<SessionWorkflow>(PassKeyTag{}, std::move(client));
    }

    /** Public must use `make` to create instances. */
    SessionWorkflow(PassKeyTag, ServiceContext::UniqueClient client);

    ~SessionWorkflow();

    /** Returns the Client given in the constructor. */
    Client* client() const;

    void start();

    /*
     * Terminates the associated transport Session, regardless of tags.
     *
     * This will not block on the session terminating cleaning itself up, it returns immediately.
     */
    void terminate();

    /*
     * Terminates the associated transport Session if the connection tags in the client don't match
     * the supplied tags.  If the connection tags indicate a pending state, before any tags have
     * been set, it will not be terminated.
     *
     * This will not block on the session terminating cleaning itself up, it returns immediately.
     */
    void terminateIfTagsDontMatch(Client::TagMask tags);

private:
    std::unique_ptr<Impl> _impl;
};

}  // namespace transport
}  // namespace mongo
