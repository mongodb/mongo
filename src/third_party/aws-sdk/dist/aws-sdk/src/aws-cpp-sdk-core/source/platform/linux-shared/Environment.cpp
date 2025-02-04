/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/platform/Environment.h>

//#include <aws/core/utils/memory/stl/AWSStringStream.h>
//#include <sys/utsname.h>

namespace Aws
{
namespace Environment
{

Aws::String GetEnv(const char* variableName)
{
    auto variableValue = std::getenv(variableName);
    return Aws::String( variableValue ? variableValue : "" );
}

} // namespace Environment
} // namespace Aws
