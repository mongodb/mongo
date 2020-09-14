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
#include "mongo/transport/transport_mode.h"

namespace mongo {
namespace transport {

/*
 * The ServiceStateMachine holds the state of a single client connection and represents the
 * lifecycle of each user request as a state machine. It is the glue between the stateless
 * ServiceEntryPoint and TransportLayer that ties network and database logic together for a
 * user.
 */
class ServiceStateMachine {
    ServiceStateMachine(ServiceStateMachine&) = delete;
    ServiceStateMachine& operator=(ServiceStateMachine&) = delete;

    ServiceStateMachine(ServiceStateMachine&&) = delete;
    ServiceStateMachine& operator=(ServiceStateMachine&&) = delete;

public:
    class Impl;

    /*
     * Construct a ServiceStateMachine for a given Client.
     */
    ServiceStateMachine(ServiceContext::UniqueClient client);

    /*
     * start() schedules a call to _runOnce() in the future.
     */
    void start(ServiceExecutorContext seCtx);

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

    /*
     * Sets a function to be called after the session is ended
     */
    void setCleanupHook(std::function<void()> hook);

private:
    std::shared_ptr<Impl> _impl;
};

}  // namespace transport
}  // namespace mongo
