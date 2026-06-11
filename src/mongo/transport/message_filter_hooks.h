/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/rpc/message.h"
#include "mongo/transport/session.h"
#include "mongo/util/modules.h"

#include <cstddef>

namespace mongo::transport {

/**
 * Provides hooks for detecting and filtering out incoming messages at the transport layer at
 * various early stages of processing.
 */
class MessageHooks {
public:
    /**
     * Called prior to each attempt at parsing a proxy protocol header.
     * Because of the peek-and-retry style of parsing, this may be called multiple times for the
     * same message, as more of the header is received.
     */
    static void onProxyHeaderReceived(Session& session,
                                      const void* msgHeader,
                                      size_t size,
                                      bool isUnixSocket);

    /**
     * Called when a new connection is accepted.
     * For connections on proxy protocol ports, this is called after the proxy protocol header is
     * successfully parsed and the session's remote is populated with the correct client address.
     * For other connections, this is called immediately after accept returns, and prior to any TLS
     * handshake. Either way, this is called with the real remote address known.
     */
    static void onConnectionEstablished(Session& session);

    /**
     * Called after at least the message header has been read.
     * If opportunistic reading is performed, size can be greater than 16.
     */
    static void onHeaderReceived(Session& session, const void* msgHeader, size_t size);

    /**
     * Called after an entire message has been received.
     * For compressed messages this is called once with the compressed envelope
     * and once with the decompressed payload.
     */
    static void onMessageReceived(Session& session, const Message& msg);

    /**
     * Called when a reply is produced, prior to compression or sending.
     * Not called for fire-and-forget requests.
     */
    static void onReplyReady(Session& session, const Message& req, const Message& resp);
};


/**
 * Starts message filter hooks service for a given process kind.
 */
MONGO_MOD_PUBLIC void initMessageFilterPluginLoader(const char* processKind);

}  // namespace mongo::transport
