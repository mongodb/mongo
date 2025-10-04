/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <cstdint>
struct sockaddr;

namespace Aws
{
    namespace Net
    {
        // 8K is aligned with default monitoring packet size.
        const static size_t UDP_BUFFER_SIZE = 8192;
        /**
         * SimpleUDP definition.
         */
        class AWS_CORE_API SimpleUDP
        {
        public:
            /**
             * @brief Constructor of SimpleUDP
             * @param addressFamily, AF_INET for IPV4 or AF_INET6 for IPV6
             * @param sendBufSize, if nonzero, try set socket's send buffer size to this value.
             * @param receiveBufSize, if nonzero, try set socket's receive buffer size to this value.
             * @param nonBlocking, if it is true, implementation will try to create a non-blocking underlying UDP socket.
             * Implementation should create and set the underlying udp socket.
             */
            SimpleUDP(int addressFamily, size_t sendBufSize = UDP_BUFFER_SIZE, size_t receiveBufSize = UDP_BUFFER_SIZE, bool nonBlocking = true);

            /**
            * @brief An easy constructor of an IPV4 or IPV6 SimpleUDP
            * @param addressFamily, either AF_INET for IPV4 or AF_INET6 for IPV6
            * @param sendBufSize, if nonzero, try set socket's send buffer size to this value.
            * @param receiveBufSize, if nonzero, try set socket's receive buffer size to this value.
            * @param nonBlocking, if it is true, implementation will try to create a non-blocking underlying UDP socket.
            * Implementation should create and set the underlying udp socket.
            */
            SimpleUDP(bool IPV4 = true, size_t sendBufSize = UDP_BUFFER_SIZE, size_t receiveBufSize = UDP_BUFFER_SIZE, bool nonBlocking = true);

            /**
            * @brief An easy constructor of SimpleUDP based on host and port
            * @param host, the host that packets will be sent to, could be ipv4 or ipv6 address, or a hostname
            * Note that "localhost" is not necessarily bind to 127.0.0.1, it could bind to ipv6 address ::1, or other type of ip addresses. If you pass localhost here, we will go through getaddrinfo procedure on Linux and Windows.
            * @param port, the port number that the host listens on.
            * @param sendBufSize, if nonzero, try set socket's send buffer size to this value.
            * @param receiveBufSize, if nonzero, try set socket's receive buffer size to this value.
            * @param nonBlocking, if it is true, implementation will try to create a non-blocking underlying UDP socket.
            * Implementation should create and set the underlying udp socket.
            */
            SimpleUDP(const char* host, unsigned short port, size_t sendBufSize = UDP_BUFFER_SIZE, size_t receiveBufSize = UDP_BUFFER_SIZE, bool nonBlocking = true);

            ~SimpleUDP();

            /**
             * @brief Connect underlying udp socket to server specified in address.
             * @param address, the server's address info.
             * @param addressLength, length of address, structure of address can vary.
             * @return 0 on success, -1 on error, check errno for detailed error information.
             */
            int Connect(const sockaddr* address, size_t addressLength);

            /**
             * @brief An easy way to connect to host
             * @param hostIP, a valid ipv4 or ipv6 address. The address type should match the m_addressFamily type settled during construction.
             * Otherwise the connection will fail.
             * @param port, the port that host listens on.
             */
            int ConnectToHost(const char* hostIP, unsigned short port) const;

            /**
             * @brief An easy way to connect to 127.0.0.1 or ::1 based on m_addressFamily
             */
            int ConnectToLocalHost(unsigned short port) const;

            /**
             * @brief Bind underlying udp socket to an address.
             * @param address, the server's address info.
             * @param addressLength, length of address, structure of address can vary.
             * @return 0 on success, -1 on error, check errno for detailed error information.
             */
            int Bind(const sockaddr* address, size_t addressLength) const;

            /**
            * @brief An easy way to bind to localhost
            */
            int BindToLocalHost(unsigned short port) const;

            /**
             * @brief Send data to server without specifying address, only usable if hostIP and port are available.
             * @param data, the data you want to send.
             * @param dataLen, the length of data you want to send. On Windows, dataLen larger than INT32_MAX will cause undefined behavior
             * @return 0 on success, -1 on error, check errno for detailed error information.
             */
            int SendData(const uint8_t* data, size_t dataLen) const;

            /**
             * @brief Send data to server.
             * @param address, the server's address info.
             * @param addressLength, length of address, structure of address can vary.
             * @param data, the memory address of the data you want to send.
             * @param dataLen, the length of data you want to send. On Windows, dataLen larger than INT32_MAX will cause undefined behavior
             * @return 0 on success, -1 on error check errno for detailed error information.
             */
            int SendDataTo(const sockaddr* address, size_t addressLength, const uint8_t* data, size_t dataLen) const;

            /**
             * @brief An easy way to send data to localhost, when the underlying udp is connected, call this function will 
             * send the data to where it connects to, not essentially to localhost. when it's not connected, it will send data
             * to localhost, but this call will not connect underlying socket to localhost for you.
             * @param data, the memory address of the data you want to send.
             * @param dataLen, the length of data you want to send. On Windows, dataLen larger than INT32_MAX will cause undefined behavior
             * @param port, port of localhost.
             * @return 0 on success, -1 on error, check errno for detailed error information.
             */
            int SendDataToLocalHost(const uint8_t* data, size_t dataLen, unsigned short port) const;

            /**
             * @brief Receive data from unique address specified in ConnectWithServer call.
             * this function is equivalent to call ReceiveDataFrom(nullptr, 0, data, dataLen, flags).
             * @param buffer, the memory address where you want to store received data.
             * @param bufferLen, the size of data buffer.
             * @return -1 on failure, check errno for detailed error information, on success, returns the actual bytes of data received
             */
            int ReceiveData(uint8_t* buffer, size_t bufferLen) const;

            /**
             * @brief Receive data from network.
             * @param address, if not null and underlying implementation supply the incoming data's source address, this will be filled with source address info.
             * @param addressLength, the size of source address, should not be null.
             * @param buffer, the memory address where you want to store received data.
             * @param bufferLen, the size of data buffer.
             * @return -1 on failure, check errno for detailed error information, on success, returns the actual bytes of data received.
             */
            int ReceiveDataFrom(sockaddr* address, size_t* addressLength, uint8_t* buffer, size_t bufferLen) const;

            /**
             * Gets the AddressFamily used for the underlying socket. E.g. AF_INET, AF_INET6 etc.
             */
            inline int GetAddressFamily() const { return m_addressFamily; }

            /**
             * Is the underlying socket connected with a remote address
             */
            inline bool IsConnected() const { return m_connected; }

        private:
            void CreateSocket(int addressFamily, size_t sendBufSize, size_t receiveBufSize, bool nonBlocking);
            int GetUnderlyingSocket() const { return m_socket; }
            void SetUnderlyingSocket(int socket) { m_socket = socket; } 
            int m_addressFamily;
            // if not connected, you can't perform SendData, if connected,  SendDataTo will call SendData
            mutable bool m_connected;
            int m_socket;
            unsigned short m_port;
            Aws::String m_hostIP;
        };
    }
}
