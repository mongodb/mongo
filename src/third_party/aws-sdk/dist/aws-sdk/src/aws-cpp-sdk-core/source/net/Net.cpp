/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/net/Net.h>

namespace Aws
{
    namespace Net
    {
        // For Posix system, currently we don't need to do anything for network stack initialization.
        // But we need to do initialization for WinSock on Windows and call them in Aws.cpp. So these functions
        // also exist for Posix systems.
        bool IsNetworkInitiated() 
        {
            return true;
        }

        void InitNetwork()
        {
        }

        void CleanupNetwork()
        {
        }
    }
}
