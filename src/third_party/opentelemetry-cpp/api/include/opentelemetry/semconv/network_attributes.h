/*
 * Copyright The OpenTelemetry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * DO NOT EDIT, this is an Auto-generated file from:
 * buildscripts/semantic-convention/templates/registry/semantic_attributes-h.j2
 */

#pragma once

#include "opentelemetry/common/macros.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace semconv
{
namespace network
{

/**
  Local address of the network connection - IP address or Unix domain socket name.
 */
static constexpr const char *kNetworkLocalAddress = "network.local.address";

/**
  Local port number of the network connection.
 */
static constexpr const char *kNetworkLocalPort = "network.local.port";

/**
  Peer address of the network connection - IP address or Unix domain socket name.
 */
static constexpr const char *kNetworkPeerAddress = "network.peer.address";

/**
  Peer port number of the network connection.
 */
static constexpr const char *kNetworkPeerPort = "network.peer.port";

/**
  <a href="https://wikipedia.org/wiki/Application_layer">OSI application layer</a> or non-OSI
  equivalent. <p> The value SHOULD be normalized to lowercase.
 */
static constexpr const char *kNetworkProtocolName = "network.protocol.name";

/**
  The actual version of the protocol used for network communication.
  <p>
  If protocol version is subject to negotiation (for example using <a
  href="https://www.rfc-editor.org/rfc/rfc7301.html">ALPN</a>), this attribute SHOULD be set to the
  negotiated version. If the actual protocol version is not known, this attribute SHOULD NOT be set.
 */
static constexpr const char *kNetworkProtocolVersion = "network.protocol.version";

/**
  <a href="https://wikipedia.org/wiki/Transport_layer">OSI transport layer</a> or <a
  href="https://wikipedia.org/wiki/Inter-process_communication">inter-process communication
  method</a>. <p> The value SHOULD be normalized to lowercase. <p> Consider always setting the
  transport when setting a port number, since a port number is ambiguous without knowing the
  transport. For example different processes could be listening on TCP port 12345 and UDP port
  12345.
 */
static constexpr const char *kNetworkTransport = "network.transport";

/**
  <a href="https://wikipedia.org/wiki/Network_layer">OSI network layer</a> or non-OSI equivalent.
  <p>
  The value SHOULD be normalized to lowercase.
 */
static constexpr const char *kNetworkType = "network.type";

namespace NetworkTransportValues
{
/**
  TCP
 */
static constexpr const char *kTcp = "tcp";

/**
  UDP
 */
static constexpr const char *kUdp = "udp";

/**
  Named or anonymous pipe.
 */
static constexpr const char *kPipe = "pipe";

/**
  Unix domain socket
 */
static constexpr const char *kUnix = "unix";

/**
  QUIC
 */
static constexpr const char *kQuic = "quic";

}  // namespace NetworkTransportValues

namespace NetworkTypeValues
{
/**
  IPv4
 */
static constexpr const char *kIpv4 = "ipv4";

/**
  IPv6
 */
static constexpr const char *kIpv6 = "ipv6";

}  // namespace NetworkTypeValues

}  // namespace network
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
