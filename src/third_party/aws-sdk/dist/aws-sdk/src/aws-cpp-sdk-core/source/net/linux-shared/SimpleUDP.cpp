/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <cassert>
#include <string.h>
#include <aws/core/net/SimpleUDP.h>
#include <aws/core/utils/logging/LogMacros.h>

namespace Aws
{
    namespace Net
    {
        static const char ALLOC_TAG[] = "SimpleUDP";
        static const char IPV4_LOOP_BACK_ADDRESS[] = "127.0.0.1";
        static const char IPV6_LOOP_BACK_ADDRESS[] = "::1";

        static inline bool IsValidIPAddress(const char* ip, int addressFamily/*AF_INET or AF_INET6*/)
        {
            char buffer[128];
            return inet_pton(addressFamily, ip, (void*)buffer) == 1 ?true :false;
        }

        static bool GetASockAddrFromHostName(const char* hostName, void* sockAddrBuffer, size_t& addrLength, int& addressFamily)
        {
            struct addrinfo hints, *res;
            
            memset(&hints, 0, sizeof(hints));

            hints.ai_family = PF_UNSPEC;
            hints.ai_socktype = SOCK_DGRAM;
            if (getaddrinfo(hostName, nullptr, &hints, &res))
            {
                return false;
            }

            memcpy(sockAddrBuffer, res->ai_addr, res->ai_addrlen);
            addrLength = res->ai_addrlen;
            addressFamily = res->ai_family;
            freeaddrinfo(res);
            return true;
        }

        static sockaddr_in BuildAddrInfoIPV4(const char* hostIP, short port)
        {
#if (__GNUC__ == 4) && !defined(__clang__)
    AWS_SUPPRESS_WARNING("-Wmissing-field-initializers",
            sockaddr_in addrinfo {};
    );
#else
            sockaddr_in addrinfo {};
#endif
            addrinfo.sin_family = AF_INET;
            addrinfo.sin_port = htons(port);
            inet_pton(AF_INET, hostIP, &addrinfo.sin_addr);
            return addrinfo;
        }

        static sockaddr_in6 BuildAddrInfoIPV6(const char* hostIP, short port)
        {
#if (__GNUC__ == 4) && !defined(__clang__)
    AWS_SUPPRESS_WARNING("-Wmissing-field-initializers",
            sockaddr_in6 addrinfo {};
    );
#else
            sockaddr_in6 addrinfo {};
#endif
            addrinfo.sin6_family = AF_INET6;
            addrinfo.sin6_port = htons(port);
            inet_pton(AF_INET6, hostIP, &addrinfo.sin6_addr);
            return addrinfo;
        }

        SimpleUDP::SimpleUDP(int addressFamily, size_t sendBufSize, size_t receiveBufSize, bool nonBlocking):
            m_addressFamily(addressFamily), m_connected(false), m_socket(-1), m_port(0)
        {
            CreateSocket(addressFamily, sendBufSize, receiveBufSize, nonBlocking);
        }

        SimpleUDP::SimpleUDP(bool IPV4, size_t sendBufSize, size_t receiveBufSize, bool nonBlocking) :
            m_addressFamily(IPV4 ? AF_INET : AF_INET6), m_connected(false), m_socket(-1), m_port(0)
        {
            CreateSocket(m_addressFamily, sendBufSize, receiveBufSize, nonBlocking);
        }

        SimpleUDP::SimpleUDP(const char* host, unsigned short port, size_t sendBufSize, size_t receiveBufSize, bool nonBlocking) :
            m_addressFamily(AF_INET), m_connected(false), m_socket(-1), m_port(port)
        {
            if (IsValidIPAddress(host, AF_INET))
            {
                m_addressFamily = AF_INET;
                m_hostIP = Aws::String(host);
            }
            else if (IsValidIPAddress(host, AF_INET6))
            {
                m_addressFamily = AF_INET6;
                m_hostIP = Aws::String(host);
            }
            else 
            {
                char sockAddrBuffer[100];
                char hostBuffer[100];
                size_t addrLength = 0;
                if (GetASockAddrFromHostName(host, (void*)sockAddrBuffer, addrLength, m_addressFamily))
                {
                    if (m_addressFamily == AF_INET)
                    {
                        struct sockaddr_in* sockaddr = reinterpret_cast<struct sockaddr_in*>(sockAddrBuffer);
                        inet_ntop(m_addressFamily, &(sockaddr->sin_addr), hostBuffer, sizeof(hostBuffer));
                    }
                    else
                    {
                        struct sockaddr_in6* sockaddr = reinterpret_cast<struct sockaddr_in6*>(sockAddrBuffer);
                        inet_ntop(m_addressFamily, &(sockaddr->sin6_addr), hostBuffer, sizeof(hostBuffer));
                    }
                    m_hostIP = Aws::String(hostBuffer);
                }
                else
                {
                    AWS_LOGSTREAM_ERROR(ALLOC_TAG, "Can't retrieve a valid ip address based on provided host: " << host);
                }
            }
            CreateSocket(m_addressFamily, sendBufSize, receiveBufSize, nonBlocking);
        }

        SimpleUDP::~SimpleUDP()
        {
            close(GetUnderlyingSocket());
        }

