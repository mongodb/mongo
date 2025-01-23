/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/client/AsyncCallerContext.h>
#include <aws/core/utils/UUID.h>

namespace Aws
{
    namespace Client
    {
        AsyncCallerContext::AsyncCallerContext() : m_uuid(Aws::Utils::UUID::PseudoRandomUUID())
        {}
    }
}