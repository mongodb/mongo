/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>

namespace Aws
{
    namespace Net
    {
        // Has network stack been initiated.
        bool IsNetworkInitiated();

        // Initiate network stack.
        void InitNetwork();

        // Cleanup network stack.
        void CleanupNetwork();
    }
}