        void SimpleUDP::CreateSocket(int addressFamily, size_t sendBufSize, size_t receiveBufSize, bool nonBlocking)
        {
            int sock = socket(addressFamily, SOCK_DGRAM, IPPROTO_UDP);
            assert(sock != -1);

            // Try to set sock to nonblocking mode.
            if (nonBlocking)
            {
                int flags = fcntl(sock, F_GETFL, 0);
                if (flags != -1)
                {
                    flags |= O_NONBLOCK;
                    fcntl(sock, F_SETFL, flags);
                }
            }

            // if sendBufSize is not zero, try to set send buffer size
            if (sendBufSize)
            {
                int ret = setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sendBufSize, sizeof(sendBufSize));
                if (ret)
                {
                    AWS_LOGSTREAM_WARN(ALLOC_TAG, "Failed to set UDP send buffer size to " << sendBufSize << " for socket " << sock << " error message: " << strerror(errno));
                }
                assert(ret == 0);
            }

            // if receiveBufSize is not zero, try to set receive buffer size
            if (receiveBufSize)
            {
                int ret = setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &receiveBufSize, sizeof(receiveBufSize));
                if (ret)
                {
                    AWS_LOGSTREAM_WARN(ALLOC_TAG, "Failed to set UDP receive buffer size to " << receiveBufSize << " for socket " << sock << " error message: " << strerror(errno));
                }
                assert(ret == 0);
            }

            SetUnderlyingSocket(sock);
        }

        int SimpleUDP::Connect(const sockaddr* address, size_t addressLength)
        {
            int ret = connect(GetUnderlyingSocket(), address, static_cast<socklen_t>(addressLength));
            m_connected = ret ? false : true;
            return ret;
        }

        int SimpleUDP::ConnectToHost(const char* hostIP, unsigned short port) const
        {
            int ret;
            if (m_addressFamily == AF_INET6)
            {
                sockaddr_in6 addrinfo = BuildAddrInfoIPV6(hostIP, port);
                ret = connect(GetUnderlyingSocket(), reinterpret_cast<sockaddr*>(&addrinfo), sizeof(sockaddr_in6));
            }
            else
            {
                sockaddr_in addrinfo = BuildAddrInfoIPV4(hostIP, port);
                ret = connect(GetUnderlyingSocket(), reinterpret_cast<sockaddr*>(&addrinfo), sizeof(sockaddr_in));
            }
            m_connected = ret ? false : true;
            return ret;
        }

        int SimpleUDP::ConnectToLocalHost(unsigned short port) const
        {
            if (m_addressFamily == AF_INET6)
            {
                return ConnectToHost(IPV6_LOOP_BACK_ADDRESS, port);
            }
            else
            {
                return ConnectToHost(IPV4_LOOP_BACK_ADDRESS, port);
            }
        }

        int SimpleUDP::Bind(const sockaddr* address, size_t addressLength) const
        {
            return bind(GetUnderlyingSocket(), address, static_cast<socklen_t>(addressLength));
        }

        int SimpleUDP::BindToLocalHost(unsigned short port) const
        {
            if (m_addressFamily == AF_INET6)
            {
                sockaddr_in6 addrinfo = BuildAddrInfoIPV6(IPV6_LOOP_BACK_ADDRESS, port);
                return bind(GetUnderlyingSocket(), reinterpret_cast<sockaddr*>(&addrinfo), sizeof(sockaddr_in6));
            }
            else
            {
                sockaddr_in addrinfo = BuildAddrInfoIPV4(IPV4_LOOP_BACK_ADDRESS, port);
                return bind(GetUnderlyingSocket(), reinterpret_cast<sockaddr*>(&addrinfo), sizeof(sockaddr_in));
            }
        }

        int SimpleUDP::SendData(const uint8_t* data, size_t dataLen) const
        {
            if (!m_connected)
            {
                ConnectToHost(m_hostIP.c_str(), m_port);
            }
            return send(GetUnderlyingSocket(), data, dataLen, 0);
        }

        int SimpleUDP::SendDataTo(const sockaddr* address, size_t addressLength, const uint8_t* data, size_t dataLen) const
        {
            if (m_connected)
            {
                return send(GetUnderlyingSocket(), data, dataLen, 0);
            }
            else
            {
                return sendto(GetUnderlyingSocket(), data, dataLen, 0, address, static_cast<socklen_t>(addressLength));
            }
        }

        int SimpleUDP::SendDataToLocalHost(const uint8_t* data, size_t dataLen, unsigned short port) const
        {
            if (m_connected)
            {
                return send(GetUnderlyingSocket(), data, dataLen, 0);
            }
            else if (m_addressFamily == AF_INET6)
            {
                sockaddr_in6 addrinfo = BuildAddrInfoIPV6(IPV6_LOOP_BACK_ADDRESS, port);
                return sendto(GetUnderlyingSocket(), data, dataLen, 0, reinterpret_cast<sockaddr*>(&addrinfo), sizeof(sockaddr_in6));
            }
            else
            {
                sockaddr_in addrinfo = BuildAddrInfoIPV4(IPV4_LOOP_BACK_ADDRESS, port);
                return sendto(GetUnderlyingSocket(), data, dataLen, 0, reinterpret_cast<sockaddr*>(&addrinfo), sizeof(sockaddr_in));
            }
        }

        int SimpleUDP::ReceiveData(uint8_t* buffer, size_t bufferLen) const
        {
            return recv(GetUnderlyingSocket(), buffer, static_cast<int>(bufferLen), 0);
        }


        int SimpleUDP::ReceiveDataFrom(sockaddr* address, size_t* addressLength, uint8_t* buffer, size_t bufferLen) const
        {
            return recvfrom(GetUnderlyingSocket(), buffer, static_cast<int>(bufferLen), 0, address, reinterpret_cast<socklen_t*>(addressLength));
        }
    }
}
