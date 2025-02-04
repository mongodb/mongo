/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <cstddef>
#include <aws/core/utils/UnreferencedParam.h>
#include <aws/core/net/SimpleUDP.h>

namespace Aws
{
    namespace Net
    {
        SimpleUDP::SimpleUDP(int, size_t, size_t, bool)
        {
        }

        SimpleUDP::SimpleUDP(bool, size_t, size_t, bool)
        {
        }

        SimpleUDP::SimpleUDP(const char*, unsigned short, size_t, size_t, bool)
        {
            //prevent compiler warning for unused private variables
            m_port = 0;
        }

        SimpleUDP::~SimpleUDP()
        {
        }

        void SimpleUDP::CreateSocket(int, size_t, size_t, bool)
        {
        }

        int SimpleUDP::Connect(const sockaddr*, size_t)
        {
            return -1;
        }

        int SimpleUDP::ConnectToHost(const char*, unsigned short) const
        {
            return -1;
        }

        int SimpleUDP::ConnectToLocalHost(unsigned short) const
        {
            return -1;
        }

        int SimpleUDP::Bind(const sockaddr*, size_t) const
        {
            return -1;
        }

        int SimpleUDP::BindToLocalHost(unsigned short) const
        {
            return -1;
        }

        int SimpleUDP::SendData(const uint8_t*, size_t) const
        {
            return -1;
        }

        int SimpleUDP::SendDataTo(const sockaddr*, size_t, const uint8_t*, size_t) const
        {
            return -1;
        }

        int SimpleUDP::SendDataToLocalHost(const uint8_t*, size_t, unsigned short) const
        {
            return -1;
        }

        int SimpleUDP::ReceiveData(uint8_t*, size_t) const
        {
            return -1;
        }

        int SimpleUDP::ReceiveDataFrom(sockaddr*, size_t*, uint8_t*, size_t) const
        {
            return -1;
        }
    }
}
