#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/crt/Exports.h>

#include <aws/io/socket.h>

namespace Aws
{
    namespace Crt
    {
        namespace Io
        {
            enum class SocketType
            {
                /**
                 * A streaming socket sends reliable messages over a two-way connection.
                 * This means TCP when used with IPV4/6, and Unix domain sockets, when used with
                 * AWS_SOCKET_LOCAL
                 */
                Stream = AWS_SOCKET_STREAM,

                /**
                 * A datagram socket is connectionless and sends unreliable messages.
                 * This means UDP when used with IPV4/6.
                 * LOCAL sockets are not compatible with DGRAM.
                 */
                Dgram = AWS_SOCKET_DGRAM,
            };

            enum class SocketDomain
            {
                IPv4 = AWS_SOCKET_IPV4,
                IPv6 = AWS_SOCKET_IPV6,
                /**
                 * Unix domain sockets (or at least something like them)
                 */
                Local = AWS_SOCKET_LOCAL,
            };

            /**
             * Socket configuration options
             */
            class AWS_CRT_CPP_API SocketOptions
            {
              public:
                SocketOptions();
                SocketOptions(const SocketOptions &rhs) = default;
                SocketOptions(SocketOptions &&rhs) = default;

                SocketOptions &operator=(const SocketOptions &rhs) = default;
                SocketOptions &operator=(SocketOptions &&rhs) = default;

                /**
                 * Set socket type
                 * @param type: SocketType object.
                 */
                void SetSocketType(SocketType type) { options.type = (enum aws_socket_type)type; }

                /**
                 * @return the type of socket to use
                 */
                SocketType GetSocketType() const { return (SocketType)options.type; }

                /**
                 * Set socket domain
                 * @param domain: SocketDomain object.
                 */
                void SetSocketDomain(SocketDomain domain) { options.domain = (enum aws_socket_domain)domain; }

                /**
                 * @return the domain type to use with the socket
                 */
                SocketDomain GetSocketDomain() const { return (SocketDomain)options.domain; }

                /**
                 * Set connection timeout
                 * @param timeout: connection timeout in milliseconds.
                 */
                void SetConnectTimeoutMs(uint32_t timeout) { options.connect_timeout_ms = timeout; }

                /**
                 * @return the connection timeout in milliseconds to use with the socket
                 */
                uint32_t GetConnectTimeoutMs() const { return options.connect_timeout_ms; }

                /**
                 * Set keep alive interval seconds.
                 * @param keepAliveInterval: Duration, in seconds, between keepalive probes. If 0, then a default value
                 * is used.
                 */
                void SetKeepAliveIntervalSec(uint16_t keepAliveInterval)
                {
                    options.keep_alive_interval_sec = keepAliveInterval;
                }

                /**
                 * @return the (tcp) keep alive interval to use with the socket, in seconds
                 */
                uint16_t GetKeepAliveIntervalSec() const { return options.keep_alive_interval_sec; }

                /**
                 * Set keep alive time out seconds.
                 * @param keepAliveTimeout: interval, in seconds, that a connection must be idle for before keep alive
                 * probes begin to get sent out
                 */
                void SetKeepAliveTimeoutSec(uint16_t keepAliveTimeout)
                {
                    options.keep_alive_timeout_sec = keepAliveTimeout;
                }

                /**
                 * @return interval, in seconds, that a connection must be idle for before keep alive probes begin
                 * to get sent out
                 */
                uint16_t GetKeepAliveTimeoutSec() const { return options.keep_alive_timeout_sec; }

                /**
                 * Set keep alive max failed probes.
                 * @param maxProbes: The number of keepalive probes allowed to fail before a connection is considered
                 * lost.
                 */
                void SetKeepAliveMaxFailedProbes(uint16_t maxProbes)
                {
                    options.keep_alive_max_failed_probes = maxProbes;
                }

                /**
                 * @return number of keepalive probes allowed to fail before a connection is considered lost.
                 */
                uint16_t GetKeepAliveMaxFailedProbes() const { return options.keep_alive_max_failed_probes; }

                /**
                 * Set keep alive option.
                 * @param keepAlive: True, periodically transmit keepalive messages for detecting a disconnected peer.
                 */
                void SetKeepAlive(bool keepAlive) { options.keepalive = keepAlive; }

                /**
                 * @return true/false if the socket implementation should use TCP keepalive
                 */
                bool GetKeepAlive() const { return options.keepalive; }

                /// @private
                aws_socket_options &GetImpl() { return options; }
                /// @private
                const aws_socket_options &GetImpl() const { return options; }

              private:
                aws_socket_options options;
            };
        } // namespace Io
    } // namespace Crt
} // namespace Aws
