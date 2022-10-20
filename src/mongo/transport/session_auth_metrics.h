/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <boost/optional.hpp>

#include "mongo/db/commands.h"
#include "mongo/transport/session.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/time_support.h"

namespace mongo::transport {

/**
 * A decoration on the Session object used to capture and report the metrics around authentication
 * conversation for an ingress session, such as the time it takes to finish the auth conversation
 * and the time until the first non-auth command.
 */
class SessionAuthMetrics {
public:
    static SessionAuthMetrics& get(Session& session);

    /**
     * Marks the time when the session was started. The clock source must remain valid for at least
     * as long as the last onCommand call on this instance.
     */
    void onSessionStarted(ClockSource* clockSource);

    /**
     * Checks if the command is part of the initial authentication conversation. If this is the
     * first non-auth command received on this session, it reports the associated metrics.
     * Note: for the purposes of these metrics, hello commands are also considered to be part of the
     * initial authentication conversation.
     */
    void onCommand(const Command* command);

private:
    ClockSource* _clockSource{nullptr};
    boost::optional<Date_t> _sessionStartedTime;
    boost::optional<Date_t> _authHandshakeFinishedTime;
    boost::optional<Date_t> _firstNonAuthCommandTime;
};

/**
 * An implementation of CommandInvocationHooks that ensures SessionAuthMetrics::onCommand will be
 * called for each command.
 */
class SessionAuthMetricsCommandHooks : public CommandInvocationHooks {
public:
    void onBeforeRun(OperationContext* opCtx,
                     const OpMsgRequest& request,
                     CommandInvocation* invocation) override;

    void onAfterRun(OperationContext* opCtx,
                    const OpMsgRequest& request,
                    CommandInvocation* invocation) override {}
};

}  // namespace mongo::transport
