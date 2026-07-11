// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/message_filter_hooks.h"

namespace mongo::transport {

// This file is all no-op stubs for the community build. The enterprise version of this file
// contains the real implementations and lives in message_filter_loader.cpp, but we need these stubs
// to link the community binaries without pulling in the enterprise code.

void initMessageFilterPluginLoader(const char* /*processKind*/) {}

void MessageHooks::onProxyHeaderReceived(Session& /*session*/,
                                         const void* /*msgHeader*/,
                                         size_t /*size*/,
                                         bool /*isUnixSocket*/) {}

void MessageHooks::onConnectionEstablished(Session& /*session*/) {}

void MessageHooks::onHeaderReceived(Session& /*session*/,
                                    const void* /*msgHeader*/,
                                    size_t /*size*/) {}

void MessageHooks::onMessageReceived(Session& /*session*/, const Message& /*msg*/) {}

void MessageHooks::onReplyReady(Session& /*session*/,
                                const Message& /*req*/,
                                const Message& /*resp*/) {}

}  // namespace mongo::transport
