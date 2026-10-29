// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/transport/asio/asio_session.h"
#include "mongo/transport/asio/asio_transport_layer.h"
#include "mongo/util/modules.h"

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
[[MONGO_MOD_NEEDS_REPLACEMENT]] std::shared_ptr<void> setConfigForTest(
    bool passive, bool client, bool server, int queueSize, Status initError);

}  // namespace mongo::transport::tfo
