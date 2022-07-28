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

#include <functional>
#include <memory>

#include "mongo/db/service_context.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/session.h"

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
     * Terminates the associated transport Session if its tags don't match the supplied tags.
     * If the session is in a pending state, before any tags have been set, it will not be
     * terminated.
     *
     * This will not block on the session terminating cleaning itself up, it returns immediately.
     */
    void terminateIfTagsDontMatch(transport::Session::TagMask tags);

private:
    std::unique_ptr<Impl> _impl;
};

}  // namespace transport
}  // namespace mongo
