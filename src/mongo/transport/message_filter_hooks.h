// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
[[MONGO_MOD_PUBLIC]] void initMessageFilterPluginLoader(const char* processKind);

}  // namespace mongo::transport
