/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/transport/asio/asio_session.h"
#include "mongo/transport/asio/asio_transport_layer.h"

#include <system_error>

/**
 * Support for the TCP Fast Open (TFO) protocol.
 * These functions do nothing on platforms without TFO.
 */
namespace mongo::transport::tfo {

/**
 * Apply any necessary TFO options onto the specified `sock`.
 *
 * The `sock` must be a stream socket opened in the `AF_INET` and `AF_INET6`
 * protocol family (aka TCP).
 *
 * Setup errors are ignored in passive mode (see `ensureInitialized`).
 */
std::error_code initOutgoingSocket(AsioSession::GenericSocket& sock);

/**
 * Similar to `initOutgoingSocket`, but for an acceptor socket.
 */
std::error_code initAcceptorSocket(AsioTransportLayer::GenericAcceptor& acceptor);

/**
 * Check that we can use TFO.
 *
 * This performs some compile time and runtime probes for TFO support,
 * considering relevant server parameters.
 *
 * Internally, this function calculates and records whether TFO will be
 * operating in "passive mode". In "passive mode", the function always
 * returns the OK status, but it will still log warning messages.
 * Otherwise (i.e. non-passive mode), this function returns any errors
 * encountered during probing and initialization.
 *
 * Memoizes its results so calls after the first are cheap noops.
 *
 * ## Passive Mode ##
 * "Passive mode" means that the user has neither set nor cleared any
 * of the server parameters that are relevant to TFO. The
 * TFO parameters will remain at their defaults.
 *
 * This means that TCPFastOpenClient==true and TCPFastOpenServer==true,
 * and TFO will be attempted for client and server sockets, but any
 * failures to perform this enabling operation will be ignored.
 */
Status ensureInitialized(bool returnUnfilteredError = false);

/**
 * Test-only. Temporarily alters the behavior of static state of the TFO
 * system.  Upon destruction of the returned object, the TFO system returns
 * to normal operation. Returns `std::shared_ptr<void>` because the caller
 * only manages the lifetime of this object, and doesn't need its value.
 */
std::shared_ptr<void> setConfigForTest(
    bool passive, bool client, bool server, int queueSize, Status initError);

}  // namespace mongo::transport::tfo
