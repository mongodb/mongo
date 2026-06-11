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
